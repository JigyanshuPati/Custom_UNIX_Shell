// builtins.h
//
// Commands that run inside the shell process itself, never fork()ed: cd,
// pwd, exit, help, echo, jobs, fg, bg.

#ifndef BUILTINS_H
#define BUILTINS_H

#include <string>
#include <vector>

bool is_builtin(const std::string& command_name);

// Runs the builtin named by args[0]. Sets should_exit to true if the REPL
// loop in shell.cpp should stop (only the "exit" builtin does this).
// Returns an exit-status-like integer (0 for success) for the shell's own
// bookkeeping; nothing currently reads it, but real shells expose it as $?.
int run_builtin(const std::vector<std::string>& args, bool& should_exit);

#endif  // BUILTINS_H
