// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <mutex>
#include <string>

namespace tinyca
{

enum class LogLevel
{
    Error,
    Warn,
    Info,
    Debug
};

// Transcript: an append-only JSONL record of every request, response,
// command, and result (SPEC.md §9's "Thinking" note -- reasoning is always
// written here even when stripped from replayed history). Separately, a
// leveled console logger gated by `log_level` (SPEC.md §4.3).
class Logger
{
  public:
    Logger(std::string transcript_path, long transcript_max_bytes, LogLevel level);

    // Appends one line: {"ts":<unix_ms>,"kind":"<kind>","payload":<payload_json>}.
    // `payload_json` must already be valid JSON text (an object, typically).
    // Once transcript_max_bytes is exceeded, further entries are dropped
    // (one cutoff warning line written first) -- SPEC.md §9.2. 0 = unlimited.
    void transcript(const std::string &kind, const std::string &payload_json);

    // Console logging (stderr), gated by `level_` -- messages below the
    // configured level are dropped entirely, not written anywhere.
    void log(LogLevel level, const std::string &message);

    static LogLevel level_from_string(const std::string &s); // "error"|"warn"|"info"|"debug"
    static const char *level_name(LogLevel level);

  private:
    std::string path_;
    long max_bytes_;
    LogLevel level_;
    long bytes_written_ = 0;
    bool cutoff_warned_ = false;
    std::mutex mutex_;
};

} // namespace tinyca
