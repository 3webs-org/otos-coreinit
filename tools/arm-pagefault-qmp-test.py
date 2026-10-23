#!/usr/bin/env python3
"""QEMU virt judge for AP-local ARM STREAM/COW, independent of serial interleaving.

Build saski with ARCH=aarch64 and this checkout's current ARM coreinit Mach-O.
Set KERNEL_IMAGE for QEMU -kernel or BOOT_DISK for AAVMF → GRUB xnu_kernel64;
QEMU points to the local qemu-system-aarch64 executable.
The guest writes known values to two allocated resource pages. QMP reads the
physical original pages and AP1's private COW pool after the AP fault handler
has resolved one stream fault and one write fault. No guest debug syscall,
serial formatting, or kernel instrumentation is required.
"""

import json
import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INIT = Path(os.environ.get("INIT_MACHO", ROOT / "build/coreinit.macho"))
SASKI = Path(os.environ.get("SASKI_ROOT", ROOT.parent / "saski"))
KERNEL = Path(os.environ.get("KERNEL_IMAGE", SASKI / "build/saski.bin"))
BOOT_DISK = os.environ.get("BOOT_DISK")
AAVMF = SASKI / "build/arm-deps/qemu-efi-aarch64/usr/share/AAVMF"
QEMU = os.environ.get("QEMU") or shutil.which("qemu-system-aarch64")
NM = os.environ.get("NM") or subprocess.check_output(["xcrun", "-f", "nm"], text=True).strip()
GIC_VERSION = os.environ.get("GIC_VERSION", "2")
LOG = Path(os.environ.get("LOG", ROOT / "build/arm-pagefault-judge.log"))

if not QEMU or GIC_VERSION not in ("2", "3"):
    raise SystemExit("set QEMU and GIC_VERSION=2|3")

symbols = {}
for line in subprocess.check_output([NM, str(INIT)], text=True).splitlines():
    parts = line.split()
    if len(parts) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
        symbols[parts[-1]] = int(parts[0], 16)
for name in ("_g_cow_copies", "_g_fault_count", "_g_cow_pool", "_g_split_base"):
    if name not in symbols:
        raise SystemExit(f"ARM coreinit symbol missing: {name}")

# AP1 region starts 32 MiB into QEMU virt RAM; its independent init image
# occupies the +1 MiB slot. These constants are asserted by arm-layout-test.c.
slide = (0x42000000 + 0x100000) - 0x40200000
LOG.parent.mkdir(parents=True, exist_ok=True)
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
machine = f"virt,gic-version={GIC_VERSION}{os.environ.get('QEMU_MACHINE_OPTS', '')}"
command = [QEMU, "-machine", machine, "-cpu", "cortex-a57", "-m", "512",
           "-smp", "4", "-display", "none", "-serial", f"file:{LOG}",
           "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-no-reboot"]
boot_temp = None
if BOOT_DISK:
    boot_temp = tempfile.TemporaryDirectory(prefix="arm-cow-grub-")
    vars_file = Path(boot_temp.name) / "vars.fd"
    disk_file = Path(boot_temp.name) / "disk.img"
    shutil.copyfile(os.environ.get("AAVMF_VARS", AAVMF / "AAVMF_VARS.fd"), vars_file)
    shutil.copyfile(BOOT_DISK, disk_file)
    code = os.environ.get("AAVMF_CODE", AAVMF / "AAVMF_CODE.no-secboot.fd")
    command.extend(["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,file={vars_file}",
                    "-drive", f"file={disk_file},format=raw,if=virtio"])
else:
    command.extend(["-kernel", str(KERNEL)])
process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + int(os.environ.get("TIMEOUT_S", "90"))
    sock = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if sock is None:
        raise RuntimeError("QMP unavailable")
    with sock, tempfile.TemporaryDirectory(prefix="arm-cow-qmp-") as temp:
        reader = sock.makefile("rwb")
        reader.readline()  # QMP version banner

        def qmp(request):
            reader.write((json.dumps(request) + "\n").encode())
            reader.flush()
            while True:
                line = reader.readline()
                if not line:
                    raise RuntimeError("QMP disconnected")
                answer = json.loads(line)
                if "error" in answer:
                    raise RuntimeError(answer["error"])
                if "return" in answer:
                    return answer["return"]

        qmp({"execute": "qmp_capabilities"})
        serial = 0

        def physical_word(address):
            global serial
            serial += 1
            path = str(Path(temp) / f"memory-{serial}.bin")
            qmp({"execute": "pmemsave", "arguments": {
                "val": address, "size": 8, "filename": path,
            }})
            return struct.unpack("<Q", Path(path).read_bytes())[0]

        while time.monotonic() < deadline:
            copies = physical_word(symbols["_g_cow_copies"] + slide)
            faults = physical_word(symbols["_g_fault_count"] + slide)
            if copies >= 1 and faults >= 2:
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("AP1 never resolved STREAM and COW faults")

        private = physical_word(symbols["_g_cow_pool"] + slide)
        original = physical_word(0x54001000)
        streamed = physical_word(0x54000000)
        split = physical_word(symbols["_g_split_base"] + slide)
        print(f"AP1 faults={faults}, COW copies={copies}, private={private:#x}, "
              f"original={original:#x}, stream={streamed:#x}, split={split:#x}")
        serial_text = LOG.read_text(errors="ignore")
        if (copies != 1 or private != 0x5A5A5A5A or
                original != 0xF00DFACE or streamed != 0xCAFEBABE or
                split != 0x54000000 or "UNHANDLED ARM exception" in serial_text or
                (BOOT_DISK and "[grubcfg] kernel loaded, booting" not in serial_text)):
            raise RuntimeError(f"COW/STREAM state mismatch; see {LOG}")
        mode = "GRUB xnu_kernel64" if BOOT_DISK else "-kernel"
        print(f"ARM QEMU GICv{GIC_VERSION} {mode} STREAM/COW: PASS")
finally:
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
    if boot_temp is not None:
        boot_temp.cleanup()
