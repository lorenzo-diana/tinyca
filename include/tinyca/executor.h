// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace tinyca
{

struct ExecResult
{
    std::string output; // already truncated (SPEC.md §8)
    int exit_code = -1;
    bool timed_out = false;
    bool killed_by_control = false; // interrupted via interrupt(), not a timeout
};

// fork + execvp of /bin/bash -c <command> (SPEC.md §8). SIGKILL targets the
// whole process group, not just the direct child -- a backgrounded
// grandchild would otherwise survive both a timeout and an interrupt().
class Executor
{
  public:
    Executor(std::string work_dir, int timeout_s, int output_head_lines, int output_tail_lines, int output_max_bytes);

    ExecResult run(const std::string &command);

    // The control thread's way of cutting through a running command
    // immediately (SPEC.md §9.1), independent of the timeout. Safe to call
    // from a different thread than the one blocked in run(); a no-op if
    // nothing is running.
    void interrupt();

  private:
    std::string work_dir_;
    int timeout_s_;
    int head_lines_;
    int tail_lines_;
    int max_bytes_;

    std::mutex pgid_mutex_;
    pid_t current_pgid_ = -1;
    std::atomic<bool> interrupted_{false};
};

// Keep the first head_lines and last tail_lines, inserting a
// "... [N lines omitted] ..." marker between them, then hard-cap at
// max_bytes. Exposed for direct testing.
std::string truncate_output(const std::string &full_output, int head_lines, int tail_lines, int max_bytes);

} // namespace tinyca
