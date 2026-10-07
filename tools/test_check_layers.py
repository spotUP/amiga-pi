#!/usr/bin/env python3
"""check_layers.py exits 0 on a clean tree and 1 on a planted include."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHK = os.path.join(HERE, "check_layers.py")


def run(root):
    return subprocess.run([sys.executable, CHK, "--root", root, "--vtcon", "/nonexistent"],
                          capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, "llm"))
        os.makedirs(os.path.join(t, "amiga-pi"))
        with open(os.path.join(t, "llm", "be_x.c"), "w") as f:
            f.write('#include "llm.h"\n#include <string.h>\n')
        with open(os.path.join(t, "amiga-pi", "ui_x.c"), "w") as f:
            f.write('#include "agent.h"\n')
        r = run(t)
        assert r.returncode == 0, "clean tree must pass: " + r.stdout
        with open(os.path.join(t, "llm", "be_x.c"), "a") as f:
            f.write('#include <proto/dos.h>\n')
        r = run(t)
        assert r.returncode == 1 and "llm/be_x.c:3" in r.stdout, "planted AmigaDOS include: " + r.stdout
        with open(os.path.join(t, "amiga-pi", "ui_x.c"), "a") as f:
            f.write('#include "../claude/net.h"\n')
        r = run(t)
        assert "amiga-pi/ui_x.c:2" in r.stdout, "planted net.h include: " + r.stdout
    print("test_check_layers: ok")


main()
