// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/skills.h"

#include <algorithm>

#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "test_util.h"
#include "tinyca/executor.h"
#include "tinyca/string_util.h"

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

// Looks up a discovered skill by name; the shipped set grows over time, so
// tests assert on the skill they care about rather than on a position.
const tinyca::SkillInfo* find_skill(const SkillRegistry& reg, const std::string& name)
{
    for (const auto& s : reg.skills())
    {
        if (s.name == name)
        {
            return &s;
        }
    }
    return nullptr;
}

void test_discover_real_fetch_url_skill()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);
    const tinyca::SkillInfo* fetch = find_skill(reg, "fetch_url");
    TINYCA_CHECK(fetch != nullptr);
    TINYCA_CHECK_EQ(fetch->usage, std::string("fetch_url.sh <url> <destination_folder>"));
    // The skill that creates skills ships alongside it (SPEC.md §7.1).
    TINYCA_CHECK(find_skill(reg, "add_skill") != nullptr);
    // The non-.sh template is not a skill.
    TINYCA_CHECK(find_skill(reg, ".skill_template") == nullptr);
}

void test_tier1_render_matches_spec_format_exactly()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);
    // SPEC.md §7's exact shape: a header line, then "- name: description"
    // and an indented "  usage: ..." per skill, in sorted filename order.
    std::string expected = "Available skills (run them with the bash tool):\n"
                            "- add_skill: Create a new reusable skill from one bash command line, callable by name "
                            "afterwards.\n"
                            "  usage: add_skill.sh <name> <description> <usage> <command line...>\n"
                            "- fetch_url: Download a file from a URL into a destination folder using wget.\n"
                            "  usage: fetch_url.sh <url> <destination_folder>\n";
    TINYCA_CHECK_EQ(reg.render_tier1(8, std::vector<std::string>()), expected);
}

void test_discover_skips_missing_usage_not_executable_and_non_sh()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("tests/fixtures/skills_test", warnings);

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
    reg.discover("tests/fixtures/does_not_exist_dir", warnings);
    TINYCA_CHECK(reg.skills().empty());
    TINYCA_CHECK(any_warning_contains(warnings, "skills_dir not found"));
}

void test_tier1_cap_at_max_tier1_skills()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("tests/fixtures/skills_test", warnings);
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(2));

    std::string rendered = reg.render_tier1(1, std::vector<std::string>());
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
    TINYCA_CHECK_EQ(paths.size(), reg.skills().size());
    bool has_fetch_url = false;
    for (const auto& p : paths)
    {
        if (p == "skills/fetch_url.sh")
        {
            has_fetch_url = true;
        }
    }
    TINYCA_CHECK(has_fetch_url);
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

// 13(a): skills are invoked by bare filename (Executor puts skills_dir on the
// child's PATH), so the filter must auto-allow both spellings -- the path as
// discovered and the bare name the model actually types.
void test_auto_allow_tokens_cover_path_and_bare_name()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);

    std::vector<std::string> tokens = reg.auto_allow_tokens();
    bool has_path = false;
    bool has_bare = false;
    for (const auto& t : tokens)
    {
        if (t == "skills/fetch_url.sh")
        {
            has_path = true;
        }
        if (t == "fetch_url.sh")
        {
            has_bare = true;
        }
    }
    TINYCA_CHECK(has_path);
    TINYCA_CHECK(has_bare);
}

// SPEC.md §7.1: a skill is auto-allowed as a whole, so its body is screened
// against the operator's filter -- otherwise writing a skill would be a way
// around the whitelist. Guards both directions: a forbidden program hidden
// after a separator on line 2 must be caught, and shell syntax (keywords,
// case globs, assignments) must not cause a false rejection.
void test_validate_bodies_rejects_forbidden_programs()
{
    const std::string dir = "build/tests/skills_screen";
    TINYCA_CHECK_EQ(system(("rm -rf " + dir + " && mkdir -p " + dir).c_str()), 0);

    std::ofstream good(dir + "/good.sh");
    good << "#!/bin/bash\n# @name: good\n# @description: Lists files.\n# @usage: good.sh\n"
            "set -euo pipefail\nname=\"$1\"\ncase \"$name\" in\n  *[!a-z]*) exit 2 ;;\nesac\n"
            "ls -la\n";
    good.close();
    std::ofstream bad(dir + "/bad.sh");
    bad << "#!/bin/bash\n# @name: bad\n# @description: Looks harmless.\n# @usage: bad.sh\n"
           "echo starting\necho cleanup; rm -rf /tmp/whatever\n";
    bad.close();
    TINYCA_CHECK_EQ(system(("chmod +x " + dir + "/good.sh " + dir + "/bad.sh").c_str()), 0);

    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover(dir, warnings);
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(2));

    const std::string wl = "ls,cat,echo,mkdir,cp";
    tinyca::CommandFilter filter(tinyca::FilterMode::Whitelist, tinyca::split(wl, ','), {}, false,
                                 reg.auto_allow_tokens());
    std::vector<std::string> rejections;
    reg.validate_bodies(filter, rejections);

    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(reg.skills()[0].name, std::string("good"));
    TINYCA_CHECK_EQ(rejections.size(), static_cast<size_t>(1));
    TINYCA_CHECK(rejections[0].find("bad: runs 'rm'") != std::string::npos);
}

