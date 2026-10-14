#!/bin/bash
# A5: build Lua 5.4 + luaprobe + hello with vbcc, reusing vtcon's vbcc setup
# (tools/vbcc-aos68k.cfg.in; VBCC_PREFIX). Usage: tools/probe/build_lua_probe.sh [extra vc flags]
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
VTCON=${VTCON_DIR:-${UPTERM_ROOT:-$(dirname "$HERE")}/vtcon}
VBCC_PREFIX=${VBCC_PREFIX:-$(brew --prefix vbcc 2>/dev/null || echo /opt/homebrew/opt/vbcc)}
export VBCC=$VBCC_PREFIX
B=$HERE/build; mkdir -p "$B/amiga/obj"
sed -e "s|@VBCC_PREFIX@|$VBCC_PREFIX|g" -e "s|-Ivendor/|-I$VTCON/vendor/|g" \
    "$VTCON/tools/vbcc-aos68k.cfg.in" > "$B/vbcc-aos68k.cfg"
L=$HERE/vendor/lua-5.4
VC="vc +$B/vbcc-aos68k.cfg -cpu=68020 -O2 -warn=-1 ${DONTWARN--dontwarn=65,79,153,163,166,167,170,172,306,307} ${WERROR--warnings-as-errors} -I$VTCON/vendor/ndk-3.2r4-Include_H -I$L ${VC_EXTRA:-}"
LOG=$B/lua_cc.log; : > "$LOG"; fail=0; objs=()
# one log per file (compiled in parallel), joined into $LOG afterwards
rm -f "$B"/amiga/obj/*.o "$B"/amiga/obj/*.log
for f in "$L"/*.c "$HERE"/tools/probe/luaprobe.c "$HERE"/tools/probe/hello.c; do
  case $f in */lua.c|*/luac.c|*/liolib.c|*/loslib.c|*/loadlib.c|*/ldblib.c|*/linit.c) continue;; esac  # libraries the probe does not open
  n=$(basename "$f" .c)
  ( $VC -c -o "$B/amiga/obj/$n.o" "$f" > "$B/amiga/obj/$n.log" 2>&1 || echo "FAIL $f" ) &
  [ $n = hello ] || objs+=("$B/amiga/obj/$n.o")
done
wait
cat "$B"/amiga/obj/*.log > "$LOG"
for o in "${objs[@]}" "$B/amiga/obj/hello.o"; do [ -f "$o" ] || fail=1; done
echo "compile: failures=$fail warning-lines=$(grep -c -i 'warning' "$LOG") log=$LOG"
[ $fail = 0 ] || exit 1
LD="vlink -bamigahunk -x -Bstatic -Cvbcc -nostdlib -mrel $VBCC_PREFIX/targets/m68k-amigaos/lib/startup.o"
# the math library comes BEFORE vc: it carries the float-capable printf/strtod (after -lvc,
# vc.lib's integer-only versions win: tonumber("3.5") is nil, "%.2f" prints literally)
LIBS="-L$VBCC_PREFIX/targets/m68k-amigaos/lib ${MATHLIB:--lmieee} -lvc"
$LD "${objs[@]}" $LIBS -o "$B/amiga/luaprobe" >> "$LOG" 2>&1 || { echo "LINK FAIL luaprobe"; tail -5 "$LOG"; exit 1; }
$LD "$B/amiga/obj/hello.o" -L$VBCC_PREFIX/targets/m68k-amigaos/lib -lvc -o "$B/amiga/hello" >> "$LOG" 2>&1 || { echo "LINK FAIL hello"; exit 1; }
ls -l "$B/amiga/luaprobe" "$B/amiga/hello" | awk '{print $5, $9}'
