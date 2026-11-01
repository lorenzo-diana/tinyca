// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

    // Directory prepended to the child's PATH, so a skill can be invoked by
    // its bare filename from any work_dir (SPEC.md §7). Needed because
    // commands run with cwd = work_dir while skills live outside it: the
    // repo-root-relative form a skill's @usage advertises would otherwise
    // fail with exit 127. Empty (the default) leaves PATH untouched.
    void set_path_prepend(const std::string &dir);

    // Adds (or replaces) one variable in the child's environment. Used to
    // tell skills where the skills directory is -- they run with
    // cwd = work_dir and cannot derive it (SPEC.md §7.1).
    void set_env(const std::string &name, const std::string &value);

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

    std::string path_prepend_;
    std::vector<std::pair<std::string, std::string>> extra_env_;

    std::mutex pgid_mutex_;
    pid_t current_pgid_ = -1;
    std::atomic<bool> interrupted_{false};
};

// Keep the first head_lines and last tail_lines, inserting a
// "... [N lines omitted] ..." marker between them, then hard-cap at
// max_bytes. Exposed for direct testing.
std::string truncate_output(const std::string &full_output, int head_lines, int tail_lines, int max_bytes);

} // namespace tinyca
