#!/usr/bin/env python3
"""QEMU judge: wrapper-owned EL0 isolation AND outgoing IPC mediation.

Use KERNEL_IMAGE=... for -kernel, or BOOT_DISK=... for the unchanged GRUB
xnu_kernel64 config. Checks hardware denial of Admin, two EL0 SVC entries
(one ungranted endpoint rejected, one Gate call forwarded), and BRK→EL1.
"""

import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SASKI = Path(os.environ.get("SASKI_ROOT", ROOT.parent / "saski"))
QEMU = os.environ.get("QEMU") or shutil.which("qemu-system-aarch64")
NM = os.environ.get("NM") or subprocess.check_output(["xcrun", "-f", "nm"], text=True).strip()
IMAGE = Path(os.environ.get("SECURINIT_IMAGE", ROOT / "build/securinit.macho"))
KERNEL = Path(os.environ.get("KERNEL_IMAGE", SASKI / "build/saski.bin"))
DISK = os.environ.get("BOOT_DISK")
GIC = os.environ.get("GIC_VERSION", "2")
LOG = Path(os.environ.get("LOG", ROOT / "build/arm-el0-wrapper.log"))
TRACE = LOG.with_suffix(".int.log")
if not QEMU or GIC not in ("2", "3"):
    raise SystemExit("set QEMU and GIC_VERSION=2|3")

symbols = {}
for line in subprocess.check_output([NM, str(IMAGE)], text=True).splitlines():
    fields = line.split()
    if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
        symbols[fields[-1]] = int(fields[0], 16)
if "_g_admin_record" not in symbols:
    raise SystemExit("securinit ARM image lacks g_admin_record")

LOG.parent.mkdir(parents=True, exist_ok=True)
command = [QEMU, "-machine", f"virt,gic-version={GIC}", "-cpu", "cortex-a57",
           "-m", "512", "-smp", "1", "-display", "none",
           "-serial", f"file:{LOG}", "-monitor", "none", "-no-reboot",
           "-d", "int", "-D", str(TRACE)]
boot_temp = None
if DISK:
    boot_temp = tempfile.TemporaryDirectory(prefix="el0-grub-")
    firmware = SASKI / "build/arm-deps/qemu-efi-aarch64/usr/share/AAVMF"
    vars_file = Path(boot_temp.name) / "vars.fd"
    disk_file = Path(boot_temp.name) / "disk.img"
    shutil.copyfile(os.environ.get("AAVMF_VARS", firmware / "AAVMF_VARS.fd"), vars_file)
    shutil.copyfile(DISK, disk_file)
    code = os.environ.get("AAVMF_CODE", firmware / "AAVMF_CODE.no-secboot.fd")
    command.extend(["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,file={vars_file}",
                    "-drive", f"file={disk_file},format=raw,if=virtio"])
else:
    command.extend(["-kernel", str(KERNEL)])

process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + int(os.environ.get("TIMEOUT_S", "70"))
    while time.monotonic() < deadline:
        text = LOG.read_text(errors="ignore") if LOG.exists() else ""
        if ("EL0 hardware denied access to Admin" in text and
                "EL0 outbound Cap'n Proto IPC traversed wrapper-owned Gate" in text and
                "Gate denied 1 request(s), forwarded 2" in text):
            break
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before the wrapper proof")
        time.sleep(0.2)
    else:
        raise RuntimeError(f"wrapper proof timed out; see {LOG}")
    trace = TRACE.read_text(errors="ignore")
    base = re.search(r"securinit image at 0x([0-9A-Fa-f]+)", text)
    if not base:
        raise RuntimeError("stage image address missing")
    protected = int(base.group(1), 16) + symbols["_g_admin_record"]
    far_match = re.search(r"with ESR 0x24/0x[0-9a-fA-F]+\n\.\.\.with FAR 0x([0-9a-fA-F]+)", trace)
    far = int(far_match.group(1), 16) if far_match else 0
    if (far != protected or "with ELR 0x54801000" not in trace or
            trace.count("with ESR 0x15/") < 2 or
            "with ESR 0x3c/" not in trace or
            "Exception return from AArch64 EL1 to AArch64 EL1 PC" not in trace or
            "UNHANDLED ARM exception" in text or "Synchronous Exception" in text or
            (DISK and "[grubcfg] kernel loaded, booting" not in text)):
        raise RuntimeError(f"EL0 denial/EL1 return mismatch: FAR={far:#x}, "
                           f"Admin={protected:#x}; see {TRACE}")
    print(f"ARM GICv{GIC} wrapper-owned EL0 IPC: PASS "
          f"(Admin FAR={far:#x}, 2×SVC, BRK→EL1)")
finally:
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
    if boot_temp:
        boot_temp.cleanup()
