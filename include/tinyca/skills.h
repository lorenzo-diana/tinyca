// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

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
    // Always returns true -- a missing/unreadable skills_dir is reported via
    // `warnings`, not treated as fatal (an agent with no skills still runs).
    bool discover(const std::string &skills_dir, std::vector<std::string> &warnings);

    const std::vector<SkillInfo> &skills() const
    {
        return skills_;
    }

    // Tier 1: the always-resident system-prompt block, in SPEC.md §7's exact
    // format. Skills beyond max_tier1_skills are omitted (discover() already
    // warned if more were found than that cap allows).
    std::string render_tier1(size_t max_tier1_skills) const;

    // Tier 2: the full script body for `name`. False if unknown.
    bool load_tier2(const std::string &name, std::string &body) const;

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
