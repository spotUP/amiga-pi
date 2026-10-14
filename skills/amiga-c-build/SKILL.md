---
name: amiga-c-build
description: Build C and assembler programs for AmigaOS 68k with vbcc, vasm and vlink (cross from a Mac or Linux host), with a worked example, and the gcc alternative. Use when compiling, linking or choosing flags for Amiga programs.
---

# Building 68k Amiga programs

Checked with vbcc (`vc`, `vbccm68k`), `vasmm68k_mot` and `vlink` from Homebrew on a Mac, output a 68k
Amiga hunk executable (the compiler runs on the host, the program runs on the Amiga). Whether a compiler is
installed on the Amiga itself is not covered: look with `Which vc`.

## Setup
`export VBCC=/opt/homebrew/opt/vbcc` (the vbcc prefix). `vc +aos68k` reads `$VBCC/config/aos68k`
(ls `$VBCC/config` shows aos68k, aos68km, aos68kr, kick13, kick13m, kick13r). Its libraries are in
`$VBCC/targets/m68k-amigaos/lib` (vc.lib, amiga.lib, auto.lib, startup.o, m881.lib, ...). The Amiga headers are
not complete there: add the NDK with `-I<NDK>/Include_H` (e.g. NDK 3.2R4 `Include_H`; see amiga-autodocs for
how to find the NDK).

## Worked example
```
vc +aos68k -cpu=68020 -O2 -I$NDK/Include_H hello.c -o hello          # dos.library only
vc +aos68k -cpu=68020 -O2 -I$NDK/Include_H -lauto gui.c -o gui       # other libraries
vc +aos68k -cpu=68020 -O2 -c -o hello.o hello.c                      # object only
vasmm68k_mot -quiet -Fhunk -m68020 -o a.o a.s                        # assembler -> hunk object
vc +aos68k -cpu=68020 -O2 main.c a.o -o main                         # C and assembler together
```
- `#include <proto/dos.h>` plus the NDK include path is enough for dos.library calls (`Printf`, `Open`, ...):
  startup.o opens `DOSBase`. For any other library (intuition, graphics, ...) add `-lauto`, which opens
  the bases for you; without it the link fails with the base undefined (checked with `DisplayBeep`).
- C names get a leading underscore in assembler: C `val` is `_val` there; a `section code,code` source with
  `public` labels is what the example above used.
- `-cpu=68020` is what the examples use (the build here targets 68020 and up); `-O2` optimises. The config passes
  `-amiga-softfloat` (no FPU code).
- `vc -c` makes an object; `vc a.c b.o -o prog` links. `-lamiga` links amiga.lib (helpers such as `NewList`).

## The same recipe in this repository (vtcon/Makefile around line 339)
```
VC := vc +build/vbcc-aos68k.cfg -I<NDK>/Include_H -cpu=68020 -O2 -warn=-1 \
      -dontwarn=163,166,167,168,170,306,307,81 -warnings-as-errors
$(VC) -o prog prog.c            # tests/amiga/busyloop.c is built exactly like this
```
The config it generates (tools/vbcc-aos68k.cfg.in) is the stock one plus the link line
`vlink -bamigahunk -x -Bstatic -Cvbcc -nostdlib -mrel startup.o ... -lvc`, `-hunkdebug` and
`-amiga-softfloat`. Assembler and manual link, as the Makefile does for hand-written sources:
```
vasmm68k_mot -quiet -Fhunk -m68020 -o x.o x.s
vlink -bamigahunk -x -Bstatic -Cvbcc -nostdlib -s -o prog x.o other.o
```
(`-s` strips symbols; leave it off while debugging.) `-warnings-as-errors` is a project choice; drop it
for third-party code.

## gcc
bebbo's cross gcc `m68k-amigaos-gcc` (the repository uses `$(HOME)/opt/amiga/bin/m68k-amigaos-gcc`):
`m68k-amigaos-gcc -mcrt=ixemul -O2 -Wall -o prog prog.c` for programs that use ixemul (needs `ixemul.library`
on the Amiga), `-noixemul` (the same as `-mcrt=nix20`, libnix) for stand-alone ones, `-m68020` for the
CPU. Choose by what the program needs: libnix for plain AmigaDOS programs, ixemul for Unix-style ones.
