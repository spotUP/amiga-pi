---
name: amiga-autodocs
description: Look up an Amiga library function, device or class in the user's own NDK autodocs (dos.doc, exec.doc, intuition.doc ...) and headers, on demand. Use when you need the exact arguments or behaviour of an Amiga system call.
---

# Amiga autodocs

The autodocs are NOT shipped with amiga-pi (Hyperion's licence). They exist only if the user installed the
Amiga NDK. Never guess a function's arguments from memory when the NDK is there; look it up.

## Where the NDK is
1. The convention: an assign `NDK:` pointing at the NDK drawer (the one holding `Autodocs`, `Include_H`,
   `FD`, `SFD`). Test with `Assign NDK: EXISTS` (return code 0 when it exists, 5 when not).
   The files are then `NDK:Autodocs/dos.doc`, `NDK:Include_H/proto/dos.h`, `NDK:FD/dos_lib.fd`.
2. If `NDK:` is missing, look for an `Autodocs` drawer on the user's disks before giving up (the NDK 3.2R4
   drawer name is `NDK3.2R4`, with `Autodocs` inside), and tell the user the assign to add:
   `Assign NDK: <drawer>` (in `S:User-Startup` to keep it).
3. No NDK at all: say so, and answer from what you know, marked as unchecked.

## How to look a function up (never load a whole file)
Each library has one file, `Autodocs/<library>.doc` (dos.doc, exec.doc, intuition.doc, graphics.doc,
rexxsyslib.doc, rexxsupport.doc, ... about 110 files in NDK 3.2R4). The first lines are a table of contents; each
entry later in the file starts with a form-feed line `library/Function   library/Function`.
1. Find the line: grep for `dos.library/Execute +dos.library/Execute` in `NDK:Autodocs/dos.doc`, with line
   numbers (the table of contents holds a bare `dos.library/Execute` and does not match). Example result:
   `1866:dos.library/Execute ...`.
2. Read about 60 lines from that line (the read tool with offset and limit). An entry has NAME, SYNOPSIS,
   FUNCTION, INPUTS, RESULT, NOTES, SEE ALSO.
3. For the C prototype and the register layout: grep the function name in `NDK:Include_H/clib/dos_protos.h`
   and `NDK:FD/dos_lib.fd`. Structures and tags: `NDK:Include_H/<dir>/<name>.h` (e.g. `dos/dostags.h`).
4. To find which library has a function when you only know the name: grep the name across
   `NDK:Autodocs/#?.doc` with `-l`.
Do not read an autodoc file whole: dos.doc alone is 6400 lines.
