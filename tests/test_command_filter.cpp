// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/command_filter.h"

#include "test_util.h"

namespace
{

using tinyca::CommandFilter;
using tinyca::FilterMode;

std::vector<std::string> default_whitelist()
{
    return {"ls", "cat", "grep", "find", "wc", "head", "tail", "pwd", "echo", "mkdir",
            "cp",  "mv",  "touch", "make", "gcc",  "g++",  "python3", "git",  "sed", "awk", "diff", "wget"};
}

std::vector<std::string> default_blacklist()
{
    return {"rm", "dd", "mkfs", "shutdown", "reboot", "sudo", "su", "chmod", "chown", "kill", "pkill",
            "killall", "mount", "umount"};
}

// ---- metacharacter rejection (unconditional, every mode) ----

void test_metacharacters_rejected_in_whitelist_mode()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    TINYCA_CHECK(!f.check("ls; rm -rf /").allowed); // ';' is a separator, not a metachar -- caught by segment check
    TINYCA_CHECK(!f.check("echo hi > /etc/passwd").allowed);
    TINYCA_CHECK(!f.check("cat $(whoami)").allowed);
    TINYCA_CHECK(!f.check("echo `id`").allowed);
    TINYCA_CHECK(!f.check("sleep 10 &").allowed);
    TINYCA_CHECK(!f.check("cat < /etc/shadow").allowed);
}

void test_double_ampersand_is_not_a_metacharacter()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    auto r = f.check("ls && pwd");
    TINYCA_CHECK(r.allowed);
}

void test_metacharacters_allowed_when_configured()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, true, {});
    TINYCA_CHECK(f.check("echo hi > /tmp/out").allowed);
}

// ---- whitelist mode: the classic bypass must be caught ----

void test_whitelist_rejects_ls_semicolon_rm()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    auto r = f.check("ls; rm -rf /");
    TINYCA_CHECK(!r.allowed);
    TINYCA_CHECK_EQ(r.offending_token, std::string("rm"));
}

void test_whitelist_allows_known_commands_and_pipes()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    TINYCA_CHECK(f.check("ls -la").allowed);
    TINYCA_CHECK(f.check("cat file.txt | grep foo").allowed);
    TINYCA_CHECK(f.check("make && echo done").allowed);
}

void test_whitelist_rejects_unknown_head_token()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    auto r = f.check("curl http://example.com");
    TINYCA_CHECK(!r.allowed);
    TINYCA_CHECK_EQ(r.offending_token, std::string("curl"));
}

void test_whitelist_strips_leading_var_assignments()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {});
    TINYCA_CHECK(f.check("FOO=1 BAR=2 echo hi").allowed);
}

// ---- blacklist mode ----

void test_blacklist_rejects_rm_allows_everything_else()
{
    CommandFilter f(FilterMode::Blacklist, {}, default_blacklist(), false, {});
    TINYCA_CHECK(!f.check("rm -rf /tmp/x").allowed);
    TINYCA_CHECK(f.check("curl http://example.com").allowed); // not blacklisted
    TINYCA_CHECK(f.check("ls -la").allowed);
}

void test_blacklist_catches_second_segment()
{
    CommandFilter f(FilterMode::Blacklist, {}, default_blacklist(), false, {});
    auto r = f.check("ls && rm -rf /");
    TINYCA_CHECK(!r.allowed);
    TINYCA_CHECK_EQ(r.offending_token, std::string("rm"));
}

// ---- none mode ----

void test_none_mode_allows_everything_except_metacharacters()
{
    CommandFilter f(FilterMode::None, {}, {}, false, {});
    TINYCA_CHECK(f.check("anything; rm -rf /; whatever").allowed);
    TINYCA_CHECK(!f.check("echo hi > /etc/passwd").allowed); // metachar check still applies
}

// ---- auto-allowed skill paths, regardless of mode ----

void test_skill_path_auto_allowed_in_whitelist_mode_even_with_dot_slash()
{
    CommandFilter f(FilterMode::Whitelist, default_whitelist(), {}, false, {"skills/fetch_url.sh"});
    TINYCA_CHECK(f.check("./skills/fetch_url.sh http://x y").allowed);
    TINYCA_CHECK(f.check("skills/fetch_url.sh http://x y").allowed);
}

void test_skill_path_auto_allowed_in_blacklist_mode()
{
    // Pathological case: a skill path that happens to share a name with a
    // blacklisted token is still allowed, since it's a path, not a bare command.
    CommandFilter f(FilterMode::Blacklist, {}, {"rm"}, false, {"skills/rm.sh"});
    TINYCA_CHECK(f.check("skills/rm.sh").allowed);
}

// ---- mode_from_string ----

void test_mode_from_string()
{
    TINYCA_CHECK(CommandFilter::mode_from_string("whitelist") == FilterMode::Whitelist);
    TINYCA_CHECK(CommandFilter::mode_from_string("blacklist") == FilterMode::Blacklist);
    TINYCA_CHECK(CommandFilter::mode_from_string("none") == FilterMode::None);
    TINYCA_CHECK(CommandFilter::mode_from_string("garbage") == FilterMode::Whitelist); // safe default
}

void run_all_tests()
{
    TINYCA_RUN(test_metacharacters_rejected_in_whitelist_mode);
    TINYCA_RUN(test_double_ampersand_is_not_a_metacharacter);
    TINYCA_RUN(test_metacharacters_allowed_when_configured);
    TINYCA_RUN(test_whitelist_rejects_ls_semicolon_rm);
    TINYCA_RUN(test_whitelist_allows_known_commands_and_pipes);
    TINYCA_RUN(test_whitelist_rejects_unknown_head_token);
    TINYCA_RUN(test_whitelist_strips_leading_var_assignments);
    TINYCA_RUN(test_blacklist_rejects_rm_allows_everything_else);
    TINYCA_RUN(test_blacklist_catches_second_segment);
    TINYCA_RUN(test_none_mode_allows_everything_except_metacharacters);
    TINYCA_RUN(test_skill_path_auto_allowed_in_whitelist_mode_even_with_dot_slash);
    TINYCA_RUN(test_skill_path_auto_allowed_in_blacklist_mode);
    TINYCA_RUN(test_mode_from_string);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
