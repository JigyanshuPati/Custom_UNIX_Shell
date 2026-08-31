// jobs.cpp - see jobs.h for the overview.

#include "jobs.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

std::vector<Job> job_list;
pid_t shell_pgid = 0;
int shell_terminal_fd = STDIN_FILENO;
bool shell_is_interactive = false;
struct termios shell_terminal_modes;

static int next_job_id = 1;

// Sequence and why the order matters:
//   1. isatty()      - if stdin isn't a terminal (a script or a pipe feeds
//                       us input), there is no terminal to control, so we
//                       skip everything below entirely.
//   2. loop on tcgetpgrp() vs getpgrp(), sending SIGTTIN to ourselves - if
//                       this shell was itself started in the background by
//                       another shell, we are not yet the terminal's
//                       foreground process group. Reading from the terminal
//                       in that state would raise SIGTTIN and stop us; we
//                       wait here (stopped, then resumed by the user
//                       bringing us to the foreground) until we are.
//   3. signal(..., SIG_IGN) - the shell must survive Ctrl-C/Ctrl-\/Ctrl-Z
//                       at the prompt; only the foreground job should react
//                       to them. This must happen before we launch any job.
//   4. setpgid() then tcsetpgrp() - put the shell in its own process group
//                       (in case it was started as part of another job's
//                       group) and only then claim the terminal, since
//                       tcsetpgrp requires the calling process to already
//                       be a valid process group in the same session.
void init_job_control() {
    shell_is_interactive = isatty(shell_terminal_fd);
    if (!shell_is_interactive) {
        return;
    }

    while (tcgetpgrp(shell_terminal_fd) != (shell_pgid = getpgrp())) {
        kill(-shell_pgid, SIGTTIN);
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        perror("shell: could not put shell in its own process group");
        exit(1);
    }
    tcsetpgrp(shell_terminal_fd, shell_pgid);
    tcgetattr(shell_terminal_fd, &shell_terminal_modes);
}

Job* add_job(pid_t process_group_id, const std::vector<pid_t>& process_ids,
             const std::string& command_line, JobStatus status) {
    Job job;
    job.job_id = next_job_id++;
    job.process_group_id = process_group_id;
    job.process_ids = process_ids;
    job.process_completed.assign(process_ids.size(), false);
    job.process_stopped.assign(process_ids.size(), false);
    job.command_line = command_line;
    job.status = status;
    job_list.push_back(job);
    return &job_list.back();
}

Job* find_job(int job_id) {
    for (size_t i = 0; i < job_list.size(); ++i) {
        if (job_list[i].job_id == job_id) {
            return &job_list[i];
        }
    }
    return nullptr;
}

Job* most_recent_job() {
    if (job_list.empty()) {
        return nullptr;
    }
    return &job_list.back();
}

// True only if no process in the job is still running AND at least one is
// genuinely stopped (Ctrl-Z), not merely finished - "everyone is either
// done or paused, and someone is actually paused" is what makes a job
// resumable with fg/bg. Without the any_stopped check, a job where every
// process simply exited normally would also satisfy "nothing is running"
// and get misreported as stopped instead of done.
bool job_is_stopped(const Job& job) {
    bool any_stopped = false;
    for (size_t i = 0; i < job.process_ids.size(); ++i) {
        if (!job.process_completed[i] && !job.process_stopped[i]) {
            return false;
        }
        if (job.process_stopped[i]) {
            any_stopped = true;
        }
    }
    return any_stopped;
}

bool job_is_completed(const Job& job) {
    for (size_t i = 0; i < job.process_ids.size(); ++i) {
        if (!job.process_completed[i]) {
            return false;
        }
    }
    return true;
}

// process_completed/process_stopped only change when a waitpid() call
// reports something new - they are a memory of the last status we actually
// observed, not a live view. Sending SIGCONT resumes a process without any
// waitpid() call happening at all, so without this, a process that was
// stopped once would be remembered as stopped forever: job_is_stopped()
// would keep reporting true, and wait_for_job()'s loop condition would then
// exit immediately every time, never actually calling waitpid() again to
// notice the process later exiting or re-stopping.
static void clear_stopped_flags(Job* job) {
    for (size_t i = 0; i < job->process_stopped.size(); ++i) {
        job->process_stopped[i] = false;
    }
}

