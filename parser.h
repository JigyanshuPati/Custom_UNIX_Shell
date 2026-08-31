// parser.h
//
// Turns one line of input into a Pipeline: a list of Commands connected by
// pipes, each with optional input/output redirection, plus a flag for
// whether the whole thing should run in the background.
//
// Limitation (kept simple on purpose, like Brennan's original tutorial):
// operators | < > >> & must be their own whitespace-separated tokens.
// "ls>out" is not recognized as redirection; you must write "ls > out".

#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>

// One command in a pipeline: "grep foo < in.txt > out.txt"
struct Command {
    std::vector<std::string> args;
    std::string input_file;   // empty means no "<" redirection
    std::string output_file;  // empty means no ">" or ">>" redirection
    bool append_output = false;  // true for ">>", false for ">"
};

// One full line of input: "a | b | c &"
struct Pipeline {
    std::vector<Command> commands;
    bool run_in_background = false;
    bool syntax_error = false;  // set by parse_pipeline on malformed input
};

// Splits a line into whitespace-separated tokens.
std::vector<std::string> tokenize_line(const std::string& line);

// Groups tokens into a Pipeline. Prints a message and sets syntax_error on
// malformed input (missing filename after a redirection operator, empty
// command between two pipes, etc.) instead of throwing.
Pipeline parse_pipeline(const std::vector<std::string>& tokens);

// True for a blank input line (no tokens at all).
bool pipeline_is_empty(const Pipeline& pipeline);

#endif  // PARSER_H
