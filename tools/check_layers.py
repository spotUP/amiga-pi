#!/usr/bin/env python3
"""A4 layering check: UI -> agent -> LLM -> HTTP -> TLS -> bsdsocket.

Greps #include lines per directory against the rule in ARCHITECTURE.md and
prints offending file:line.  Exit 0 = clean, 1 = violation.

  check_layers.py [--root DIR] [--vtcon DIR] [--strict]

The rule about claude/http.c (no provider name) lives in the vtcon repo, which
this repo only reads.  It is violated until plan item B5 lands (the Anthropic
head is still hard-coded there), so it is reported as PENDING and does not fail
the run unless --strict is given.
"""
import argparse
import os
import re
import sys

INC = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
AMIGA_DIRS = ("proto/", "dos/", "exec/", "clib/", "intuition/", "devices/", "utility/")

# (name, directory, filename regex, forbidden include test, message)
RULES = [
    ("llm-no-ui", "llm", r".*\.[ch]$",
     lambda h: os.path.basename(h) in ("ui.h", "tui.h", "sys.h"),
     "llm/ includes no ui.h/tui.h/sys.h"),
    ("backend-no-amigados", "llm", r"be_.*\.[ch]$",
     lambda h: os.path.basename(h) == "sys.h" or h.startswith(AMIGA_DIRS),
     "llm/be_*.c include no AmigaDOS headers or sys.h"),
    ("ui-no-transport", "amiga-pi", r"(ui|tui)[_a-z0-9]*\.[ch]$",
     lambda h: os.path.basename(h) in ("net.h", "tls.h"),
     "amiga-pi/ UI files include no net.h/tls.h"),
]
PROVIDER = re.compile(r"anthropic|openai|x-api-key|api\.google|bedrock|azure", re.I)


def scan(root):
    bad = []
    for name, d, rx, test, msg in RULES:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for fn in sorted(os.listdir(base)):
            if not re.match(rx, fn):
                continue
            path = os.path.join(base, fn)
            with open(path, errors="replace") as f:
                for n, line in enumerate(f, 1):
                    m = INC.match(line)
                    if m and test(m.group(1)):
                        bad.append((name, "%s:%d" % (os.path.relpath(path, root), n), msg))
    return bad


def scan_http(vtcon):
    path = os.path.join(vtcon, "claude", "http.c")
    out = []
    if os.path.isfile(path):
        with open(path, errors="replace") as f:
            for n, line in enumerate(f, 1):
                if PROVIDER.search(line):
                    out.append(("http-no-provider", "claude/http.c:%d" % n,
                                "claude/http.c contains no provider name"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(os.path.dirname(__file__), ".."))
    ap.add_argument("--vtcon", default=os.environ.get("VTCON_DIR", os.path.expanduser("~/Code/vtcon")))
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args()
    bad = scan(a.root)
    for name, where, msg in bad:
        print("VIOLATION %s %s: %s" % (name, where, msg))
    pend = scan_http(a.vtcon)
    for name, where, msg in pend:
        print("%s %s %s: %s" % ("VIOLATION" if a.strict else "PENDING(B5)", name, where, msg))
    if bad or (a.strict and pend):
        return 1
    print("check_layers: ok (%d rule(s) on this repo, %d pending hit(s) in vtcon)" % (len(RULES), len(pend)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