// The shipped skills must survive their own screen under the DEFAULT
// whitelist, or TinyCA would start by disabling its own capabilities.
void test_shipped_skills_pass_the_default_screen()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);
    const size_t discovered = reg.skills().size();

    const std::string wl = "ls,cat,grep,find,wc,head,tail,pwd,echo,mkdir,cp,mv,touch,make,gcc,g++,python3,git,"
                            "sed,awk,diff,wget";
    tinyca::CommandFilter filter(tinyca::FilterMode::Whitelist, tinyca::split(wl, ','), {}, false,
                                 reg.auto_allow_tokens());
    std::vector<std::string> rejections;
    reg.validate_bodies(filter, rejections);

    TINYCA_CHECK_EQ(rejections.size(), static_cast<size_t>(0));
    TINYCA_CHECK_EQ(reg.skills().size(), discovered);
}

// add_skill.sh must produce a file that TinyCA's own discovery accepts:
// executable, with all three header fields, runnable by bare name.
void test_add_skill_creates_a_discoverable_skill()
{
    const std::string dir = "build/tests/skills_created";
    TINYCA_CHECK_EQ(system(("rm -rf " + dir + " && mkdir -p " + dir + " && cp skills/.skill_template " + dir +
                            "/.skill_template")
                               .c_str()),
                    0);

    tinyca::Executor executor("/tmp", 10, 40, 40, 4096);
    char cwd[4096];
    TINYCA_CHECK(getcwd(cwd, sizeof(cwd)) != nullptr);
    executor.set_path_prepend(std::string(cwd) + "/skills");
    executor.set_env("TINYCA_SKILLS_DIR", std::string(cwd) + "/" + dir);

    tinyca::ExecResult r = executor.run("add_skill.sh count_files \"Count files here.\" \"count_files.sh\" ls -1");
    TINYCA_CHECK_EQ(r.exit_code, 0);
    TINYCA_CHECK(r.output.find("created skill 'count_files'") != std::string::npos);

    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover(dir, warnings);
    TINYCA_CHECK_EQ(reg.skills().size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(reg.skills()[0].name, std::string("count_files"));
    TINYCA_CHECK_EQ(reg.skills()[0].description, std::string("Count files here."));
    TINYCA_CHECK_EQ(reg.skills()[0].usage, std::string("count_files.sh"));

    // Created executable (no chmod available under the default whitelist), so
    // it runs immediately.
    tinyca::Executor runner("/tmp", 10, 40, 40, 4096);
    runner.set_path_prepend(std::string(cwd) + "/" + dir);
    TINYCA_CHECK_EQ(runner.run("count_files.sh").exit_code, 0);

    // A second attempt with the same name must refuse rather than overwrite.
    tinyca::ExecResult dup =
        executor.run("add_skill.sh count_files \"Again.\" \"count_files.sh\" ls -1");
    TINYCA_CHECK_EQ(dup.exit_code, 2);
    TINYCA_CHECK(dup.output.find("already exists") != std::string::npos);

    // An unusable name is refused too (it would become a filename).
    tinyca::ExecResult bad_name =
        executor.run("add_skill.sh ../escape \"Bad.\" \"escape.sh\" ls");
    TINYCA_CHECK_EQ(bad_name.exit_code, 2);
}

// SPEC.md §7/§16: a skill can be runnable without being advertised. Guards the
// measured regression where a 1.5B model, shown add_skill in the index,
// abandoned its task to register a junk skill instead of doing the work.
void test_tier1_exclude_hides_a_skill_without_disabling_it()
{
    SkillRegistry reg;
    std::vector<std::string> warnings;
    reg.discover("skills", warnings);

    std::vector<std::string> excluded;
    excluded.push_back("add_skill");
    const std::string rendered = reg.render_tier1(8, excluded);

    TINYCA_CHECK(rendered.find("add_skill") == std::string::npos);
    TINYCA_CHECK(rendered.find("fetch_url") != std::string::npos);

    // Still discovered, still auto-allowed, still loadable via Tier 2 --
    // excluded from the prompt only.
    TINYCA_CHECK(find_skill(reg, "add_skill") != nullptr);
    bool auto_allowed = false;
    for (const auto& t : reg.auto_allow_tokens())
    {
        if (t == "add_skill.sh")
        {
            auto_allowed = true;
        }
    }
    TINYCA_CHECK(auto_allowed);
    std::string body;
    TINYCA_CHECK(reg.load_tier2("add_skill", body));

    // The cap counts only what is actually listed: with one of two skills
    // excluded and a cap of 1, the survivor is still listed.
    const std::string capped = reg.render_tier1(1, excluded);
    TINYCA_CHECK(capped.find("fetch_url") != std::string::npos);
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
    TINYCA_RUN(test_auto_allow_tokens_cover_path_and_bare_name);
    TINYCA_RUN(test_tier1_exclude_hides_a_skill_without_disabling_it);
    TINYCA_RUN(test_validate_bodies_rejects_forbidden_programs);
    TINYCA_RUN(test_shipped_skills_pass_the_default_screen);
    TINYCA_RUN(test_add_skill_creates_a_discoverable_skill);
    TINYCA_RUN(test_show_skill_trigger_found_on_its_own_line);
    TINYCA_RUN(test_show_skill_trigger_absent);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
