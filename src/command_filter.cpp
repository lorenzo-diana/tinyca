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

// Splits a command line on the separators `;`, `&&`, `||`, `|` and newline,
// skipping any that appear inside quotes. Quote handling mirrors bash closely
// enough for a head-token screen:
//   - inside '...' nothing is special, not even a backslash, until the next '
//   - inside "..." a backslash escapes the next character, and " closes
//   - outside quotes a backslash escapes the next character, and ' or " opens
// An unbalanced quote swallows the rest of the line as one segment, which is
// also what bash does with it -- so the screen sees the same thing the shell
// would, and nothing can hide behind a quote that never closes.
//
// Quote characters are KEPT in the segment text; extract_head_token only reads
// the first word, so `echo "a; b"` still yields the head token `echo`.
//
// Why this matters: without it, a separator inside a string started a bogus
// segment whose head token was data (`world"` in `echo "hello; world"`), and
// the whole command was rejected. That failed closed, so it was a usability
// defect rather than a hole -- but it also broke SPEC.md §7.1's screen of
// skill bodies, which cannot avoid quoted punctuation in messages.
std::vector<std::string> split_command_segments(const std::string &command)
{
    std::vector<std::string> segments;
    std::string current;
    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; i < command.size();)
    {
        const char c = command[i];

        if (in_single)
        {
            current += c;
            if (c == '\'')
                in_single = false;
            ++i;
            continue;
        }
        if (in_double)
        {
            if (c == '\\' && i + 1 < command.size())
            {
                current += c;
                current += command[i + 1];
                i += 2;
                continue;
            }
            current += c;
            if (c == '"')
                in_double = false;
            ++i;
            continue;
        }

        // Outside quotes.
        if (c == '\\' && i + 1 < command.size())
        {
            // An escaped separator is not a separator (`echo a\; b`).
            current += c;
            current += command[i + 1];
            i += 2;
            continue;
        }
        if (c == '\'')
        {
            in_single = true;
            current += c;
            ++i;
            continue;
        }
        if (c == '"')
        {
            in_double = true;
            current += c;
            ++i;
            continue;
        }
        if (c == ';' || c == '\n')
        {
            segments.push_back(current);
            current.clear();
            ++i;
            continue;
        }
        if ((c == '&' || c == '|') && i + 1 < command.size() && command[i + 1] == c)
        {
            segments.push_back(current); // && or ||
            current.clear();
            i += 2;
            continue;
        }
        if (c == '|')
        {
            segments.push_back(current);
            current.clear();
            ++i;
            continue;
        }
        current += c;
        ++i;
    }

    segments.push_back(current);
    return segments;
}

namespace
{
// Head-token membership test, shared by the auto-allowed/whitelist/blacklist
// checks below -- all three ask the same question of a different list.
bool contains_token(const std::vector<std::string> &list, const std::string &token)
{
    for (const auto &t : list)
        if (t == token)
            return true;
    return false;
}

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

namespace
{

// bash word-expands a command name before looking it up, so "rm", 'rm', \rm,
// r""m and $'rm' all invoke rm. The filter has to compare the program that
// will actually run, not the spelling the model typed -- otherwise a blacklist
// is bypassed by one pair of quotes.
//
// This removes quoting and backslash escapes; it deliberately does NOT expand
// variables or command substitution, because those are refused outright by the
// metacharacter check before any of this runs.
std::string unquote_word(const std::string &word)
{
    std::string out;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < word.size(); ++i)
    {
        const char c = word[i];
        if (in_single)
        {
            if (c == '\'')
                in_single = false;
            else
                out += c;
            continue;
        }
        if (in_double)
        {
            if (c == '\\' && i + 1 < word.size())
            {
                out += word[i + 1];
                ++i;
            }
            else if (c == '"')
                in_double = false;
            else
                out += c;
            continue;
        }
        // Outside quotes. `$'...'` and `$"..."` are ANSI-C / locale quoting:
        // the '$' is syntax, not part of the name.
        if (c == '$' && i + 1 < word.size() && (word[i + 1] == '\'' || word[i + 1] == '"'))
            continue;
        if (c == '\'')
            in_single = true;
        else if (c == '"')
            in_double = true;
        else if (c == '\\' && i + 1 < word.size())
        {
            out += word[i + 1];
            ++i;
        }
        else
            out += c;
    }
    return out;
}

// Last path component, for matching a program by name when it was invoked by
// path. Only the blacklist uses this -- see CommandFilter::check_tokens_only.
std::string basename_of(const std::string &token)
{
    const size_t slash = token.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= token.size())
        return token;
    return token.substr(slash + 1);
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
    head = unquote_word(head);
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

void CommandFilter::add_auto_allowed(const std::string &token)
{
    std::string normalized = token;
    if (starts_with(normalized, "./"))
        normalized = normalized.substr(2);
    if (!contains_token(auto_allowed_, normalized))
        auto_allowed_.push_back(normalized);
}

void CommandFilter::set_confirm_required(std::vector<std::string> tokens)
{
    confirm_required_.clear();
    for (auto &t : tokens)
    {
        if (starts_with(t, "./"))
            t = t.substr(2);
        confirm_required_.push_back(t);
    }
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

    result = check_tokens_only(command);
    if (!result.allowed)
        return result;

    // Confirmation is asked per *program*, so every segment is inspected --
    // "ls && add_skill.sh ..." must prompt just like a bare invocation.
    for (const auto &segment : split_command_segments(command))
    {
        if (contains_token(confirm_required_, extract_head_token(segment)))
        {
            result.needs_confirmation = true;
            break;
        }
    }
    return result;
}

FilterResult CommandFilter::check_tokens_only(const std::string &command) const
{
    FilterResult result;

    if (mode_ == FilterMode::None)
        return result; // allowed, no segmentation needed

    for (const auto &segment : split_command_segments(command))
    {
        std::string trimmed_segment = trim(segment);
        if (trimmed_segment.empty())
            continue; // e.g. a trailing ";" leaves an empty final segment
        std::string head = extract_head_token(segment);

        if (contains_token(auto_allowed_, head))
            continue; // skill paths are allowed in every mode (SPEC.md §7)

        // Whitelist: the token as written must be listed -- listing `ls` must
        // not bless /opt/anything/ls, so a path-qualified program fails closed.
        // Blacklist: match the basename too, because the blacklist names
        // programs and /bin/rm is the program `rm`.
        const bool rejected = (mode_ == FilterMode::Whitelist)
                                  ? !contains_token(whitelist_, head)
                                  : (contains_token(blacklist_, head) || contains_token(blacklist_, basename_of(head)));
        if (rejected)
        {
            result.allowed = false;
            result.offending_token = head;
            result.reason = (mode_ == FilterMode::Whitelist) ? "'" + head + "' is not in the whitelist"
                                                             : "'" + head + "' is on the blacklist";
            return result;
        }
    }

    return result;
}

} // namespace tinyca
