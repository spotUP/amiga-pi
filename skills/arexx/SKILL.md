---
name: arexx
description: ARexx on the Amiga, covering running scripts with rx, RexxMast, message ports, ADDRESS and return codes. Use when writing or running ARexx (.rexx) programs or controlling an Amiga application through its ARexx port.
---

# ARexx

Checked on Kickstart 40.63 / Workbench 40.42 (AmigaOS 3.1).

## Running
- ARexx needs the resident server RexxMast (`SYS:System/RexxMast`) running. When it is, the port `REXX`
  appears in `show('P')`.
- The programs are in `SYS:Rexxc` (on the Shell path of a normal Workbench): `rx`, `RXC`, `RXSET`, `HI`, `TS`, `TE`,
  `TCO`, `TCC`, `WaitForPort`, `RXLIB`. If `rx` is "Unknown command", call `SYS:Rexxc/rx`.
- `rx "say 1+1"` runs a one-line program (prints 2). `rx file.rexx one two` runs a file. `rx name` finds
  `name.rexx` in the current directory, then in `REXX:` (checked: `rx r2z` ran `REXX:r2z.rexx`).
- `REXX:` is an assign, `S:` on a normal Workbench.
- `RXSET` with no argument prints `Usage: RXSET [NAME [[=] VALUE]]`.

## Writing
- A program starts with a comment (`/* ... */` as its first characters); otherwise it is not ARexx.
- `parse arg a b` reads the arguments; `say` prints; `exit n` returns a code; `~=` is not equal;
  strings use `'` or `"`; `||` joins strings.
- `address command 'Echo hi'` runs an AmigaDOS command; its return code is in `rc`
  (`address command 'Delete nosuch QUIET'` leaves `rc = 5`). The command's own text output goes to the
  console, not into a variable.
- `options results` before a command asks the port to return a result string in `result`.
- `show('P')` lists the open ports (checked: `DOPUS.1 Directory Opus REXX AREXX AMIAGENT ...`); test one
  with `show('P','NAME')`. To talk to an application: `address NAME 'command'` sends one command to the port NAME. The commands an application accepts are listed in
  its own documentation; do not guess them.
- Wait for a program's port from the Shell with `SYS:Rexxc/WaitForPort NAME` (return code 5 when it does
  not appear in time).
- Functions that are not built in come from libraries. rexxsupport.library holds ALLOCMEM, BADDR, CLOSEPORT,
  DELAY, DELETE, FORBID, FORWARD, FREEMEM, GETARG, GETPKT, MAKEDIR, NEXT, NULL, OFFSET, OPENPORT, PERMIT,
  RENAME, REPLY, SHOWDIR, SHOWLIST, STATEF, TYPEPKT. Load it once with
  `if ~show('L','rexxsupport.library') then call addlib('rexxsupport.library',0,-30,0)` (returns 1 on success);
  then `call delay(50)` works. Details: `rexxsupport.doc`, `rexxsyslib.doc`, `arexx_cl.doc` (amiga-autodocs skill).

## Pitfalls
- A script that waits for a port that never opens hangs; stop its process with `Break <n> ALL` (see `Status`).
- `rc` holds the return code of the last command as a number: `if rc > 0 then ...`.
