// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

#include "tinyca/command_filter.h"

namespace tinyca
{

struct SkillInfo
{
    std::string name;
    std::string description;
    std::string usage;
    std::string path;
};

// Two-tier skills (SPEC.md §7). Discovery scans a directory for executable
// *.sh files with a metadata header; a file missing @name/@description/
// @usage is skipped with a warning, not fatal.
class SkillRegistry
{
  public:
    // A missing/unreadable skills_dir is reported via `warnings`, not treated
    // as fatal (an agent with no skills still runs), so there is no failure
    // for a caller to handle.
    void discover(const std::string &skills_dir, std::vector<std::string> &warnings);

    const std::vector<SkillInfo> &skills() const
    {
        return skills_;
    }

    // Tier 1: the always-resident system-prompt block, in SPEC.md §7's exact
    // `excluded_names` are skipped: a skill can be runnable without being
    // advertised to the model (SPEC.md §7). They still count as discovered,
    // stay auto-allowed, and are still loadable through Tier 2.
    //
    // format. Skills beyond max_tier1_skills are omitted (discover() already
    // warned if more were found than that cap allows).
    std::string render_tier1(size_t max_tier1_skills, const std::vector<std::string> &excluded_names) const;

    // Tier 2: the full script body for `name`. False if unknown.
    bool load_tier2(const std::string &name, std::string &body) const;

    // Screens every discovered skill's body against `filter` and DROPS the
    // ones that invoke a program the operator's filter forbids (SPEC.md §7):
    // a skill is auto-allowed as a whole, so its contents would otherwise be
    // a way around the whitelist. Rejected skills are removed from the
    // registry entirely -- not merely hidden from the Tier 1 index -- and
    // reported through `rejections` as "name: reason".
    //
    // This is a token-level screen of each line's head, not a shell parser:
    // it catches a skill that runs a forbidden program, not every way a
    // script could misbehave. Shell keywords and variable assignments are
    // skipped; redirection and command substitution inside a script are not
    // treated as metacharacter violations (see
    // CommandFilter::check_tokens_only).
    void validate_bodies(const CommandFilter &filter, std::vector<std::string> &rejections);

    // The spellings a skill can be invoked as -- its path as written and its
    // bare filename, since skills resolve through the PATH entry
    // Executor::set_path_prepend adds (SPEC.md §7). Whatever treats a skill
    // specially (auto-allowing it, or demanding confirmation for it) has to
    // cover both, so the knowledge lives here once.
    static std::vector<std::string> invocation_tokens(const SkillInfo &skill);

    // invocation_tokens() for every discovered skill, flattened.
    std::vector<std::string> auto_allow_tokens() const;

    // invocation_tokens() for one skill by name; empty if there is no such
    // skill.
    std::vector<std::string> invocation_tokens_for(const std::string &name) const;

    // Every discovered skill's path, for the executor to auto-allow
    // regardless of command_filter_mode (SPEC.md §7's last paragraph).
    std::vector<std::string> paths() const;

    // SPEC.md §7's Tier 2 trigger: the model emits "show_skill: <name>" on a
    // line by itself. Returns true and sets `name` if found anywhere in
    // `content`.
    static bool find_show_skill_trigger(const std::string &content, std::string &name);

  private:
    std::vector<SkillInfo> skills_;
};

} // namespace tinyca
