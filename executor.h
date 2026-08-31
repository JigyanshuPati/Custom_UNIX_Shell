// executor.h
//
// Runs a parsed Pipeline: forks one child per command, wires up pipes and
// file redirection between them, and either waits for the whole pipeline
// (foreground) or hands it to the job table and returns immediately
// (background, when the line ended in "&").

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>

#include "parser.h"

// raw_line is the original, unparsed input line - kept only so jobs/fg/bg
// can print something readable ("[1]+ Running   sleep 10 &").
void execute_pipeline(Pipeline& pipeline, const std::string& raw_line);

// Opens the files named by a command's "<"/">"/">>", and dup2()s them onto
// stdin/stdout of the *calling* process. execute_pipeline calls this inside
// a freshly-forked child, where the effect is thrown away when that child
// exits. shell.cpp also calls it directly (no fork) to redirect a builtin,
// saving and restoring its own stdin/stdout around the call - see the
// comment above this function in executor.cpp for why builtins need that
// extra care that external commands do not.
bool apply_redirections(const Command& command);

#endif  // EXECUTOR_H
