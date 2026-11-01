// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

namespace tinyca
{

// Named sessions (SPEC.md §12.3). A session IS its transcript: the file
// <sessions_dir>/<name>.jsonl. There is deliberately no index file --
// the directory is the registry, so "is this name taken" is one stat() and
// cannot drift out of sync with reality, and a session can be archived or
// deleted with mv/rm.
struct SessionInfo
{
    std::string name;
    std::string path;
    long bytes = 0;
    long mtime_unix = 0;
    int entries = 0; // transcript lines
    int tasks = 0;   // user_task entries, i.e. things the user asked for
};

// Names double as filenames, so the charset is restricted the same way
// add_skill restricts skill names: letters, digits, '_', '-' and '.', nothing
// that could traverse a path, need quoting, or be rejected by a non-POSIX
// filesystem. Empty is invalid, and so is a leading '.'.
bool valid_session_name(const std::string &name);

// <sessions_dir>/<name>.jsonl, with exactly one separator.
std::string session_path(const std::string &sessions_dir, const std::string &name);

bool session_exists(const std::string &sessions_dir, const std::string &name);

// Default name for a session nobody named:
// "YYYY-MM-DD_hh-mm-ss_<work dir basename>", e.g.
// "2026-11-01_17-36-04_workspace". Local time, because the name is for a human
// reading a listing. `now_unix` is passed in rather than read from the clock so
// the result is testable. The basename is sanitised to the valid charset; a
// work_dir with no usable basename ("/", ".") yields "workdir".
std::string default_session_name(const std::string &work_dir, long now_unix);

// `base`, or `base-2`, `base-3`, ... -- the first form not already taken.
// Needed because the default name is per day and per directory, so a second
// session started the same day in the same place would otherwise collide.
std::string unique_session_name(const std::string &sessions_dir, const std::string &base);

// Renames a stored session's transcript. Refuses (false, `err` set) on an
// unusable new name, a name already in use -- names are unique across stored
// sessions -- or a missing source. Uses rename(2), so it is atomic within the
// directory and cannot half-finish.
bool rename_session(const std::string &sessions_dir, const std::string &from, const std::string &to, std::string &err);

// Every *.jsonl in `sessions_dir`, newest first. A missing directory is not an
// error: it just has no sessions yet.
std::vector<SessionInfo> list_sessions(const std::string &sessions_dir);

} // namespace tinyca
