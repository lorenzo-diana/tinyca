// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/logger.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace tinyca
{

namespace
{
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

long now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

Logger::Logger(std::string transcript_path, long transcript_max_bytes, LogLevel level)
    : path_(std::move(transcript_path)), max_bytes_(transcript_max_bytes), level_(level)
{
}

void Logger::transcript(const std::string &kind, const std::string &payload_json)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string line = "{\"ts\":" + std::to_string(now_unix_ms()) + ",\"kind\":\"" + json_escape(kind) +
                       "\",\"payload\":" + payload_json + "}\n";

    if (max_bytes_ > 0 && bytes_written_ + static_cast<long>(line.size()) > max_bytes_)
    {
        if (!cutoff_warned_)
        {
            std::ofstream f(path_, std::ios::app);
            std::string warn_line = "{\"ts\":" + std::to_string(now_unix_ms()) +
                                    ",\"kind\":\"transcript_cutoff\",\"payload\":{\"reason\":"
                                    "\"transcript_max_bytes exceeded, further entries dropped\"}}\n";
            f << warn_line;
            bytes_written_ += static_cast<long>(warn_line.size());
            cutoff_warned_ = true;
        }
        return;
    }

    std::ofstream f(path_, std::ios::app);
    f << line;
    bytes_written_ += static_cast<long>(line.size());
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
