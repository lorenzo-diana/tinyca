// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/command_filter.h"

#include <cctype>

#include "tinyca/string_util.h"

namespace tinyca
{

bool contains_disallowed_metacharacter(const std::string &command)
{
    if (command.find("$(") != std::string::npos || command.find('`') != std::string::npos ||
        command.find('>') != std::string::npos || command.find('<') != std::string::npos)
        return true;
    // '&' is disallowed (backgrounding) but "&&" (logical AND, also a valid
    // segment separator below) is not -- remove every "&&" pair first, then
    // any remaining lone '&' is the real metacharacter.
    std::string without_and_and;
    for (size_t i = 0; i < command.size();)
    {
        if (command[i] == '&' && i + 1 < command.size() && command[i + 1] == '&')
        {
            i += 2;
            continue;
        }
        without_and_and += command[i];
        ++i;
    }
    return without_and_and.find('&') != std::string::npos;
}

std::vector<std::string> split_command_segments(const std::string &command)
{
    std::vector<std::string> segments;
    std::string current;
    for (size_t i = 0; i < command.size();)
    {
        if (command[i] == ';' || command[i] == '\n')
        {
            segments.push_back(current);
            current.clear();
            ++i;
        }
        else if (command[i] == '&' && i + 1 < command.size() && command[i + 1] == '&')
        {
            segments.push_back(current);
            current.clear();
            i += 2;
        }
        else if (command[i] == '|' && i + 1 < command.size() && command[i + 1] == '|')
        {
            segments.push_back(current);
            current.clear();
            i += 2;
        }
        else if (command[i] == '|')
        {
            segments.push_back(current);
            current.clear();
            ++i;
        }
        else
        {
            current += command[i];
            ++i;
        }
    }
    segments.push_back(current);
    return segments;
}

namespace
{
bool looks_like_assignment(const std::string &token)
{
    size_t eq = token.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(token[0])) && token[0] != '_')
        return false;
    for (size_t i = 1; i < eq; ++i)
        if (!std::isalnum(static_cast<unsigned char>(token[i])) && token[i] != '_')
            return false;
    return true;
}
} // namespace

std::string extract_head_token(const std::string &segment)
{
    std::string s = trim(segment);
    while (true)
    {
        size_t space = s.find_first_of(" \t");
        std::string first_token = (space == std::string::npos) ? s : s.substr(0, space);
        if (!looks_like_assignment(first_token))
            break;
        if (space == std::string::npos)
        {
            s.clear();
            break;
        }
        s = trim(s.substr(space + 1));
    }
    size_t space = s.find_first_of(" \t");
    std::string head = (space == std::string::npos) ? s : s.substr(0, space);
    if (starts_with(head, "./"))
        head = head.substr(2);
    return head;
}

FilterMode CommandFilter::mode_from_string(const std::string &s)
{
    if (s == "blacklist")
        return FilterMode::Blacklist;
    if (s == "none")
        return FilterMode::None;
    return FilterMode::Whitelist;
}

CommandFilter::CommandFilter(FilterMode mode, std::vector<std::string> whitelist, std::vector<std::string> blacklist,
                             bool allow_metacharacters, std::vector<std::string> auto_allowed_paths)
    : mode_(mode), whitelist_(std::move(whitelist)), blacklist_(std::move(blacklist)),
      allow_metacharacters_(allow_metacharacters), auto_allowed_(std::move(auto_allowed_paths))
{
    // Normalize auto-allowed skill paths the same way a head token is
    // normalized (strip a leading "./"), so "./skills/x.sh" and "skills/x.sh"
    // both match regardless of which form the model or the config used.
    for (auto &p : auto_allowed_)
        if (starts_with(p, "./"))
            p = p.substr(2);
}

FilterResult CommandFilter::check(const std::string &command) const
{
    FilterResult result;

    if (!allow_metacharacters_ && contains_disallowed_metacharacter(command))
    {
        result.allowed = false;
        result.reason = "command contains a disallowed metacharacter ($(), `, >, <, or &)";
        return result;
    }

    if (mode_ == FilterMode::None)
        return result; // allowed, no segmentation needed

    for (const auto &segment : split_command_segments(command))
    {
        std::string trimmed_segment = trim(segment);
        if (trimmed_segment.empty())
            continue; // e.g. a trailing ";" leaves an empty final segment
        std::string head = extract_head_token(segment);

        bool auto_allowed = false;
        for (const auto &p : auto_allowed_)
        {
            if (p == head)
            {
                auto_allowed = true;
                break;
            }
        }
        if (auto_allowed)
            continue;

        if (mode_ == FilterMode::Whitelist)
        {
            bool found = false;
            for (const auto &w : whitelist_)
            {
                if (w == head)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                result.allowed = false;
                result.offending_token = head;
                result.reason = "'" + head + "' is not in the whitelist";
                return result;
            }
        }
        else // Blacklist
        {
            for (const auto &b : blacklist_)
            {
                if (b == head)
                {
                    result.allowed = false;
                    result.offending_token = head;
                    result.reason = "'" + head + "' is on the blacklist";
                    return result;
                }
            }
        }
    }

    return result;
}

} // namespace tinyca
