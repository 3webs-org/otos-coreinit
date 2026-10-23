#!/usr/bin/env python3
"""QEMU judge: wrapper-owned CPL3 page denial and INT80 interception.

BOOT_DISK is a GRUB xnu FAT image containing the current x86 saski Mach-O.
The wrapper denies Admin via hardware #PF, rejects an ungranted endpoint,
intercepts the one INT80 path, then INT3 returns to its trusted ring-0 stack.
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
QEMU = os.environ.get("QEMU") or shutil.which("qemu-system-x86_64")
NM = os.environ.get("NM") or subprocess.check_output(["xcrun", "-f", "nm"], text=True).strip()
IMAGE = Path(os.environ.get("SECURINIT_IMAGE", ROOT / "build/securinit.macho"))
DISK = os.environ.get("BOOT_DISK")
LOG = Path(os.environ.get("LOG", ROOT / "build/x86-ring3-wrapper.log"))
TRACE = LOG.with_suffix(".int.log")
if not QEMU or not DISK:
    raise SystemExit("set QEMU and BOOT_DISK to the prepared GRUB xnu disk")

symbols = {}
for line in subprocess.check_output([NM, str(IMAGE)], text=True).splitlines():
    fields = line.split()
    if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
        symbols[fields[-1]] = int(fields[0], 16)
if "_g_admin_record" not in symbols:
    raise SystemExit("securinit x86 image lacks g_admin_record")

LOG.parent.mkdir(parents=True, exist_ok=True)
firmware = SASKI / "build/qemu-deps/ovmf/usr/share/OVMF"
with tempfile.TemporaryDirectory(prefix="ring3-grub-") as temp:
    vars_file = Path(temp) / "vars.fd"
    disk_file = Path(temp) / "disk.img"
    shutil.copyfile(os.environ.get("OVMF_VARS", firmware / "OVMF_VARS_4M.fd"), vars_file)
    shutil.copyfile(DISK, disk_file)
    code = os.environ.get("OVMF_CODE", firmware / "OVMF_CODE_4M.fd")
    command = [QEMU, "-machine", "q35", "-cpu", "max", "-m", "512",
               "-smp", "1", "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
               "-drive", f"if=pflash,format=raw,file={vars_file}",
               "-drive", f"file={disk_file},format=raw,if=ide,index=0,media=disk",
               "-display", "none", "-serial", f"file:{LOG}",
               "-monitor", "none", "-no-reboot", "-d", "int", "-D", str(TRACE)]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + int(os.environ.get("TIMEOUT_S", "70"))
        while time.monotonic() < deadline:
            text = LOG.read_text(errors="ignore") if LOG.exists() else ""
            if ("ring3 hardware denied access to Admin" in text and
                    "Gate denied 1 request(s), forwarded 1" in text):
                break
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before ring3 proof")
            time.sleep(0.2)
        else:
            raise RuntimeError(f"ring3 proof timed out; see {LOG}")
        trace = TRACE.read_text(errors="ignore")
        base = re.search(r"securinit image at 0x([0-9A-Fa-f]+)", text)
        if not base:
            raise RuntimeError("stage image address missing")
        admin = int(base.group(1), 16) + symbols["_g_admin_record"]
        user_pf = re.search(
            r"v=0e e=0005.*cpl=3 IP=0033:([0-9a-fA-F]+).*CR2=([0-9a-fA-F]+)",
            trace,
        )
        cr2 = int(user_pf.group(2), 16) if user_pf else 0
        if (cr2 != admin or trace.count("v=80 e=0000") < 2 or
                "v=03 e=0000" not in trace or
                "GENERAL PROTECTION FAULT" in text or
                "DOUBLE FAULT" in text or "UNHANDLED PAGE FAULT" in text or
                "[grubcfg] kernel loaded, booting" not in text):
            raise RuntimeError(f"ring3 denial mismatch CR2={cr2:#x}, "
                               f"Admin={admin:#x}; see {TRACE}")
        print(f"x86 wrapper-owned ring3 denial: PASS "
              f"(Admin CR2={cr2:#x}, 2×INT80, INT3→ring0)")
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
