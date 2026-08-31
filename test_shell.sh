#!/usr/bin/env bash
# test_shell.sh - exercises every phase of ./shell: the REPL, builtins,
# redirection, pipes, signals, and job control.
#
# Two styles of test are used:
#   - "batch" tests feed a whole script to `./shell`'s stdin at once and
#     check what it printed (or a side-effect file it wrote). This covers
#     everything that doesn't need a live terminal.
#   - "session" tests keep a `./shell` process alive across several sends,
#     with real signals delivered via `kill` from this script - this is the
#     only way to test Ctrl-C/Ctrl-Z and fg/bg/jobs meaningfully, since
#     those depend on process groups and timing, not just one-shot output.

set -u
cd "$(dirname "$0")"

SHELL_BIN="./shell"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"; pkill -f "$TMP_DIR" 2>/dev/null' EXIT

PASS_COUNT=0
FAIL_COUNT=0

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS: $1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL: $1"; echo "        $2"; }

# assert_contains NAME EXPECTED_SUBSTRING ACTUAL_OUTPUT
assert_contains() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        pass "$name"
    else
        fail "$name" "expected to contain: [$expected]  --  got: [$actual]"
    fi
}

# assert_not_contains NAME UNEXPECTED_SUBSTRING ACTUAL_OUTPUT
assert_not_contains() {
    local name="$1" unexpected="$2" actual="$3"
    if [[ "$actual" != *"$unexpected"* ]]; then
        pass "$name"
    else
        fail "$name" "expected NOT to contain: [$unexpected]  --  got: [$actual]"
    fi
}

# assert_eq NAME EXPECTED ACTUAL
assert_eq() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        pass "$name"
    else
        fail "$name" "expected: [$expected]  --  got: [$actual]"
    fi
}

# run_batch SCRIPT - feeds SCRIPT to a fresh shell, echoes its stdout+stderr.
run_batch() {
    printf '%s' "$1" | "$SHELL_BIN" 2>&1
}

echo "=========================================="
echo " Phase 1-3: REPL, builtins, redirection"
echo "=========================================="

assert_contains "echo prints its arguments" \
    "hello world" "$(run_batch $'echo hello world\nexit\n')"

assert_eq "echo -n suppresses the trailing newline" \
    "no-newline-here" "$(run_batch $'echo -n no-newline-here\nexit\n')"

assert_contains "pwd prints an absolute path" \
    "/" "$(run_batch $'pwd\nexit\n')"

assert_contains "cd changes directory (visible via pwd)" \
    "/tmp" "$(run_batch $'cd /tmp\npwd\nexit\n')"

assert_contains "cd with no argument goes to \$HOME" \
    "$HOME" "$(run_batch $'cd /tmp\ncd\npwd\nexit\n')"

assert_contains "cd to a missing directory reports an error" \
    "No such file or directory" "$(run_batch $'cd /no/such/directory/xyz\nexit\n')"

assert_contains "shell survives a failed cd and keeps running" \
    "still-here" "$(run_batch $'cd /no/such/directory/xyz\necho still-here\nexit\n')"

assert_contains "help lists the builtins" \
    "Builtins" "$(run_batch $'help\nexit\n')"

assert_eq "exit with no preceding output produces no output" \
    "" "$(run_batch $'exit\n')"

assert_contains "EOF (empty stdin, no exit command) terminates cleanly" \
    "" "$(run_batch '')"

assert_eq "a blank line is ignored, not an error" \
    "survived" "$(run_batch $'\n\n\necho survived\nexit\n')"

assert_contains "an external command runs via fork/exec" \
    "hi-from-child" "$(run_batch $'echo hi-from-child\nexit\n')"

assert_contains "a missing command reports an error, not a crash" \
    "No such file or directory" \
    "$(run_batch $'this_command_does_not_exist_xyz\nexit\n')"

assert_contains "the shell keeps running after a missing command" \
    "recovered" "$(run_batch $'nosuchcmd_xyz\necho recovered\nexit\n')"

echo
echo "=========================================="
echo " Phase 3: redirection"
echo "=========================================="

