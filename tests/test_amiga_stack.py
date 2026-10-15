#!/usr/bin/env python3
"""amiga-pi -p ended in Guru #80000008 / #80000004 (VTC:amiga-pi and the RAM handler, two Software Failures at once)
on the rig: with the 32768-byte stack of its $STACK cookie, or the Shell's default, the run overflowed the stack and
wrote into the memory below it. The same run under `Stack 262144` ended with rc 0 (rig 2, 2026-10-15).

The fix: vbcc's startup (vc.lib) reads `long __stack` and runs main on a stack of that size when the caller's is
smaller (install/upupdate.c in vtcon does the same), so amiga-pi no longer depends on the Stack the Shell, a script or
Workbench gives it. This test pins it: main_amiga.c declares __stack of at least MIN bytes, and the $STACK cookie (what
Workbench and the Shell read) says the same size."""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, 'amiga-pi', 'main_amiga.c')
MIN = 131072    # four times the 32 KB that overflowed; 256 KB passed on the rig


def main():
    src = open(MAIN).read()
    fails = []
    m = re.search(r'^long __stack = (\d+);', src, re.M)
    if not m:
        fails.append('amiga-pi -p overflows a 32 KB stack: main_amiga.c declares no `long __stack = N;` for vbcc\'s startup')
    elif int(m.group(1)) < MIN:
        fails.append('__stack is %s, below %d' % (m.group(1), MIN))
    c = re.search(r'\$STACK: (\d+)', src)
    if m and (not c or c.group(1) != m.group(1)):
        fails.append('the $STACK cookie (%s) differs from __stack (%s)' % (c and c.group(1), m.group(1)))
    for f in fails:
        print('[FAIL] ' + f)
    print('amiga_stack: %d failed' % len(fails))
    sys.exit(1 if fails else 0)


main()
