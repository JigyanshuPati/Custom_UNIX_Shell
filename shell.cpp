// shell.cpp
//
// The REPL: print a prompt, read a line, turn it into a Pipeline, and
// either run it as a builtin (directly, in this process) or hand it to
// execute_pipeline() to fork/exec it. See parser.h, builtins.h, executor.h,
// and jobs.h for how each of those pieces works.

#include <climits>
#include <iostream>
#include <unistd.h>

#include "builtins.h"
#include "executor.h"
#include "jobs.h"
#include "parser.h"

// Prompts only make sense when a human is typing at a terminal; a shell
// reading a script from a pipe (as our test script does) should stay quiet
// so its output is just the commands' own output, nothing extra to filter
// out.
static void print_prompt() {
    if (!shell_is_interactive) {
        return;
    }
    char current_directory[PATH_MAX];
    if (getcwd(current_directory, sizeof(current_directory)) == nullptr) {
        std::cout << "shell$ ";
    } else {
        std::cout << current_directory << "$ ";
    }
    std::cout.flush();
}

// Builtins run inside the shell's own long-lived process, so redirecting
// one means temporarily replacing the shell's own stdin/stdout, running the
// builtin, then restoring the originals - unlike an external command,
// where redirection only ever touches a throwaway forked child that exits
// right after (see executor.cpp's comment above apply_redirections).
// Returns false if the REPL loop should stop (the "exit" builtin ran).
static bool run_builtin_with_redirection(const Command& command) {
    bool should_exit = false;

    if (command.input_file.empty() && command.output_file.empty()) {
        run_builtin(command.args, should_exit);
        return !should_exit;
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        perror("shell: dup");
        return true;
    }

    if (apply_redirections(command)) {
        run_builtin(command.args, should_exit);
    }

    // Must flush before fd 1 is pointed back at the original destination -
    // std::cout's buffer is a C++-level detail, independent of which actual
    // OS file the fd number currently refers to, so flushing late would
    // send buffered output to the wrong place.
    std::cout.flush();
    if (dup2(saved_stdin, STDIN_FILENO) < 0 || dup2(saved_stdout, STDOUT_FILENO) < 0) {
        perror("shell: dup2");
    }
    close(saved_stdin);
    close(saved_stdout);

    return !should_exit;
}

int main() {
    init_job_control();

    std::string line;
    while (true) {
        reap_background_jobs();
        print_prompt();

        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl-D): getline() sets std::cin's eofbit and returns a
            // stream that converts to false. A real terminal newline was
            // never sent, so print one ourselves to leave the cursor on a
            // clean line before the shell exits.
            if (shell_is_interactive) {
                std::cout << "\n";
            }
            break;
        }

        std::vector<std::string> tokens = tokenize_line(line);
        if (tokens.empty()) {
            continue;
        }

        Pipeline pipeline = parse_pipeline(tokens);
        if (pipeline.syntax_error || pipeline_is_empty(pipeline)) {
            continue;
        }

        // Builtins only run in place of a single, whole command - not as
        // one stage of a pipeline. cd/pwd/exit/etc. only make sense that
        // way (see the comment above builtin_cd in builtins.cpp for why cd
        // in particular could never work piped through a fork() at all).
        if (pipeline.commands.size() == 1 && is_builtin(pipeline.commands[0].args[0])) {
            if (!run_builtin_with_redirection(pipeline.commands[0])) {
                break;  // "exit" was run
            }
            continue;
        }

        execute_pipeline(pipeline, line);
    }

    return 0;
}