// Records the outcome of one waitpid() call against the process it names.
static void mark_process_status(Job* job, pid_t pid, int status) {
    for (size_t i = 0; i < job->process_ids.size(); ++i) {
        if (job->process_ids[i] != pid) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->process_stopped[i] = true;
        } else {
            // WIFEXITED or WIFSIGNALED: either way, this process is done.
            job->process_completed[i] = true;
        }
        return;
    }
}

// waitpid(-pgid, ...) waits for any process in that process group, so one
// call here can report on any of the pipeline's stages, in any order. We
// keep calling it until every process has either exited or stopped.
// WUNTRACED is required to be told about stops (Ctrl-Z) at all; without it,
// waitpid only reports terminations.
void wait_for_job(Job* job) {
    while (!job_is_stopped(*job) && !job_is_completed(*job)) {
        int status = 0;
        pid_t waited_pid = waitpid(-job->process_group_id, &status, WUNTRACED);
        if (waited_pid < 0) {
            if (errno == ECHILD) {
                break;
            }
            perror("shell: waitpid");
            break;
        }
        mark_process_status(job, waited_pid, status);
    }
}

// tcsetpgrp() must happen before we wait: it is what makes the kernel's
// terminal driver deliver Ctrl-C/Ctrl-Z to the job's process group instead
// of the shell's. We restore the shell's own terminal control (and its
// saved terminal modes, in case the job changed them) after the job stops
// being in the foreground, whether that is because it finished or because
// it was suspended.
void put_job_in_foreground(Job* job, bool send_sigcont) {
    if (shell_is_interactive) {
        tcsetpgrp(shell_terminal_fd, job->process_group_id);
    }

    if (send_sigcont) {
        clear_stopped_flags(job);
        if (kill(-job->process_group_id, SIGCONT) < 0) {
            perror("shell: kill (SIGCONT)");
        }
    }

    wait_for_job(job);

    if (shell_is_interactive) {
        tcsetpgrp(shell_terminal_fd, shell_pgid);
        tcsetattr(shell_terminal_fd, TCSADRAIN, &shell_terminal_modes);
    }
}

void put_job_in_background(Job* job, bool send_sigcont) {
    if (send_sigcont) {
        clear_stopped_flags(job);
        if (kill(-job->process_group_id, SIGCONT) < 0) {
            perror("shell: kill (SIGCONT)");
        }
    }
    job->status = JOB_RUNNING;
}

void reap_background_jobs() {
    for (size_t job_index = 0; job_index < job_list.size();) {
        Job& job = job_list[job_index];

        int status = 0;
        pid_t waited_pid;
        while ((waited_pid = waitpid(-job.process_group_id, &status, WNOHANG | WUNTRACED)) > 0) {
            mark_process_status(&job, waited_pid, status);
        }
        if (waited_pid < 0 && errno != ECHILD) {
            perror("shell: waitpid");
        }

        if (job_is_completed(job)) {
            std::cout << "[" << job.job_id << "]+ Done\t" << job.command_line << "\n";
            job_list.erase(job_list.begin() + static_cast<long>(job_index));
            continue;  // next job shifted into job_index; do not advance
        }
        if (job_is_stopped(job) && job.status != JOB_STOPPED) {
            job.status = JOB_STOPPED;
            std::cout << "[" << job.job_id << "]+ Stopped\t" << job.command_line << "\n";
        }
        job_index += 1;
    }
}

void print_jobs() {
    for (size_t i = 0; i < job_list.size(); ++i) {
        const Job& job = job_list[i];
        std::cout << "[" << job.job_id << "]  "
                   << (job.status == JOB_RUNNING ? "Running" : "Stopped") << "\t\t"
                   << job.command_line << (job.status == JOB_RUNNING ? " &" : "") << "\n";
    }
}
