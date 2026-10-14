---
name: amigados
description: AmigaDOS Shell commands, scripts, assigns, protection bits and redirection. Use when writing or running AmigaDOS commands or Execute scripts on the Amiga, or when a command line behaves differently from a Unix shell.
---

# AmigaDOS

Checked on Kickstart 40.63 / Workbench 40.42 (AmigaOS 3.1). AmigaOS 3.2 commands take the same arguments and add options.

## Find a command's arguments
`Command ?` prints its template, e.g. `Protect ?` gives `FILE/A,FLAGS,ADD/S,SUB/S,ALL/S,QUIET/S`.
/A required, /K keyword must be typed, /S switch, /M several, /N number, /F rest of line.
Other templates: `Copy FROM/M,TO/A,ALL/S,QUIET/S,BUF=BUFFER/K/N,CLONE/S,DATES/S,NOPRO/S,COM/S,NOREQ/S`,
`Delete FILE/M/A,ALL/S,QUIET/S,FORCE/S`, `Rename FROM/A/M,TO=AS/A,QUIET/S`,
`Search FROM/M,SEARCH/A,ALL/S,NONUM/S,QUIET/S,QUICK/S,FILE/S,PATTERN/S`, `Which FILE/A,NORES/S,RES/S,ALL/S`,
`Type FROM/A/M,TO/K,OPT/K,HEX/S,NUMBER/S`, `Eval VALUE1/A,OP,VALUE2/M,TO/K,LFORMAT/K`.

## Paths and patterns
`Volume:dir/file`; a leading `/` is the parent directory. Wildcards: `#?` any text, `?` one character,
`~(pattern)` not. `*` is NOT a wildcard. Example: `List RAM:#?.s#?`.

## Redirection
Works: `>file`, `>>file` (append), `<file`, `>NIL:`. The Shell has no `|` pipe and no `2>`, `2>&1` (checked:
`Echo a 2>&1` and `Echo a | Type` print their text, they redirect nothing).
`*>` (error output) is NOT a redirect on the 3.1 Shell: `Echo a *>NIL:` prints `a *>NIL:`. The AmigaOS 3.2
NDK release notes (dos 47.3) say error output can be redirected with `*>`; test it on the target first.
Inside a quoted string `*` is the escape character: `*"` quote, `*N` newline, `**` an asterisk.
`Echo "a*"b*Nc"` prints `a"b` and `c`. Write `**` when you mean one `*` inside quotes.
`Run >NIL: Command` starts a background process; `Status` lists processes; `Break <n> ALL` stops one
(`Break <n> C` did not stop an Execute script).

## Scripts (Execute file args)
```
.key name/A,count/K        ; argument template; /N is not accepted here
.bra {                     ; {name} substitutes an argument
.ket }
Echo "name={name} count={count$0}"    ; {arg$default} gives a default
Set n 0
Lab again
Set n `Eval $n + 1`        ; backticks substitute a command's output
If NOT $n GE 3
  Skip again BACK          ; Skip label [BACK]; labels are  Lab name
EndIf
If EXISTS RAM:file
  Echo "there"
Else
  Echo "missing"
EndIf
Quit 3                     ; end the script with return code 3
```
`If` takes `[NOT] [WARN|ERROR|FAIL] [A EQ B] [A GT B] [A GE B] [VAL] [EXISTS file]`. There is no LT or LE:
use `If NOT $n GE 3`. `EQ` compares strings; add `VAL` to compare numbers.
A command sets `$RC` and `$Result2`. `FailAt n` sets the level at which the script stops (default 10):
`Delete nosuchfile` returns 5, the script goes on; a return code of 10 or more ends it.
Run a script by its name only when its script bit is set: `Protect file +s` (else `file is not executable`).
`Set` is local to the Shell, `SetEnv name value` is global (ENV:); to keep it, `Copy ENV:name ENVARC:name`.

## Protection bits
`Protect file FLAGS=<letters> [ADD|SUB]`, `+s` / `-s` also work. Letters h s p a r w e d: hidden, script,
pure, archive, read, write, execute, delete. `List` shows them in that order: `-s--rwed`.
`Protect file FLAGS=e SUB` shows `----rw-d`.

## Assigns
`Assign NAME: path` creates, `Assign NAME: path ADD`, `Assign NAME: REMOVE`, `Assign LIST` shows them.
`Assign NAME: EXISTS` returns 5 and prints `NAME: not assigned` when it is missing. Use it to test for a
volume or assign: `If EXISTS Vol:file` on a missing volume can raise a "Please insert volume" requester.
