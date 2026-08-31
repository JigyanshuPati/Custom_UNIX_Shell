// executor.cpp - see executor.h for the overview.

#include "executor.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

#include "jobs.h"

// execvp() requires a null-terminated array of char*: argv[argc] must be
// nullptr. We build this array only right here, right before the call, and
// only inside the child process. Each pointer aliases the string's own
// internal buffer (legal since C++11 guarantees std::string storage is
// contiguous and null-terminated) - args must not be modified or destroyed
// before execvp runs, and it is not: this is the last thing the child does
// before becoming a different program.
static std::vector<char*> build_argv(std::vector<std::string>& args) {
    std::vector<char*> argv_pointers;
    for (size_t i = 0; i < args.size(); ++i) {
        argv_pointers.push_back(args[i].data());
    }
    argv_pointers.push_back(nullptr);
    return argv_pointers;
}

// Opens the files named by a command's "<"/">"/">>" and dup2()s them onto
// stdin/stdout of whichever process calls this. For an external command,
// execute_pipeline calls it in the child, after fork(), before exec - a
// child's file descriptor table is a private copy of the parent's, so
// changing it there never affects the shell or any other command in the
// pipeline, and it is thrown away the moment that child exits. Builtins
// have no such disposable child to redirect instead: shell.cpp calls this
// function directly on the shell's own stdin/stdout when a builtin has a
// "<"/">"/">>", and is responsible for saving and restoring the original
// fds itself afterward (see shell.cpp), since there the redirection is
// happening to the one long-lived process that has to keep running.
// Returns false (caller should not proceed) if a file could not be opened.
bool apply_redirections(const Command& command) {
    if (!command.input_file.empty()) {
        int input_fd = open(command.input_file.c_str(), O_RDONLY);
        if (input_fd < 0) {
            perror(command.input_file.c_str());
            return false;
        }
        if (dup2(input_fd, STDIN_FILENO) < 0) {
            perror("dup2");
            return false;
        }
        close(input_fd);  // fd 0 is now an equivalent copy; the original can go
    }

    if (!command.output_file.empty()) {
        int open_flags = O_WRONLY | O_CREAT | (command.append_output ? O_APPEND : O_TRUNC);
        int output_fd = open(command.output_file.c_str(), open_flags, 0644);
        if (output_fd < 0) {
            perror(command.output_file.c_str());
            return false;
        }
        if (dup2(output_fd, STDOUT_FILENO) < 0) {
            perror("dup2");
            return false;
        }
        close(output_fd);
    }

    return true;
}

