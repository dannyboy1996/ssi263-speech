# linux_control.sh -- control(), sourced by linux_tests.sh and tested by linux_control_guard.sh.  Expects $fail.
# A must-fail control: it passes only by failing the way it names (as run_tests.py's, after Astra, Replies 97 and
# 101) -- exit 1, EVERY extended-regex mark found in its output (the intended failures, the checks that must still
# pass, the completed summary), and no Python traceback.  A crash, a missing build or another failure is not that.
control() {                        # name, mark..., --, then the command
    name="$1"; shift
    marks=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do marks="$marks
$1"; shift; done
    shift
    out="$("$@" 2>&1)"; got=$?
    why=""
    if echo "$out" | grep -q "Traceback (most recent call last)"; then why="control CRASHED instead of failing"
    elif [ $got -ne 1 ]; then why="control exit $got, not its expected 1"
    else
        old_ifs="$IFS"; IFS='
'
        for m in $marks; do
            if [ -z "$why" ] && ! echo "$out" | grep -E -q "$m"; then why="control's own failure not shown (missing $m)"; fi
        done
        IFS="$old_ifs"
    fi
    if [ -z "$why" ]; then echo "ok    $name: $(echo "$out" | tail -1 | cut -c1-90)"
    else echo "FAIL  $name: $why"; fail=1; fi
}
