// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/skills.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "tinyca/string_util.h"

namespace tinyca
{

namespace
{

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads the first few lines of `path` looking for "# @name:", "# @description:",
// and "# @usage:" markers (order and exact comment prefix don't matter, just
// that the marker text appears on the line). All three are required.
//
// The 6/14/7 offsets are strlen("@name:"), strlen("@description:") and
// strlen("@usage:"). Only the first 20 lines are scanned, so a header pushed
// below that by a long preamble reads as a missing field and the skill is
// skipped with a warning -- not silently half-registered.
bool parse_header(const std::string &path, SkillInfo &info, std::string &missing_field)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        missing_field = "file unreadable";
        return false;
    }

    std::string line;
    int lines_read = 0;
    const int max_header_lines = 20;
    while (lines_read < max_header_lines && std::getline(f, line))
    {
        ++lines_read;
        size_t pos;
        if ((pos = line.find("@name:")) != std::string::npos)
            info.name = trim(line.substr(pos + 6));
        else if ((pos = line.find("@description:")) != std::string::npos)
            info.description = trim(line.substr(pos + 14));
        else if ((pos = line.find("@usage:")) != std::string::npos)
            info.usage = trim(line.substr(pos + 7));
    }

    if (info.name.empty())
    {
        missing_field = "@name";
        return false;
    }
    if (info.description.empty())
    {
        missing_field = "@description";
        return false;
    }
    if (info.usage.empty())
    {
        missing_field = "@usage";
        return false;
    }
    return true;
}

// Shell syntax that is not a program invocation. A head token from this list
// says nothing about what the script runs, so it is skipped rather than
// checked against the whitelist. `eval`, `source` and `.` are deliberately
// ABSENT: they execute text this screen cannot see, so they must satisfy the
// operator's filter like any other program.
bool is_shell_keyword(const std::string &token)
{
    static const char *kKeywords[] = {
        "if",    "then",  "else",     "elif",     "fi",     "for",    "while",  "until", "do",    "done",   "case",
        "esac",  "in",    "function", "{",        "}",      "(",      ")",      "[",     "[[",    "]]",     "test",
        "true",  "false", "break",    "continue", "return", "exit",   "set",    "shift", "local", "export", "readonly",
        "unset", "trap",  "declare",  "typeset",  "time",   "coproc", "select", "wait",  "cd"};
    for (size_t i = 0; i < sizeof(kKeywords) / sizeof(kKeywords[0]); ++i)
        if (token == kKeywords[i])
            return true;
    return false;
}

// A token that could name a program. Shell syntax reaching the head-token
// position (glob patterns from a `case` arm, quoted data, a bare `VAR=value`,
// an option word) is not a command and says nothing about what the script
// runs, so it is skipped -- no real program name contains these characters.
bool looks_like_program_name(const std::string &token)
{
    if (token.empty() || token[0] == '-')
        return false;
    static const std::string kNotInNames = "*?[]{}()\"'$=|&<>;`\\";
    return token.find_first_of(kNotInNames) == std::string::npos;
}

// First forbidden program a script body invokes, or an empty string if the
// whole body passes. Comments, blank lines, shell keywords and non-command
// head tokens are skipped.
//
// Every segment of every line is checked, so `echo hi; rm -rf /` buried on
// line 2 of a skill is caught, not just the line's first command. Since the
// segmenter became quote-aware (SPEC.md §8.1) a separator inside a quoted
// message no longer starts a bogus segment, so the generous skipping above is
// about shell *syntax* reaching the head-token position, not about quoting.
//
// What it still cannot see: a program reached through indirection (`eval`,
// `source`, an interpreter given inline code). Those are deliberately NOT in
// the keyword skip list, so the interpreter itself must satisfy the filter --
// but a whitelisted interpreter can of course run anything (SPEC.md §8.1).
std::string first_rejected_token(const std::string &path, const CommandFilter &filter)
{
    std::ifstream f(path);
    if (!f.is_open())
        return "<unreadable>";
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;
        // Every segment is inspected, not just the line's first command.
        for (const auto &segment : split_command_segments(trimmed))
        {
            const std::string head = extract_head_token(segment);
            if (!looks_like_program_name(head) || is_shell_keyword(head))
                continue;
            FilterResult r = filter.check_tokens_only(head);
            if (!r.allowed)
                return head;
        }
    }
    return std::string();
}

} // namespace

