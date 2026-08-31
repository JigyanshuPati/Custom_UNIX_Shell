# A Teaching UNIX Shell

A small UNIX shell in C++17, modeled on Stephen Brennan's *Write a Shell in C*
tutorial and extended with redirection, arbitrary-length pipes, signal
handling, and job control. Built in six phases, each one working before the
next was added; see "How it's built" below.

## Build and run

```bash
make        # builds ./shell
./shell     # run it
make clean  # remove build output
```

Run the test suite (38 cases covering every phase):

```bash
./test_shell.sh
```

## Files

| File | Contents |
|---|---|
| `shell.cpp` | The REPL: prompt, read a line, tokenize/parse it, dispatch to a builtin or to the executor. |
| `parser.h` / `parser.cpp` | Turns a line of text into a `Pipeline` of `Command`s (args, redirection, background flag). |
| `executor.h` / `executor.cpp` | Forks one process per command in a pipeline, wires up pipes and file redirection, manages process groups, launches the job. |
| `builtins.h` / `builtins.cpp` | `cd`, `pwd`, `exit`, `help`, `echo`, `jobs`, `fg`, `bg` - run directly in the shell process, no fork. |
| `jobs.h` / `jobs.cpp` | The job table: process groups, foreground/background handoff, terminal control, reaping. |
| `test_shell.sh` | 38 automated test cases (batch tests + live signal/job-control sessions). |

## Feature -> syscall map

| Feature | Where | Syscalls / library calls used |
|---|---|---|
| Read a command line | `shell.cpp` | `std::getline` on `std::cin` |
| Tokenize / parse | `parser.cpp` | none (pure string processing) |
| Run an external command | `executor.cpp` | `fork()`, `execvp()`, `waitpid()` |
| `cd` | `builtins.cpp` | `chdir()` |
| `pwd` | `builtins.cpp` | `getcwd()` |
| Output redirection `>` / `>>` | `executor.cpp` (`apply_redirections`) | `open()` (`O_CREAT\|O_TRUNC` or `O_CREAT\|O_APPEND`), `dup2()`, `close()` |
| Input redirection `<` | `executor.cpp` (`apply_redirections`) | `open()` (`O_RDONLY`), `dup2()`, `close()` |
| Redirecting a builtin | `shell.cpp` (`run_builtin_with_redirection`) | `dup()` (save), `apply_redirections`, `dup2()` (restore) |
| Pipes `a \| b \| c` | `executor.cpp` (`execute_pipeline`) | `pipe()` per stage, `dup2()`, `close()` |
| Ctrl-C / Ctrl-\\ / Ctrl-Z ignored by the shell | `jobs.cpp` (`init_job_control`) | `signal(SIGINT/SIGQUIT/SIGTSTP, SIG_IGN)` |
| Default signal behavior restored in children | `executor.cpp` (`run_child_process`) | `signal(..., SIG_DFL)` before `execvp()` |
| Process groups (one per pipeline) | `executor.cpp`, `jobs.cpp` | `setpgid()` |
| Terminal handoff to the foreground job | `jobs.cpp` (`put_job_in_foreground`) | `tcsetpgrp()`, `tcgetattr()` / `tcsetattr()` |
| Background jobs (`&`) | `executor.cpp`, `jobs.cpp` | same `fork`/`pipe`/`setpgid` machinery, no wait |
| Reaping finished background jobs | `jobs.cpp` (`reap_background_jobs`) | `waitpid(-pgid, ..., WNOHANG \| WUNTRACED)` |
| Waiting for the foreground job, noticing Ctrl-Z stops | `jobs.cpp` (`wait_for_job`) | `waitpid(-pgid, ..., WUNTRACED)` |
| `jobs` / `fg` / `bg` | `builtins.cpp`, `jobs.cpp` | `kill(-pgid, SIGCONT)` (fg/bg on a stopped job), the above `waitpid`/`tcsetpgrp` |
| Shell claims the controlling terminal at startup | `jobs.cpp` (`init_job_control`) | `isatty()`, `tcgetpgrp()`, `getpgrp()`, `setpgid()`, `tcsetpgrp()` |

## The four syscalls, explained

**`fork()`** clones the calling process. After it returns, two nearly
identical processes are running the same program from the same point - the
parent gets the child's PID back from `fork()`, the child gets `0`. Every
external command in this shell starts with a `fork()`: one process (the
child) goes on to `execvp()` into a different program; the other (the
parent, the shell itself) goes on to `waitpid()` for it.