OUT_FILE="$TMP_DIR/redir_out.txt"
run_batch "echo first > $OUT_FILE"$'\nexit\n' >/dev/null
assert_eq "'>' creates a file with the command's output" \
    "first" "$(cat "$OUT_FILE" 2>/dev/null)"

run_batch "echo second >> $OUT_FILE"$'\nexit\n' >/dev/null
assert_eq "'>>' appends instead of overwriting" \
    "$(printf 'first\nsecond')" "$(cat "$OUT_FILE" 2>/dev/null)"

run_batch "echo third > $OUT_FILE"$'\nexit\n' >/dev/null
assert_eq "'>' truncates - it does not append" \
    "third" "$(cat "$OUT_FILE" 2>/dev/null)"

THREE_LINE_FILE="$TMP_DIR/three_lines.txt"
printf 'one\ntwo\nthree\n' >"$THREE_LINE_FILE"
assert_eq "'<' feeds a file's contents to a command's stdin" \
    "3" "$(run_batch "wc -l < $THREE_LINE_FILE"$'\nexit\n' | tr -d ' ')"

assert_contains "redirecting a builtin's output to a file also works" \
    "from-a-builtin" "$(run_batch "echo from-a-builtin > $OUT_FILE"$'\ncat '"$OUT_FILE"$'\nexit\n')"

assert_contains "reading from a nonexistent file reports an error" \
    "No such file or directory" \
    "$(run_batch "cat < $TMP_DIR/does_not_exist.txt"$'\nexit\n')"

echo
echo "=========================================="
echo " Phase 4: pipes"
echo "=========================================="

assert_eq "a two-stage pipe connects stdout to stdin" \
    "3" "$(run_batch "cat $THREE_LINE_FILE | wc -l"$'\nexit\n' | tr -d ' ')"

FRUIT_FILE="$TMP_DIR/fruit.txt"
printf 'banana\napple\ncherry\n' >"$FRUIT_FILE"
assert_eq "a three-stage pipe (sort | uniq) works" \
    "$(printf 'apple\nbanana\ncherry')" \
    "$(run_batch "cat $FRUIT_FILE | sort | uniq"$'\nexit\n')"

assert_eq "an arbitrary-length (four-stage) pipe works" \
    "apple" "$(run_batch "cat $FRUIT_FILE | sort | uniq | head -n 1"$'\nexit\n')"

PIPE_RESULT="$(printf 'nosuchcmd_xyz | wc -l\nexit\n' | "$SHELL_BIN" 2>/dev/null | tr -d ' ')"
assert_eq "a failing first stage does not hang the rest of the pipeline" \
    "0" "$PIPE_RESULT"

assert_contains "a trailing '|' with nothing after it is a syntax error, not a hang" \
    "syntax error" "$(run_batch $'ls |\nexit\n')"

assert_contains "shell keeps running after a pipe syntax error" \
    "recovered" "$(run_batch $'echo x | | wc\necho recovered\nexit\n')"

echo
echo "=========================================="
echo " Phase 6: background jobs"
echo "=========================================="

assert_contains "'&' backgrounds a job and reports a job id immediately" \
    "[1]" "$(run_batch $'sleep 1 &\nexit\n')"

assert_contains "'jobs' lists a running background job" \
    "sleep 1" "$(run_batch $'sleep 1 &\njobs\nwait_a_bit_via_echo\nexit\n' | grep -v echo)"

RESULT="$(run_batch $'sleep 0.2 &\nsleep 0.5\njobs\nexit\n')"
assert_contains "a finished background job is reported as 'Done'" "Done" "$RESULT"

RESULT="$(run_batch $'sleep 0.3 &\nsleep 0.3 &\nexit\n')"
assert_contains "background jobs get job id 1" "[1]" "$RESULT"
assert_contains "background jobs get job id 2" "[2]" "$RESULT"

echo
echo "=========================================="
echo " Phase 5-6: signals and job control"
echo " (these use a live shell session + real kill -SIGNAL)"
echo "=========================================="

