// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/executor.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <vector>

#include "tinyca/string_util.h"

namespace tinyca
{

// AUDIT NOTES (both visible in what the model is shown, and in the
// transcript):
//  - Output is rebuilt line by line with '\n' appended to each kept line, so
//    the result is always newline-terminated even when the command's own
//    output was not. Line counts, not bytes, decide what is kept.
//  - The byte cap is applied after line selection by a plain resize(), so it
//    can cut mid-line and mid-UTF-8-sequence. The result never exceeds
//    max_bytes (the truncation marker is reserved for inside the cap), but a
//    reviewer reading a truncated tail should not assume it is complete.
std::string truncate_output(const std::string &full_output, int head_lines, int tail_lines, int max_bytes)
{
    std::vector<std::string> lines = split(full_output, '\n');
    // split() on a string ending in '\n' produces one trailing empty
    // element; drop it so line counts match what a human would call "lines".
    if (!lines.empty() && lines.back().empty() && !full_output.empty() && full_output.back() == '\n')
        lines.pop_back();

    std::string out;
    size_t total = lines.size();
    size_t head = static_cast<size_t>(head_lines < 0 ? 0 : head_lines);
    size_t tail = static_cast<size_t>(tail_lines < 0 ? 0 : tail_lines);

    if (total <= head + tail)
    {
        for (size_t i = 0; i < total; ++i)
        {
            out += lines[i];
            out += '\n';
        }
    }
    else
    {
        size_t omitted = total - head - tail;
        for (size_t i = 0; i < head; ++i)
        {
            out += lines[i];
            out += '\n';
        }
        out += "... [" + std::to_string(omitted) + " lines omitted] ...\n";
        for (size_t i = total - tail; i < total; ++i)
        {
            out += lines[i];
            out += '\n';
        }
    }

    // max_bytes is a hard cap on the returned string (SPEC.md §8), so the
    // marker has to fit INSIDE it: reserve its length first, then resize.
    // A cap smaller than the marker itself yields the marker alone, still
    // within the cap (an empty result would hide that anything was cut).
    if (max_bytes > 0 && out.size() > static_cast<size_t>(max_bytes))
    {
        const std::string marker = "\n... [truncated at output_max_bytes] ...\n";
        const size_t cap = static_cast<size_t>(max_bytes);
        const size_t keep = (cap > marker.size()) ? cap - marker.size() : 0;
        out.resize(keep);
        out += marker;
        if (out.size() > cap)
            out.resize(cap);
    }
    return out;
}

Executor::Executor(std::string work_dir, int timeout_s, int output_head_lines, int output_tail_lines,
                   int output_max_bytes)
    : work_dir_(std::move(work_dir)), timeout_s_(timeout_s), head_lines_(output_head_lines),
      tail_lines_(output_tail_lines), max_bytes_(output_max_bytes)
{
}

void Executor::set_path_prepend(const std::string &dir)
{
    path_prepend_ = dir;
}

void Executor::set_env(const std::string &name, const std::string &value)
{
    for (auto &kv : extra_env_)
    {
        if (kv.first == name)
        {
            kv.second = value;
            return;
        }
    }
    extra_env_.push_back(std::make_pair(name, value));
}

namespace
{

// Builds the child's environment in the PARENT, before fork(): everything
// between fork() and exec() in a multi-threaded process must avoid malloc,
// and setenv() in the child would allocate. `storage` owns the strings that
// the returned pointer array points into, so both must outlive the exec call.
void build_child_env(const std::string &path_prepend, const std::vector<std::pair<std::string, std::string>> &extra,
                     std::vector<std::string> &storage, std::vector<char *> &envp)
{
    const std::string kPathPrefix = "PATH=";
    bool path_seen = false;
    for (char **e = environ; e != nullptr && *e != nullptr; ++e)
    {
        std::string entry = *e;
        if (!path_prepend.empty() && entry.compare(0, kPathPrefix.size(), kPathPrefix) == 0)
        {
            entry = kPathPrefix + path_prepend + ":" + entry.substr(kPathPrefix.size());
            path_seen = true;
        }
        bool overridden = false;
        for (const auto &kv : extra)
            if (entry.compare(0, kv.first.size() + 1, kv.first + "=") == 0)
                overridden = true; // the explicit value below wins
        if (!overridden)
            storage.push_back(entry);
    }
    if (!path_prepend.empty() && !path_seen)
        storage.push_back(kPathPrefix + path_prepend); // no PATH inherited at all
    for (const auto &kv : extra)
        storage.push_back(kv.first + "=" + kv.second);
    for (auto &entry : storage)
        envp.push_back(const_cast<char *>(entry.c_str()));
    envp.push_back(nullptr);
}

} // namespace

ExecResult Executor::run(const std::string &command)
{
    ExecResult result;
    interrupted_ = false;

    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        result.output = "internal error: pipe() failed";
        return result;
    }

    // Deliberately fork/exec rather than popen(): the child needs its own
    // process group (for the group-wide SIGKILL below) and both streams
    // merged into one pipe, neither of which popen() offers.
    std::vector<std::string> env_storage;
    std::vector<char *> envp;
    build_child_env(path_prepend_, extra_env_, env_storage, envp);
    std::string bash_path = "/bin/bash";
    std::string dash_c = "-c";
    char *argv[] = {const_cast<char *>(bash_path.c_str()), const_cast<char *>(dash_c.c_str()),
                    const_cast<char *>(command.c_str()), nullptr};

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        result.output = "internal error: fork() failed";
        return result;
    }

    if (pid == 0)
    {
        // Child: own process group, so a timeout/interrupt kill(-pgid) also
        // reaches backgrounded grandchildren.
        setpgid(0, 0);
        if (!work_dir_.empty())
        {
            if (chdir(work_dir_.c_str()) != 0)
                _exit(126);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execve(bash_path.c_str(), argv, envp.data());
        _exit(127); // execve failed
    }

    // Parent.
    close(pipefd[1]);
    {
        std::lock_guard<std::mutex> lock(pgid_mutex_);
        current_pgid_ = pid; // == pgid, since the child called setpgid(0, 0)
    }

    std::string output;
    bool timed_out = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s_);
    char buf[4096];

    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining < 0)
            remaining = 0;

        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, static_cast<int>(remaining));

        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP)))
        {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0)
            {
                output.append(buf, static_cast<size_t>(n));
                continue;
            }
            break; // n == 0: EOF, child closed its end
        }
        if (pr == 0)
        {
            timed_out = true;
            kill(-pid, SIGKILL);
            // Drain whatever was already buffered before the kill took effect.
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
                output.append(buf, static_cast<size_t>(n));
            break;
        }
        // pr < 0: interrupted by a signal (e.g. our own kill() from another
        // thread delivering SIGCHLD) -- just retry the poll.
    }

    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    {
        std::lock_guard<std::mutex> lock(pgid_mutex_);
        current_pgid_ = -1;
    }

    // AUDIT NOTE: on a timeout or a control interrupt, exit_code keeps its
    // initial -1 -- there is no real wait status to report, since the process
    // group was SIGKILLed. Callers must read timed_out/killed_by_control
    // rather than inferring failure from exit_code alone.
    if (timed_out)
        result.timed_out = true;
    else if (interrupted_)
        result.killed_by_control = true;
    else if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);

    result.output = truncate_output(output, head_lines_, tail_lines_, max_bytes_);
    return result;
}

void Executor::interrupt()
{
    std::lock_guard<std::mutex> lock(pgid_mutex_);
    if (current_pgid_ > 0)
    {
        interrupted_ = true;
        kill(-current_pgid_, SIGKILL);
    }
}

} // namespace tinyca
