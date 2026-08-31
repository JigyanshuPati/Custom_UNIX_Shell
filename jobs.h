// jobs.h
//
// Job control: tracking pipelines the shell has launched, moving them
// between foreground/background, and giving/taking back the terminal.
//
// Background job (&): the shell forks it, notes its process group, and
// returns to the prompt immediately without waiting.
// Foreground job: the shell hands the terminal to the job's process group
// with tcsetpgrp(), waits for it, then takes the terminal back.
//
// A "process group" is just a set of related processes the kernel treats as
// one unit for signal delivery: sending a signal to a process group (e.g.
// via Ctrl-C on the controlling terminal) delivers it to every process in
// that group at once. Every command in one pipeline shares a single process
// group, so Ctrl-C stops the whole pipeline together, not just one stage.

#ifndef JOBS_H
#define JOBS_H

#include <string>
#include <termios.h>
#include <sys/types.h>
#include <vector>

enum JobStatus { JOB_RUNNING, JOB_STOPPED };

struct Job {
    int job_id = 0;
    pid_t process_group_id = 0;
    std::vector<pid_t> process_ids;
    std::vector<bool> process_completed;
    std::vector<bool> process_stopped;
    std::string command_line;
    JobStatus status = JOB_RUNNING;
};

extern std::vector<Job> job_list;
extern pid_t shell_pgid;
extern int shell_terminal_fd;
extern bool shell_is_interactive;
extern struct termios shell_terminal_modes;

// Must be called once at startup, before any command is run. Puts the shell
// in its own process group and claims the controlling terminal (only if
// stdin is actually a terminal - a shell reading a script or a pipe skips
// this and never touches the terminal).
void init_job_control();

// Registers a newly-launched pipeline as a job and returns a pointer to it
// (valid until the next add_job/remove call, since job_list can reallocate).
Job* add_job(pid_t process_group_id, const std::vector<pid_t>& process_ids,
             const std::string& command_line, JobStatus status);

Job* find_job(int job_id);
Job* most_recent_job();

// Gives the terminal to the job, optionally sends SIGCONT to wake it up if
// it was stopped, waits for it to finish or stop, then takes the terminal
// back.
void put_job_in_foreground(Job* job, bool send_sigcont);

// Optionally sends SIGCONT, then returns immediately without waiting.
void put_job_in_background(Job* job, bool send_sigcont);

// Blocks until every process in the job has exited or stopped.
void wait_for_job(Job* job);

// Non-blocking: checks all jobs for state changes (WNOHANG) and prints
// "Done"/"Stopped" notices, removing finished jobs from job_list. Called
// once per prompt so background jobs get reaped without the shell blocking.
void reap_background_jobs();

void print_jobs();

bool job_is_stopped(const Job& job);
bool job_is_completed(const Job& job);

#endif  // JOBS_H
