// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/skills.h"

#include <algorithm>

#include "test_util.h"

namespace
{

using tinyca::SkillRegistry;

bool any_warning_contains(const std::vector<std::string>& warnings, const std::string& needle)
{
    for (const auto& w : warnings)
    {
        if (w.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

void test_discover_real_fetch_url_skill()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    TINYCA_CHECK(reg.discover("skills", warnings));
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(reg.skills()[0].name, std::string("fetch_url"));
    TINYCA_CHECK_EQ(reg.skills()[0].usage, std::string("skills/fetch_url.sh <url> <destination_folder>"));
}

void test_tier1_render_matches_spec_format_exactly()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);
    std::string expected = "Available skills (run them with the bash tool):\n"
                            "- fetch_url: Download a file from a URL into a destination folder using wget.\n"
                            "  usage: skills/fetch_url.sh <url> <destination_folder>\n";
    TINYCA_CHECK_EQ(reg.render_tier1(8), expected);
}

void test_discover_skips_missing_usage_not_executable_and_non_sh()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    TINYCA_CHECK(reg.discover("tests/fixtures/skills_test", warnings));

    // Only the two well-formed, executable .sh files get registered.
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(2));

    bool has_good = false;
    bool has_another = false;
    for (const auto& s : reg.skills())
    {
        if (s.name == "good_skill")
        {
            has_good = true;
        }
        if (s.name == "another_skill")
        {
            has_another = true;
        }
    }
    TINYCA_CHECK(has_good);
    TINYCA_CHECK(has_another);

    TINYCA_CHECK(any_warning_contains(warnings, "missing_usage.sh: missing @usage"));
    TINYCA_CHECK(any_warning_contains(warnings, "not_executable.sh: not executable"));
}

void test_discover_missing_dir_is_not_fatal()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    TINYCA_CHECK(reg.discover("tests/fixtures/does_not_exist_dir", warnings));
    TINYCA_CHECK(reg.skills().empty());
    TINYCA_CHECK(any_warning_contains(warnings, "skills_dir not found"));
}

void test_tier1_cap_at_max_tier1_skills()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("tests/fixtures/skills_test", warnings);
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(2));

    std::string rendered = reg.render_tier1(1);
    size_t entry_count = 0;
    size_t pos = 0;
    while ((pos = rendered.find("\n- ", pos)) != std::string::npos)
    {
        ++entry_count;
        pos += 1;
    }
    TINYCA_CHECK_EQ(entry_count, static_cast<size_t>(1));
}

void test_load_tier2_returns_full_body_for_known_skill_and_fails_for_unknown()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);

    std::string body;
    TINYCA_CHECK(reg.load_tier2("fetch_url", body));
    TINYCA_CHECK(body.find("#!/bin/bash") != std::string::npos);
    TINYCA_CHECK(body.find("wget") != std::string::npos);

    std::string unused;
    TINYCA_CHECK(!reg.load_tier2("does_not_exist", unused));
}

void test_paths_returns_all_discovered_skill_paths()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);
    auto paths = reg.paths();
    TINYCA_CHECK_EQ(paths.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(paths[0], std::string("skills/fetch_url.sh"));
}

void test_show_skill_trigger_found_on_its_own_line()
{
    std::string name;
    TINYCA_CHECK(SkillRegistry::find_show_skill_trigger("blah blah\nshow_skill: fetch_url\nmore text", name));
    TINYCA_CHECK_EQ(name, std::string("fetch_url"));
}

void test_show_skill_trigger_absent()
{
    std::string name;
    TINYCA_CHECK(!SkillRegistry::find_show_skill_trigger("no trigger here at all", name));
}

void run_all_tests()
{
    TINYCA_RUN(test_discover_real_fetch_url_skill);
    TINYCA_RUN(test_tier1_render_matches_spec_format_exactly);
    TINYCA_RUN(test_discover_skips_missing_usage_not_executable_and_non_sh);
    TINYCA_RUN(test_discover_missing_dir_is_not_fatal);
    TINYCA_RUN(test_tier1_cap_at_max_tier1_skills);
    TINYCA_RUN(test_load_tier2_returns_full_body_for_known_skill_and_fails_for_unknown);
    TINYCA_RUN(test_paths_returns_all_discovered_skill_paths);
    TINYCA_RUN(test_show_skill_trigger_found_on_its_own_line);
    TINYCA_RUN(test_show_skill_trigger_absent);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
