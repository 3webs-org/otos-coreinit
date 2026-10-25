#!/usr/bin/env python3
"""Wrap a raw aarch64 code blob in the smallest Mach-O an arm64 XNU loader
accepts: MH_EXECUTE, one __TEXT segment at the arm64 link base, and an
LC_UNIXTHREAD carrying ARM_THREAD_STATE64 -- the same entry convention an
arm64 XNU kernel uses, and the one loader/arm64/xnu.c reads (pc after
x0..x28, fp, lr, sp).

usage: mkmacho.py <code.bin> <out.macho>"""
import struct, sys

BASE = 0x40200000          # coreinit's aarch64 PAGEZERO / link base (see CMakeLists.txt)
CODE_OFF = 0x1000
SIZE = 0x2000

code = open(sys.argv[1], 'rb').read()
assert len(code) <= SIZE - CODE_OFF
seg = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT',
                  BASE, SIZE, 0, SIZE, 5, 5, 0, 0)
state = struct.pack('<29QQQQQII', *([0] * 29), 0, 0,
                    BASE + SIZE, BASE + CODE_OFF, 0x3c5, 0)
thread = struct.pack('<IIII', 5, 16 + len(state), 6, len(state) // 4) + state
cmds = seg + thread
hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 2,
                  len(cmds), 1, 0)
img = bytearray(SIZE)
img[:len(hdr + cmds)] = hdr + cmds
img[CODE_OFF:CODE_OFF + len(code)] = code
open(sys.argv[2], 'wb').write(img)