// Runs one command of a pipeline. Never returns: it always ends in _exit().
//
// Sequence and why the order matters:
//   1. signal(..., SIG_DFL) - undo the shell's own SIG_IGN (set in
//      init_job_control). Signal dispositions are inherited across
//      fork()+exec(), so without this the new program would also ignore
//      Ctrl-C/Ctrl-Z forever.
//   2. setpgid()            - join the pipeline's process group (see the
//      comment in execute_pipeline for why both child and parent call this).
//   3. dup2() the pipe ends, then apply_redirections() - pipe wiring goes
//      first so an explicit "<"/">" on this command overrides it (a command
//      that is both mid-pipeline and has "> file" should write to the file).
//   4. close every pipe fd  - see the comment in execute_pipeline about what
//      happens if a write end is left open.
//   5. execvp()             - replace this process's program image. Only
//      returns on failure.
static void run_child_process(Command& command, pid_t process_group_id,
                               const std::vector<int>& pipe_read_ends,
                               const std::vector<int>& pipe_write_ends,
                               int stdin_pipe_index, int stdout_pipe_index) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    if (setpgid(0, process_group_id) < 0) {
        perror("shell: setpgid (child)");
    }

    if (stdin_pipe_index >= 0 && dup2(pipe_read_ends[stdin_pipe_index], STDIN_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }
    if (stdout_pipe_index >= 0 && dup2(pipe_write_ends[stdout_pipe_index], STDOUT_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }

    if (!apply_redirections(command)) {
        _exit(1);
    }

    for (size_t i = 0; i < pipe_read_ends.size(); ++i) {
        close(pipe_read_ends[i]);
        close(pipe_write_ends[i]);
    }

    std::vector<char*> argv_pointers = build_argv(command.args);
    execvp(argv_pointers[0], argv_pointers.data());

    perror(command.args[0].c_str());  // execvp only returns on failure
    _exit(127);
}

// Runs an entire pipeline: N commands need N-1 pipes connecting them.
//
// If we forget to close a pipe's write end somewhere - in the shell itself,
// or in a child that does not use it - every reader downstream can hang
// forever. read() on a pipe only returns 0 (EOF) once *no* process anywhere
// still holds that write end open. A forgotten copy, even one nobody is
// writing through, keeps the pipe "possibly still active" as far as the
// kernel is concerned. Concretely: in "a | b", if the shell's own copy of
// the write end were left open in the parent process, b's read() would
// block forever waiting for an EOF that can never arrive, because that
// extra open write end - unused, but still open - is enough to prevent it.
void execute_pipeline(Pipeline& pipeline, const std::string& raw_line) {
    size_t num_commands = pipeline.commands.size();
    if (num_commands == 0) {
        return;
    }

    std::vector<int> pipe_read_ends(num_commands - 1);
    std::vector<int> pipe_write_ends(num_commands - 1);
    for (size_t i = 0; i < num_commands - 1; ++i) {
        int fds[2];
        if (pipe(fds) < 0) {
            perror("pipe");
            return;
        }
        pipe_read_ends[i] = fds[0];
        pipe_write_ends[i] = fds[1];
    }

    std::vector<pid_t> child_pids;
    pid_t process_group_id = 0;  // 0 until the first child's pid is known

    for (size_t i = 0; i < num_commands; ++i) {
        pid_t child_pid = fork();

        if (child_pid < 0) {
            perror("fork");
            continue;
        }

        if (child_pid == 0) {
            int stdin_pipe_index = (i > 0) ? static_cast<int>(i - 1) : -1;
            int stdout_pipe_index = (i < num_commands - 1) ? static_cast<int>(i) : -1;
            // process_group_id is still 0 in this child only when it is the
            // first command; setpgid(0, 0) then means "make me the leader
            // of a brand new group named after my own pid" - exactly right.
            run_child_process(pipeline.commands[i], process_group_id, pipe_read_ends,
                               pipe_write_ends, stdin_pipe_index, stdout_pipe_index);
        }

        // Parent: both the parent and the child call setpgid() on the
        // child, to close a race. Whichever of the two runs first "wins"
        // harmlessly; the other gets ESRCH (child already exited) or
        // EACCES (child already exec'd - POSIX forbids changing the group
        // of a process that has replaced its image), both expected and
        // safe to ignore. Without both calls, code that runs right after
        // fork() here (like tcsetpgrp() a few lines down) could act before
        // the child has actually joined the group.
        if (process_group_id == 0) {
            process_group_id = child_pid;
        }
        if (setpgid(child_pid, process_group_id) < 0 && errno != EACCES && errno != ESRCH) {
            perror("shell: setpgid (parent)");
        }

        child_pids.push_back(child_pid);
    }

    // The shell's own copies of every pipe fd must be closed once all
    // children have inherited what they need, or the shell itself becomes
    // one of the "extra" processes described above that keeps read ends
    // from ever seeing EOF.
    for (size_t i = 0; i < pipe_read_ends.size(); ++i) {
        close(pipe_read_ends[i]);
        close(pipe_write_ends[i]);
    }

    // Job display lines (jobs/fg/bg) add their own trailing " &" for running
    // jobs, so the stored command line should not already end in one -
    // otherwise a job started as "sleep 1 &" would print as "sleep 1 & &".
    std::string job_command_line = raw_line;
    if (pipeline.run_in_background) {
        size_t ampersand_position = job_command_line.find_last_of('&');
        job_command_line.erase(ampersand_position);
        size_t last_non_space = job_command_line.find_last_not_of(" \t");
        job_command_line.erase(last_non_space == std::string::npos ? 0 : last_non_space + 1);
    }

    Job* job = add_job(process_group_id, child_pids, job_command_line, JOB_RUNNING);

    if (pipeline.run_in_background) {
        std::cout << "[" << job->job_id << "] " << process_group_id << "\n";
        put_job_in_background(job, false);
        return;
    }

    put_job_in_foreground(job, false);

    if (job_is_stopped(*job)) {
        // Mark it here, not just print the notice: reap_background_jobs()
        // (called once per prompt) only prints its own "Stopped" notice
        // when job.status is not already JOB_STOPPED. Without this, the
        // very next loop iteration would think the stop was new and print
        // the same notice a second time.
        job->status = JOB_STOPPED;
        std::cout << "\n[" << job->job_id << "]+ Stopped\t" << raw_line << "\n";
    } else {
        for (size_t i = 0; i < job_list.size(); ++i) {
            if (job_list[i].job_id == job->job_id) {
                job_list.erase(job_list.begin() + static_cast<long>(i));
                break;
            }
        }
    }
}
