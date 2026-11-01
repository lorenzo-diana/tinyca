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
    // `transcript_max_bytes` caps the SIZE OF THE FILE (entries already in it
    // from previous runs included -- measured once at construction);
    // `transcript_max_bytes_per_run` caps only what THIS run appends. Either
    // 0 means unlimited; whichever is hit first stops further appends
    // (SPEC.md §9.2). load_config clamps per-run to the total so the two
    // cannot contradict each other.
    Logger(std::string transcript_path, long transcript_max_bytes, long transcript_max_bytes_per_run, LogLevel level);

    // Appends one line: {"ts":<unix_ms>,"kind":"<kind>","payload":<payload_json>}.
    // `payload_json` must already be valid JSON text (an object, typically).
    // Once either cap would be exceeded, further entries are dropped and one
    // cutoff line naming the cap that tripped is written -- SPEC.md §9.2.
    void transcript(const std::string &kind, const std::string &payload_json);

    // Points the logger at a different file, for a session renamed mid-run
    // (SPEC.md §12.3). The total-file budget is re-measured from the new
    // file; this run's own budget keeps counting, since the run did not
    // restart.
    void set_path(const std::string &path);

    // Console logging (stderr), gated by `level_` -- messages below the
    // configured level are dropped entirely, not written anywhere.
    void log(LogLevel level, const std::string &message);

    // True once any transcript write has failed (an unwritable path, a full
    // disk). The failure is also reported on stderr once; this accessor exists
    // so the condition can be asserted in a test rather than by scraping
    // stderr.
    bool write_failed() const
    {
        return write_failed_;
    }

    static LogLevel level_from_string(const std::string &s); // "error"|"warn"|"info"|"debug"
    static const char *level_name(LogLevel level);

  private:
    static std::string cutoff_line(const std::string &which_cap);
    void append_line(const std::string &line);            // caller holds mutex_
    void write_cutoff_line(const std::string &which_cap); // caller holds mutex_

    std::string path_;
    long max_bytes_;         // total file cap
    long max_bytes_per_run_; // this run's append cap
    LogLevel level_;
    long file_bytes_ = 0; // pre-existing size + what this run appended
    long run_bytes_ = 0;  // what this run appended
    bool cutoff_warned_ = false;
    bool write_failed_ = false;
    bool write_failure_warned_ = false;
    std::mutex mutex_;
};

} // namespace tinyca
