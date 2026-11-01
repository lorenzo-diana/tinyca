// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

namespace tinyca
{

enum class FilterMode
{
    Whitelist,
    Blacklist,
    None
};

struct FilterResult
{
    bool allowed = true;
    std::string offending_token; // set when allowed == false
    // Set when the command is allowed but the operator asked to confirm this
    // particular program before every run (SPEC.md §8.1) -- used for the
    // skill that creates other skills, whose effect outlives the command.
    bool needs_confirmation = false;
    std::string reason; // human-readable, for the tool message / confirm prompt
};

// SPEC.md §8.1. Metacharacter rejection is unconditional and runs first in
// every mode; whitelist/blacklist additionally segment the command and check
// every segment's head token -- a check on the first token alone would let
// `ls; rm -rf /` through.
class CommandFilter
{
  public:
    CommandFilter(FilterMode mode, std::vector<std::string> whitelist, std::vector<std::string> blacklist,
                  bool allow_metacharacters, std::vector<std::string> auto_allowed_paths);

    FilterResult check(const std::string &command) const;

    // Same token checks as check(), but without the metacharacter rule: a
    // *script* legitimately contains redirection and command substitution,
    // while the programs it invokes still have to satisfy the operator's
    // whitelist/blacklist. Used to screen skill bodies (SPEC.md §7).
    FilterResult check_tokens_only(const std::string &command) const;

    // Registers one more token (path or bare filename) as auto-allowed, for a
    // skill that appeared after startup.
    void add_auto_allowed(const std::string &token);

    // Programs that must be confirmed by the user before every execution even
    // though the filter allows them.
    void set_confirm_required(std::vector<std::string> tokens);

    static FilterMode mode_from_string(const std::string &s); // "whitelist"|"blacklist"|"none"

  private:
    FilterMode mode_;
    std::vector<std::string> whitelist_;
    std::vector<std::string> blacklist_;
    bool allow_metacharacters_;
    std::vector<std::string> auto_allowed_;
    std::vector<std::string> confirm_required_;
};

// Exposed for direct testing; also used internally by check().
bool contains_disallowed_metacharacter(const std::string &command);
std::vector<std::string> split_command_segments(const std::string &command);
std::string extract_head_token(const std::string &segment);

} // namespace tinyca
