#!/bin/sh
# linux_control_guard.sh -- the Linux gate's must-fail judgement (tools/linux_control.sh's control()), tested with fake
# commands (after Astra, Replies 97 and 101): only a control failing the way it names may pass.  A crash, a silent
# exit, a wrong exit code, and a failure of the WRONG check (the wheel's chip check failing while the Braille Lite
# comparison passes) are all rejected.
#
#   sh tools/linux_control_guard.sh
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/linux_control.sh"
bad=0
expect() {                         # want (ok|FAIL), then control's arguments
    want="$1"; shift
    fail=0
    tmp="$(mktemp)"
    control "$@" > "$tmp"          # not in $( ): control sets $fail, which a subshell would lose
    line="$(cat "$tmp")"; rm -f "$tmp"
    got=ok; [ $fail -ne 0 ] && got=FAIL
    if [ "$got" = "$want" ]; then echo "ok    $want as expected: $line"
    else echo "WRONG $got, wanted $want: $line"; bad=1; fi
}
MARKS='^ok +the chip alone:
^FAIL +the Braille Lite:
^wheel: 1 FAILED$'
wheel() {                          # name, then the fake output's lines and exit code as python code
    name="$1"; shift
    old_ifs="$IFS"; IFS='
'
    # shellcheck disable=SC2086
    set -- "$name" $MARKS -- python3 -c "$1"
    IFS="$old_ifs"
    expect "$want" "$@"
}
want=ok;   wheel "the intended failure" 'print("ok   the chip alone: x"); print("FAIL the Braille Lite: differs"); print("wheel: 1 FAILED"); raise SystemExit(1)'
want=FAIL; wheel "the wrong check failing" 'print("FAIL the chip alone: broken"); print("ok the Braille Lite: correct"); print("wheel: 1 FAILED"); raise SystemExit(1)'
want=FAIL; wheel "a crash" 'raise RuntimeError("broken build")'
want=FAIL; wheel "a silent exit 1" 'raise SystemExit(1)'
want=FAIL; wheel "exit 42" 'print("ok   the chip alone: x"); print("FAIL the Braille Lite: differs"); print("wheel: 1 FAILED"); raise SystemExit(42)'
want=FAIL; wheel "the summary only" 'print("wheel: 1 FAILED"); raise SystemExit(1)'
[ $bad -eq 0 ] && echo "control guard: PASS" || echo "control guard: FAILED"
exit $bad