void SkillRegistry::validate_bodies(const CommandFilter &filter, std::vector<std::string> &rejections)
{
    std::vector<SkillInfo> kept;
    for (const auto &s : skills_)
    {
        std::string bad = first_rejected_token(s.path, filter);
        if (bad.empty())
            kept.push_back(s);
        else
            rejections.push_back(s.name + ": runs '" + bad + "', which the command filter forbids");
    }
    skills_ = kept;
}

void SkillRegistry::discover(const std::string &skills_dir, std::vector<std::string> &warnings)
{
    skills_.clear();

    DIR *d = opendir(skills_dir.c_str());
    if (d == nullptr)
    {
        warnings.push_back("skills_dir not found or unreadable: " + skills_dir);
        return;
    }

    std::vector<std::string> filenames;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        if (!ends_with(name, ".sh"))
            continue;
        filenames.push_back(name);
    }
    closedir(d);
    std::sort(filenames.begin(), filenames.end()); // deterministic Tier 1 order

    for (const auto &fname : filenames)
    {
        std::string full_path = skills_dir;
        if (!full_path.empty() && full_path.back() != '/')
            full_path += '/';
        full_path += fname;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0)
        {
            warnings.push_back(full_path + ": cannot stat, skipped");
            continue;
        }
        if ((st.st_mode & S_IXUSR) == 0)
        {
            warnings.push_back(full_path + ": not executable, skipped");
            continue;
        }

        SkillInfo info;
        info.path = full_path;
        std::string missing_field;
        if (!parse_header(full_path, info, missing_field))
        {
            warnings.push_back(full_path + ": missing " + missing_field + ", skipped");
            continue;
        }
        skills_.push_back(info);
    }
}

std::string SkillRegistry::render_tier1(size_t max_tier1_skills, const std::vector<std::string> &excluded_names) const
{
    std::string out = "Available skills (run them with the bash tool):\n";
    size_t listed = 0;
    for (size_t i = 0; i < skills_.size() && listed < max_tier1_skills; ++i)
    {
        bool excluded = false;
        for (const auto &name : excluded_names)
            if (skills_[i].name == name)
                excluded = true;
        if (excluded)
            continue; // runnable, deliberately not advertised (SPEC.md §7)
        out += "- " + skills_[i].name + ": " + skills_[i].description + "\n";
        out += "  usage: " + skills_[i].usage + "\n";
        ++listed;
    }
    return out;
}

bool SkillRegistry::load_tier2(const std::string &name, std::string &body) const
{
    for (const auto &s : skills_)
    {
        if (s.name == name)
        {
            std::ifstream f(s.path);
            if (!f.is_open())
                return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
            return true;
        }
    }
    return false;
}

std::vector<std::string> SkillRegistry::invocation_tokens(const SkillInfo &skill)
{
    std::vector<std::string> out;
    out.push_back(skill.path);
    const size_t slash = skill.path.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < skill.path.size())
        out.push_back(skill.path.substr(slash + 1));
    return out;
}

std::vector<std::string> SkillRegistry::auto_allow_tokens() const
{
    std::vector<std::string> out;
    for (const auto &s : skills_)
    {
        const std::vector<std::string> tokens = invocation_tokens(s);
        out.insert(out.end(), tokens.begin(), tokens.end());
    }
    return out;
}

std::vector<std::string> SkillRegistry::invocation_tokens_for(const std::string &name) const
{
    for (const auto &s : skills_)
        if (s.name == name)
            return invocation_tokens(s);
    return std::vector<std::string>();
}

std::vector<std::string> SkillRegistry::paths() const
{
    std::vector<std::string> out;
    for (const auto &s : skills_)
        out.push_back(s.path);
    return out;
}

bool SkillRegistry::find_show_skill_trigger(const std::string &content, std::string &name)
{
    std::istringstream iss(content);
    std::string line;
    const std::string marker = "show_skill:";
    while (std::getline(iss, line))
    {
        std::string trimmed = trim(line);
        if (starts_with(trimmed, marker))
        {
            std::string candidate = trim(trimmed.substr(marker.size()));
            if (!candidate.empty())
            {
                name = candidate;
                return true;
            }
        }
    }
    return false;
}

} // namespace tinyca
