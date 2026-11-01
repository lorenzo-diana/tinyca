// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/logger.h"

#include <sys/stat.h>

#include <chrono>
#include <fstream>
#include <iostream>

namespace tinyca
{

namespace
{
// Escapes only '"' and '\\'. That is sufficient for its single caller --
// `kind`, which is always an internal string literal ("request", "response",
// "tool_result", ...) with no control characters. It is NOT a general JSON
// string escaper; payloads are serialised by nlohmann::json upstream and
// passed through verbatim.
std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

// Size of an existing transcript, or 0 if there is none yet. Used to make
// transcript_max_bytes a cap on the file rather than on one run's growth.
long existing_file_size(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<long>(st.st_size);
}

long now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

Logger::Logger(std::string transcript_path, long transcript_max_bytes, long transcript_max_bytes_per_run,
               LogLevel level)
    : path_(std::move(transcript_path)), max_bytes_(transcript_max_bytes),
      max_bytes_per_run_(transcript_max_bytes_per_run), level_(level), file_bytes_(existing_file_size(path_))
{
}

std::string Logger::cutoff_line(const std::string &which_cap)
{
    return "{\"ts\":" + std::to_string(now_unix_ms()) + ",\"kind\":\"transcript_cutoff\",\"payload\":{\"reason\":\"" +
           which_cap + " exceeded, further entries dropped\"}}\n";
}

void Logger::write_cutoff_line(const std::string &which_cap)
{
    if (cutoff_warned_)
        return;
    cutoff_warned_ = true; // set first: a cap too small to hold the line still stops appending
    const std::string warn_line = cutoff_line(which_cap);
    // Both caps are hard caps, so the marker has to fit inside them. Room was
    // reserved in transcript() below; the guard here covers the degenerate
    // case of a cap smaller than the marker itself, where the honest outcome
    // is an unmarked (or empty) transcript rather than an over-cap file.
    const long size = static_cast<long>(warn_line.size());
    if ((max_bytes_ > 0 && file_bytes_ + size > max_bytes_) ||
        (max_bytes_per_run_ > 0 && run_bytes_ + size > max_bytes_per_run_))
        return;
    append_line(warn_line);
}

// AUDIT NOTE: bytes_written_ counts only what THIS process appended, not the
// size of a transcript file inherited from earlier runs. transcript_max_bytes
// therefore bounds one run's growth of the file, not the file's total size on
// disk -- SPEC.md §9.2's intent (a safety valve for a long interactive
// session) holds, but the file can exceed the cap across repeated runs.
void Logger::set_path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    file_bytes_ = existing_file_size(path_);
}

// The single place this class touches the filesystem. Both callers used to
// open, write and count bytes themselves, and neither looked at whether the
// write worked: with an unwritable transcript path TinyCA ran normally, said
// "new session: ...", and produced no diagnostics at all. On the target board
// the transcript is the only post-mortem available, so a lost one is reported
// -- once, because a failure that repeats per entry would bury the task's own
// output -- and the byte counters only advance on a write that succeeded.
void Logger::append_line(const std::string &line)
{
    std::ofstream f(path_, std::ios::app);
    f << line;
    f.flush();
    if (!f)
    {
        write_failed_ = true;
        if (!write_failure_warned_)
        {
            write_failure_warned_ = true;
            std::cerr << "warning: cannot write the transcript at " << path_
                      << " -- this run will leave no diagnostic record\n";
        }
        return;
    }
    file_bytes_ += static_cast<long>(line.size());
    run_bytes_ += static_cast<long>(line.size());
}

void Logger::transcript(const std::string &kind, const std::string &payload_json)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string line = "{\"ts\":" + std::to_string(now_unix_ms()) + ",\"kind\":\"" + json_escape(kind) +
                       "\",\"payload\":" + payload_json + "}\n";
    const long size = static_cast<long>(line.size());

    // The two caps are independent; the first one that would be exceeded
    // stops appending for the rest of the run (SPEC.md §9.2). Existing
    // entries are never rotated or trimmed -- the log stays append-only.
    // `reserve` keeps room for the cutoff marker so that neither cap is ever
    // exceeded by the act of reporting that it was reached.
    const long reserve = static_cast<long>(cutoff_line("transcript_max_bytes_per_run").size());
    if (max_bytes_ > 0 && file_bytes_ + size + reserve > max_bytes_)
    {
        write_cutoff_line("transcript_max_bytes");
        return;
    }
    if (max_bytes_per_run_ > 0 && run_bytes_ + size + reserve > max_bytes_per_run_)
    {
        write_cutoff_line("transcript_max_bytes_per_run");
        return;
    }

    append_line(line);
}

void Logger::log(LogLevel level, const std::string &message)
{
    if (static_cast<int>(level) > static_cast<int>(level_))
        return; // below the configured verbosity
    std::cerr << "[" << level_name(level) << "] " << message << std::endl;
}

LogLevel Logger::level_from_string(const std::string &s)
{
    if (s == "error")
        return LogLevel::Error;
    if (s == "warn")
        return LogLevel::Warn;
    if (s == "debug")
        return LogLevel::Debug;
    return LogLevel::Info;
}

const char *Logger::level_name(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
    default:
        return "info";
    }
}

} // namespace tinyca
