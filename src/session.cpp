// BSD 3-Clause License — see LICENSE at the repository root.
//
// Session naming and the session directory (SPEC.md §12.3). This unit owns the
// mapping between a name and a file and nothing else: it never opens a
// transcript for writing (Logger does) and never interprets its contents
// (resume.cpp does) -- it only counts lines for the listing.
#include "tinyca/session.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace tinyca
{

namespace
{

// Deliberately no ':' -- the default name carries a clock time, written
// hh-mm-ss so a session file stays copyable onto FAT/exFAT/Windows volumes
// (an SD card, say) without renaming it first.
bool is_valid_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
}

bool ends_with_jsonl(const std::string &name)
{
    const std::string suffix = ".jsonl";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Counts transcript lines and how many of them are user_task entries. Read as
// text rather than parsed as JSON: this is for a listing, and a cheap scan
// cannot be tripped by a truncated final line.
//
// The kind is matched allowing optional whitespace around the colon and the
// quotes, because a plain substring test for the exact bytes Logger happens to
// write silently reported "0 task(s)" for any transcript serialised with a
// space after the colon -- a wrong number that reads like a real one.
bool line_has_kind(const std::string &line, const std::string &kind)
{
    size_t pos = line.find("\"kind\"");
    if (pos == std::string::npos)
        return false;
    pos += 6; // past "kind"
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    if (pos >= line.size() || line[pos] != ':')
        return false;
    ++pos;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    const std::string quoted = "\"" + kind + "\"";
    return line.compare(pos, quoted.size(), quoted) == 0;
}

void count_entries(const std::string &path, int &entries, int &tasks)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        ++entries;
        if (line_has_kind(line, "user_task"))
            ++tasks;
    }
}

} // namespace

bool valid_session_name(const std::string &name)
{
    if (name.empty())
        return false;
    // "." and ".." would resolve to a directory, and a leading '.' hides the
    // file from the listing below for no good reason.
    if (name[0] == '.')
        return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (!is_valid_name_char(name[i]))
            return false;
    return true;
}

std::string session_path(const std::string &sessions_dir, const std::string &name)
{
    std::string dir = sessions_dir;
    if (!dir.empty() && dir[dir.size() - 1] == '/')
        dir.erase(dir.size() - 1);
    return dir + "/" + name + ".jsonl";
}

bool session_exists(const std::string &sessions_dir, const std::string &name)
{
    struct stat st;
    return stat(session_path(sessions_dir, name).c_str(), &st) == 0;
}

std::string default_session_name(const std::string &work_dir, long now_unix)
{
    char date[32] = {0};
    const time_t when = static_cast<time_t>(now_unix);
    struct tm local;
    // Local time, not UTC: the name is for a human reading a listing, and
    // "which day was this" means their day.
    if (localtime_r(&when, &local) != nullptr)
        strftime(date, sizeof(date), "%Y-%m-%d_%H-%M-%S", &local);

    // Basename of the work dir, trailing slashes ignored.
    std::string dir = work_dir;
    while (dir.size() > 1 && dir[dir.size() - 1] == '/')
        dir.erase(dir.size() - 1);
    size_t slash = dir.find_last_of('/');
    std::string base = (slash == std::string::npos) ? dir : dir.substr(slash + 1);

    std::string sanitised;
    for (size_t i = 0; i < base.size(); ++i)
        sanitised += is_valid_name_char(base[i]) ? base[i] : '_';
    while (!sanitised.empty() && sanitised[0] == '.')
        sanitised.erase(0, 1); // "." / ".." / hidden dirs
    if (sanitised.empty())
        sanitised = "workdir";

    return std::string(date) + "_" + sanitised;
}

// AUDIT NOTE: claiming creates the file, so a run that dies during the rest of
// its startup (a missing grammar file, say) leaves an empty session behind. It
// shows up honestly in --list-sessions as 0 tasks / 0 entries / 0 bytes and
// costs an inode; that is preferred over leaving the race open.
//
// AUDIT NOTE: the search gives up after 999 attempts and returns
// "<base>-999", which may already exist -- that run would then append to an
// existing session's transcript instead of starting its own. Reaching it
// requires 999 sessions whose names collide, i.e. 999 runs started in the same
// second in the same work directory, so it is a deliberate "never fail a run
// over this" fallback rather than a case worth erroring on.
std::string unique_session_name(const std::string &sessions_dir, const std::string &base)
{
    // The name is CLAIMED, not just checked: O_CREAT|O_EXCL creates the empty
    // transcript atomically, so two TinyCA processes starting in the same
    // second in the same work directory cannot pick the same name and
    // interleave their turns into one file. A plain "does it exist" test left
    // that race open.
    for (int n = 1; n < 1000; ++n)
    {
        const std::string candidate = (n == 1) ? base : base + "-" + std::to_string(n);
        const int fd = open(session_path(sessions_dir, candidate).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0)
        {
            close(fd);
            return candidate;
        }
        if (errno != EEXIST)
            break; // unwritable directory, say: fall through to the base name
                   // -- Logger reports the resulting write failure (logger.cpp)
    }
    // Pathological (999 collisions, or a directory we cannot write): return
    // the base name and let the caller fail on the real I/O error instead of
    // inventing a name.
    return base;
}

bool rename_session(const std::string &sessions_dir, const std::string &from, const std::string &to, std::string &err)
{
    if (!valid_session_name(to))
    {
        err = "'" + to + "' is not a usable session name (letters, digits, '_', '-', '.' only)";
        return false;
    }
    if (from == to)
    {
        err = "the session is already called '" + to + "'";
        return false;
    }
    if (!session_exists(sessions_dir, from))
    {
        err = "no stored session named '" + from + "'";
        return false;
    }
    if (session_exists(sessions_dir, to))
    {
        err = "a session named '" + to + "' already exists";
        return false;
    }
    const std::string from_path = session_path(sessions_dir, from);
    const std::string to_path = session_path(sessions_dir, to);
    if (std::rename(from_path.c_str(), to_path.c_str()) != 0)
    {
        err = "could not rename " + from_path + " to " + to_path;
        return false;
    }
    return true;
}

std::vector<SessionInfo> list_sessions(const std::string &sessions_dir)
{
    std::vector<SessionInfo> out;
    DIR *d = opendir(sessions_dir.c_str());
    if (d == nullptr)
        return out; // no sessions yet is not an error

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr)
    {
        const std::string filename = entry->d_name;
        if (filename.empty() || filename[0] == '.' || !ends_with_jsonl(filename))
            continue;

        SessionInfo info;
        info.name = filename.substr(0, filename.size() - 6); // strip ".jsonl"
        info.path = session_path(sessions_dir, info.name);

        struct stat st;
        if (stat(info.path.c_str(), &st) != 0)
            continue;
        info.bytes = static_cast<long>(st.st_size);
        info.mtime_unix = static_cast<long>(st.st_mtime);
        count_entries(info.path, info.entries, info.tasks);
        out.push_back(info);
    }
    closedir(d);

    // Newest first: the session you want is almost always the last one used.
    std::sort(out.begin(), out.end(),
              [](const SessionInfo &a, const SessionInfo &b) { return a.mtime_unix > b.mtime_unix; });
    return out;
}

} // namespace tinyca