# Starts $SHELL_BIN reading from a FIFO so this script can send it commands
# one at a time, like a slow-typing user, with real delays and real signals
# in between. Sets SESSION_FIFO/SESSION_LOG/SESSION_PID as globals.
start_session() {
    SESSION_FIFO="$TMP_DIR/session_fifo_$$_$RANDOM"
    SESSION_LOG="$TMP_DIR/session_log_$$_$RANDOM"
    mkfifo "$SESSION_FIFO"
    : >"$SESSION_LOG"
    ( "$SHELL_BIN" <"$SESSION_FIFO" >"$SESSION_LOG" 2>&1 ) &
    SESSION_PID=$!
    exec {SESSION_FD}>"$SESSION_FIFO"
}

send_to_session() {
    echo "$1" >&"$SESSION_FD"
    sleep 0.3
}

# find_child_pid PATTERN - polls (up to ~4s) for a process matching PATTERN.
find_child_pid() {
    local pattern="$1" found=""
    for _ in $(seq 1 20); do
        found="$(pgrep -f "$pattern" | head -1)"
        [[ -n "$found" ]] && break
        sleep 0.2
    done
    echo "$found"
}

stop_session() {
    exec {SESSION_FD}>&- 2>/dev/null
    wait "$SESSION_PID" 2>/dev/null
}

# --- Ctrl-C: SIGINT kills the foreground job, not the shell ---
start_session
send_to_session "sleep 30"
CHILD_PID="$(find_child_pid "sleep 30")"
if [[ -n "$CHILD_PID" ]]; then
    kill -INT "$CHILD_PID"
    sleep 0.3
    if kill -0 "$CHILD_PID" 2>/dev/null; then
        fail "SIGINT to the foreground child kills it" "pid $CHILD_PID is still alive"
    else
        pass "SIGINT to the foreground child kills it"
    fi
    send_to_session "echo shell-survived-sigint"
    if kill -0 "$SESSION_PID" 2>/dev/null || grep -q "shell-survived-sigint" "$SESSION_LOG"; then
        pass "the shell itself survives Ctrl-C and keeps accepting commands"
    else
        fail "the shell itself survives Ctrl-C and keeps accepting commands" "shell process died"
    fi
else
    fail "SIGINT to the foreground child kills it" "could not find the child process to signal"
fi
send_to_session "exit"
stop_session

# --- Ctrl-Z / bg / fg: full job-control round trip ---
start_session
send_to_session "sleep 30"
CHILD_PID="$(find_child_pid "sleep 30")"
if [[ -n "$CHILD_PID" ]]; then
    kill -TSTP "$CHILD_PID"
    sleep 0.3
    send_to_session "jobs"
    assert_contains "Ctrl-Z (SIGTSTP) suspends the foreground job" \
        "Stopped" "$(cat "$SESSION_LOG")"

    send_to_session "bg"
    sleep 0.2
    send_to_session "jobs"
    LOG_AFTER_BG="$(cat "$SESSION_LOG")"
    assert_contains "'bg' resumes a stopped job and marks it Running" \
        "Running" "$LOG_AFTER_BG"

    STATE="$(ps -o state= -p "$CHILD_PID" 2>/dev/null | tr -d ' ')"
    assert_not_contains "the process itself is actually running again after 'bg' (not still T)" \
        "T" "$STATE"

    send_to_session "fg"
    sleep 0.2
    assert_contains "'fg' echoes the job's command line" \
        "sleep 30" "$(cat "$SESSION_LOG")"

    kill -INT "$CHILD_PID" 2>/dev/null
    sleep 0.3
    send_to_session "echo back-at-the-prompt"
    assert_contains "shell is back at a normal prompt after 'fg' finishes" \
        "back-at-the-prompt" "$(cat "$SESSION_LOG")"
else
    fail "job control round trip (stop/bg/fg)" "could not find the child process to signal"
fi
send_to_session "exit"
stop_session

echo
echo "=========================================="
echo " Results: $PASS_COUNT passed, $FAIL_COUNT failed"
echo "=========================================="

[[ "$FAIL_COUNT" -eq 0 ]]