**`execvp(file, argv)`** replaces the *current* process's program image with
a new one - same PID, same open file descriptors, same process group, but a
completely different `main()` and address space. It only returns if it
*failed* (e.g. the command doesn't exist); on success, the calling code
never gets control back at all. It's always called after `fork()`, in the
child, so that replacing the program image doesn't take out the shell
itself. The `p` in `execvp` means it searches `$PATH` for the command name,
which is why this shell never has to implement that search itself.

**`dup2(old_fd, new_fd)`** makes `new_fd` become another reference to
whatever `old_fd` points to (closing whatever `new_fd` used to point to
first). `dup2(file_fd, STDOUT_FILENO)` is how "redirect stdout to a file"
and "redirect stdout to a pipe" are both implemented: after that call, fd 1
*is* the file (or the pipe), and any code that just writes to fd 1 - like
every `printf`/`std::cout` in every program ever - ends up writing there
without knowing anything changed.

**`setpgid(pid, pgid)`** puts process `pid` into process group `pgid`
(creating that group if `pid == pgid`). A process group is the unit the
kernel uses for delivering job-control signals: sending a signal to a
*process group* (which the terminal driver does automatically for Ctrl-C,
Ctrl-\\, and Ctrl-Z, and which `kill(-pgid, sig)` does explicitly) reaches
every process in that group at once. Every command in one pipeline is put
into the same new process group, so Ctrl-C stops the whole pipeline
together, not just the last stage the shell happened to fork most recently.

## How it's built (six phases)

1. **REPL** - prompt, `getline`, tokenize, `fork`+`execvp`+`waitpid`, clean
   exit on EOF.
2. **Builtins** - `cd`, `pwd`, `exit`, `help`, `echo`, all running directly in
   the shell process (see the comment above `builtin_cd` in `builtins.cpp`
   for why `cd` specifically *cannot* be a child process: a child's
   `chdir()` only changes that child's own cwd, which vanishes the moment it
   exits, leaving the shell's cwd - the one every future command actually
   runs from - completely untouched).
3. **Redirection** - `<`, `>`, `>>` via `open()`+`dup2()`, applied in the
   child after `fork()` and before `execvp()`.
4. **Pipes** - arbitrary length, one `pipe()` per adjacent pair of commands,
   with careful `close()`ing of every fd nobody needs (see the big comment
   above `execute_pipeline` in `executor.cpp` for what happens if a write end
   is left open: the next reader in the pipeline can block forever, because
   `read()` only returns EOF once *no* process anywhere still holds that
   write end open).
5. **Signals** - the shell ignores `SIGINT`/`SIGQUIT`/`SIGTSTP` so Ctrl-C at
   the prompt doesn't kill it, and children reset those to their default
   behavior before `execvp()` (signal dispositions are inherited across
   `fork()`+`exec()`, so without this reset, every program the shell ran
   would also silently ignore Ctrl-C).
6. **Job control** - `&` for background jobs, `jobs`/`fg`/`bg`, one process
   group per pipeline, and `tcsetpgrp()` to hand the controlling terminal to
   the foreground job's process group (and take it back afterward) so that
   Ctrl-C/Ctrl-Z generated by the terminal driver land on the job, not the
   shell.

## Known limitations (by design, for readability)

- **No quoting or escaping.** Tokens are split on whitespace only, just like
  Brennan's original tutorial. `echo "a b"` runs `echo` with two arguments,
  `"a` and `b"`, quote characters and all. Operators (`| < > >> &`) must be
  their own whitespace-separated tokens: `echo hi>out` is not recognized as
  redirection; write `echo hi > out`.
- **Builtins never run as a pipeline stage**, only as an entire, standalone
  command (redirection on a lone builtin, e.g. `pwd > out.txt`, does work -
  see `run_builtin_with_redirection` in `shell.cpp`). `echo hi | cat` runs
  `/bin/echo` as an external program instead, which happens to also work,
  but `cd /tmp | cat` would fail (no external `cd` binary exists), which is
  the correct and expected outcome for a command that only makes sense
  inside the shell's own process.
- **No `cd -`** (previous-directory tracking) or shell variables/expansion.
- **`&` must be the last token** on the line; `cmd1 & cmd2` is a syntax
  error rather than two separate jobs.
