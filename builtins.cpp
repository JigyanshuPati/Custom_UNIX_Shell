// builtins.cpp - see builtins.h for the overview.

#include "builtins.h"

#include <climits>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

#include "jobs.h"

bool is_builtin(const std::string& command_name) {
    return command_name == "cd" || command_name == "pwd" || command_name == "exit" ||
           command_name == "help" || command_name == "echo" || command_name == "jobs" ||
           command_name == "fg" || command_name == "bg";
}

// cd changes the *shell process's own* current working directory. This is
// exactly why it cannot be an external program run as a child: every
// process has a private cwd that fork() copies but never propagates back
// up. If "cd" were a separate executable, the shell would fork a child,
// that child would chdir() and exit, and the shell's own cwd - the one
// every future command actually runs from - would be completely unchanged.
// Running it directly in the shell process is the only way "cd" can have
// any lasting effect.
static int builtin_cd(const std::vector<std::string>& args) {
    std::string target_directory;
    if (args.size() < 2) {
        const char* home_directory = getenv("HOME");
        if (home_directory == nullptr) {
            std::cerr << "cd: HOME not set\n";
            return 1;
        }
        target_directory = home_directory;
    } else {
        target_directory = args[1];
    }

    if (chdir(target_directory.c_str()) < 0) {
        perror(target_directory.c_str());
        return 1;
    }
    return 0;
}

static int builtin_pwd() {
    char current_directory[PATH_MAX];
    if (getcwd(current_directory, sizeof(current_directory)) == nullptr) {
        perror("pwd");
        return 1;
    }
    std::cout << current_directory << "\n";
    return 0;
}

static int builtin_echo(const std::vector<std::string>& args) {
    size_t start_index = 1;
    bool suppress_newline = false;
    if (args.size() > 1 && args[1] == "-n") {
        suppress_newline = true;
        start_index = 2;
    }

    for (size_t i = start_index; i < args.size(); ++i) {
        if (i > start_index) {
            std::cout << " ";
        }
        std::cout << args[i];
    }
    if (!suppress_newline) {
        std::cout << "\n";
    }
    return 0;
}

static int builtin_help() {
    std::cout
        << "A teaching UNIX shell.\n"
           "Builtins:\n"
           "  cd [dir]       change directory (default: $HOME)\n"
           "  pwd            print working directory\n"
           "  echo [-n] ...  print arguments\n"
           "  jobs           list background/stopped jobs\n"
           "  fg [%job]      resume a job in the foreground\n"
           "  bg [%job]      resume a stopped job in the background\n"
           "  exit           exit the shell\n"
           "  help           show this message\n"
           "Everything else is looked up on $PATH and run as: command args...\n"
           "Pipelines (a | b | c), redirection (< > >>), and background jobs\n"
           "(trailing &) are all supported for external commands.\n";
    return 0;
}

// Parses an optional job-id argument like "%2" or plain "2". With no
// argument at all, falls back to the most recently launched job (matching
// the usual shorthand meaning of a bare "fg" or "bg").
static Job* resolve_job_argument(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        Job* job = most_recent_job();
        if (job == nullptr) {
            std::cerr << "shell: no current job\n";
        }
        return job;
    }

    std::string job_id_text = args[1];
    if (!job_id_text.empty() && job_id_text[0] == '%') {
        job_id_text = job_id_text.substr(1);
    }

    Job* job = find_job(atoi(job_id_text.c_str()));
    if (job == nullptr) {
        std::cerr << "shell: no such job: " << args[1] << "\n";
    }
    return job;
}

static int builtin_fg(const std::vector<std::string>& args) {
    Job* job = resolve_job_argument(args);
    if (job == nullptr) {
        return 1;
    }

    std::cout << job->command_line << "\n";
    bool was_stopped = (job->status == JOB_STOPPED);
    put_job_in_foreground(job, was_stopped);

    if (job_is_completed(*job)) {
        for (size_t i = 0; i < job_list.size(); ++i) {
            if (job_list[i].job_id == job->job_id) {
                job_list.erase(job_list.begin() + static_cast<long>(i));
                break;
            }
        }
    } else if (job_is_stopped(*job)) {
        std::cout << "\n[" << job->job_id << "]+ Stopped\t" << job->command_line << "\n";
    }
    return 0;
}

static int builtin_bg(const std::vector<std::string>& args) {
    Job* job = resolve_job_argument(args);
    if (job == nullptr) {
        return 1;
    }
    put_job_in_background(job, true);
    std::cout << "[" << job->job_id << "]+ " << job->command_line << " &\n";
    return 0;
}

int run_builtin(const std::vector<std::string>& args, bool& should_exit) {
    should_exit = false;
    const std::string& command_name = args[0];

    if (command_name == "cd") {
        return builtin_cd(args);
    }
    if (command_name == "pwd") {
        return builtin_pwd();
    }
    if (command_name == "echo") {
        return builtin_echo(args);
    }
    if (command_name == "help") {
        return builtin_help();
    }
    if (command_name == "jobs") {
        print_jobs();
        return 0;
    }
    if (command_name == "fg") {
        return builtin_fg(args);
    }
    if (command_name == "bg") {
        return builtin_bg(args);
    }
    if (command_name == "exit") {
        should_exit = true;
        return 0;
    }

    std::cerr << "shell: unknown builtin: " << command_name << "\n";
    return 1;
}
