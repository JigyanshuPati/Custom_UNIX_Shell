// parser.cpp - see parser.h for the overview.

#include "parser.h"

#include <iostream>

std::vector<std::string> tokenize_line(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current_token;

    for (size_t i = 0; i < line.size(); ++i) {
        char current_char = line[i];
        if (current_char == ' ' || current_char == '\t') {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token.push_back(current_char);
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    return tokens;
}

bool pipeline_is_empty(const Pipeline& pipeline) {
    return pipeline.commands.size() == 1 && pipeline.commands[0].args.empty();
}

Pipeline parse_pipeline(const std::vector<std::string>& tokens) {
    Pipeline pipeline;
    Command current_command;

    size_t index = 0;
    while (index < tokens.size()) {
        const std::string& token = tokens[index];

        if (token == "|") {
            pipeline.commands.push_back(current_command);
            current_command = Command();
            index += 1;

        } else if (token == "<") {
            if (index + 1 >= tokens.size()) {
                std::cerr << "shell: expected a filename after '<'\n";
                pipeline.syntax_error = true;
                return pipeline;
            }
            current_command.input_file = tokens[index + 1];
            index += 2;

        } else if (token == ">" || token == ">>") {
            if (index + 1 >= tokens.size()) {
                std::cerr << "shell: expected a filename after '" << token << "'\n";
                pipeline.syntax_error = true;
                return pipeline;
            }
            current_command.output_file = tokens[index + 1];
            current_command.append_output = (token == ">>");
            index += 2;

        } else if (token == "&") {
            if (index + 1 != tokens.size()) {
                std::cerr << "shell: '&' must be the last token on the line\n";
                pipeline.syntax_error = true;
                return pipeline;
            }
            pipeline.run_in_background = true;
            index += 1;

        } else {
            current_command.args.push_back(token);
            index += 1;
        }
    }

    pipeline.commands.push_back(current_command);

    // A lone blank line parses to one command with empty args - that is not
    // an error, execute_pipeline just does nothing with it. But an empty
    // command anywhere in a multi-command pipeline ("ls | | wc", "ls |")
    // means the user left out a command between (or after) pipe symbols.
    if (pipeline.commands.size() > 1) {
        for (size_t i = 0; i < pipeline.commands.size(); ++i) {
            if (pipeline.commands[i].args.empty()) {
                std::cerr << "shell: syntax error near unexpected token '|'\n";
                pipeline.syntax_error = true;
                return pipeline;
            }
        }
    }

    return pipeline;
}
