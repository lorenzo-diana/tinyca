// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/executor.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "tinyca/string_util.h"

namespace tinyca
{

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

    if (max_bytes > 0 && out.size() > static_cast<size_t>(max_bytes))
    {
        out.resize(static_cast<size_t>(max_bytes));
        out += "\n... [truncated at output_max_bytes] ...\n";
    }
    return out;
}

Executor::Executor(std::string work_dir, int timeout_s, int output_head_lines, int output_tail_lines,
                   int output_max_bytes)
    : work_dir_(std::move(work_dir)), timeout_s_(timeout_s), head_lines_(output_head_lines),
      tail_lines_(output_tail_lines), max_bytes_(output_max_bytes)
{
}

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
        execl("/bin/bash", "bash", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127); // execl failed
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
