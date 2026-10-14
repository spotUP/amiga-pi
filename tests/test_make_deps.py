#!/usr/bin/env python3
"""build/pi_test is rebuilt when a vtcon file it compiles or includes changes, and when VTCON_DIR points
at another tree (before: only the sources in sources.mk and claude/ + llm/ headers were prerequisites, so a
`fails on old code` check could pass on a stale binary). Same method as vtcon's tests/test_host_deps.py.

The sources are read from the link command (`make -n -B`), the headers they include are followed (quoted
#include, transitively, through the -I roots of HOSTCFLAGS), and for each source and header
`make -n -W <file> build/pi_test` must want to rebuild. Then `make -n VTCON_DIR=<another tree>` must too.
Run after build/pi_test is built (make test builds it first)."""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGETS = ['build/pi_test', 'build/amiga-pi-host']  # per-file checks: the first only (same prerequisites)
STAMP = os.path.join(ROOT, 'build', 'vtcon_dir.stamp')


def make(*args):
    """the commands make -n would run (its notes dropped)"""
    out = subprocess.run(['make', '-n'] + list(args), cwd=ROOT, capture_output=True, text=True).stdout
    return '\n'.join(l for l in out.splitlines() if not re.match(r"make(\[\d+\])?: ", l))


def vtcon_dir():
    """the resolved VTCON_DIR, as the makefile defaults it"""
    root = os.environ.get('UPTERM_ROOT') or os.path.dirname(ROOT)
    return os.path.realpath(os.environ.get('VTCON_DIR') or os.path.join(root, 'vtcon'))


def includes(path, roots, seen):
    try:
        text = open(path, encoding='latin-1').read()
    except OSError:
        return
    for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
        for base in [os.path.dirname(path)] + roots:
            h = os.path.normpath(os.path.join(base, inc))
            if os.path.exists(h):
                if h not in seen:
                    seen.add(h)
                    includes(h, roots, seen)
                break


def restore(saved):
    if saved:
        open(STAMP, 'w').write(saved[0])
        os.utime(STAMP, ns=(saved[1], saved[1]))


def main():
    fails = 0
    vt = vtcon_dir()
    for t in TARGETS[:1]:
        if make(t).strip():
            print('[INFO] %s is out of date: build it first' % t)
            continue
        cmd = make('-B', t)
        words = cmd.split()
        srcs = {os.path.normpath(os.path.join(ROOT, w)) for w in words if w.endswith('.c') and os.path.exists(os.path.join(ROOT, w))}
        roots = [os.path.normpath(os.path.join(ROOT, w[2:])) for w in words if w.startswith('-I')]
        hdrs = set()
        for s in srcs:
            includes(s, roots, hdrs)
        n_vt = sum(1 for f in srcs | hdrs if f.startswith(vt + os.sep))
        if not n_vt:
            print('[FAIL] %s: found no vtcon file among its sources (the check itself is broken)' % t)
            fails += 1
            continue
        stale = [f for f in sorted(srcs | hdrs) if f.startswith(vt + os.sep) and not make('-W', f, t).strip()]
        for f in stale:
            print('[FAIL] %s does not rebuild when %s changes' % (t, os.path.relpath(f, ROOT)))
        fails += len(stale)
        if not stale:
            print('[OK] %s: rebuilt for each of its %d vtcon sources and headers' % (t, n_vt))

    # VTCON_DIR at another tree: a directory of links to the entries of this one has another resolved path
    other = tempfile.mkdtemp(prefix='vtcon-other-')
    saved = (open(STAMP).read(), os.stat(STAMP).st_mtime_ns) if os.path.exists(STAMP) else None
    try:
        for e in os.listdir(vt):
            os.symlink(os.path.join(vt, e), os.path.join(other, e))
        for t in TARGETS:
            if make(t).strip():
                continue
            wants = make('VTCON_DIR=' + other, t).strip()
            restore(saved)  # parsing with the other path rewrote the stamp: put back its text and age
            if wants:
                print('[OK] %s: rebuilt when VTCON_DIR changes' % t)
            else:
                print('[FAIL] %s does not rebuild when VTCON_DIR changes' % t)
                fails += 1
    finally:
        shutil.rmtree(other, ignore_errors=True)
        restore(saved)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
