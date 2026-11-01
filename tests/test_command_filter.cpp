// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/command_filter.h"

#include "test_util.h"
#include "tinyca/string_util.h"

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

// Defect 12: the segmenter is quote-aware, so a separator INSIDE a quoted
// string no longer starts a bogus segment. This is the one change in this
// project that *expands* what runs, so the adversarial cases come first and
// stay here: anything that could smuggle a forbidden program past the filter
// must still be rejected.
void test_quoted_separators_do_not_split_segments()
{
    tinyca::CommandFilter f(tinyca::FilterMode::Whitelist, tinyca::split("ls,cat,echo,grep,wc,find", ','), {}, false,
                            {});

    // --- previously false rejections: the separator is data, not syntax ---
    TINYCA_CHECK(f.check("echo \"hello; world\"").allowed);
    TINYCA_CHECK(f.check("echo 'a | b'").allowed);
    TINYCA_CHECK(f.check("grep 'a|b' file.txt").allowed);
    TINYCA_CHECK(f.check("echo \"x && y\"").allowed);
    TINYCA_CHECK(f.check("echo 'it is 10; stop'").allowed);

    // --- must still be caught: real separators outside quotes ---
    TINYCA_CHECK(!f.check("ls; rm -rf /").allowed);
    TINYCA_CHECK(!f.check("echo \"a\" ; rm -rf /").allowed);
    TINYCA_CHECK(!f.check("echo 'quoted; text' && rm -rf /").allowed);
    TINYCA_CHECK(!f.check("ls | rm").allowed);
    TINYCA_CHECK(!f.check("echo \"a;b\" | sudo tee /etc/passwd").allowed);
    // A quote that closes before the separator must not keep protecting it.
    TINYCA_CHECK(!f.check("echo \"done\"; chmod 777 /etc").allowed);
    // Nested/alternating quotes still end: the last segment is a real one.
    TINYCA_CHECK(!f.check("echo \"a'b\" ; rm x").allowed);

    // --- unbalanced quotes must fail closed, not open ---
    // The quote never closes, so everything after it is one segment whose head
    // token is `echo` -- allowed, but nothing forbidden can hide there either,
    // because bash would treat it the same way.
    TINYCA_CHECK(f.check("echo \"unterminated ; rm -rf /").allowed);
    // ...whereas an unbalanced quote that STARTS after a forbidden head token
    // is still rejected on that token.
    TINYCA_CHECK(!f.check("rm \"unterminated ; ls").allowed);

    // --- escaped quotes inside a quoted string do not end it ---
    TINYCA_CHECK(f.check("echo \"a \\\" b ; c\"").allowed);
}

// SPEC.md §8.1: tools.confirm_commands flags an ALLOWED command for
// confirmation. It must fire wherever the program appears, not only as the
// first word, and must not change the allow/deny verdict itself.
void test_confirm_commands_flag_allowed_commands()
{
    tinyca::CommandFilter f(tinyca::FilterMode::Whitelist, default_whitelist(), {}, false, {});
    std::vector<std::string> confirm;
    confirm.push_back("python3");
    confirm.push_back("sed");
    f.set_confirm_required(confirm);

    tinyca::FilterResult direct = f.check("python3 script.py");
    TINYCA_CHECK(direct.allowed);
    TINYCA_CHECK(direct.needs_confirmation);

    // Later segments count too -- "ls && python3 x" must still prompt.
    tinyca::FilterResult second = f.check("ls && python3 x.py");
    TINYCA_CHECK(second.allowed);
    TINYCA_CHECK(second.needs_confirmation);

    // An unlisted program is untouched.
    tinyca::FilterResult plain = f.check("ls -la");
    TINYCA_CHECK(plain.allowed);
    TINYCA_CHECK(!plain.needs_confirmation);

    // A rejected command is still rejected; confirmation does not override it.
    tinyca::FilterResult blocked = f.check("python3 x.py; rm -rf /");
    TINYCA_CHECK(!blocked.allowed);
}

// Finding 1: the filter must compare what bash will RUN, not the spelling the
// model typed. bash word-expands the command name before looking it up, so
// "rm", 'rm', \rm, r""m and $'rm' all invoke rm -- and before this was fixed,
// every one of them walked straight past a blacklist (verified by watching
// bash delete the files). The default whitelist mode failed closed on the same
// input, so this was a blacklist-mode hole and a whitelist-mode annoyance.
void test_head_token_is_unquoted_before_comparison()
{
    tinyca::CommandFilter deny(tinyca::FilterMode::Blacklist, {}, tinyca::split("rm,chmod,sudo", ','), true, {});

    // Every spelling bash resolves to "rm" must be refused.
    TINYCA_CHECK(!deny.check("rm -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("\"rm\" -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("'rm' -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("\\rm -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("r\"\"m -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("r''m -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("$'rm' -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("\"r\"m -rf /tmp/x").allowed);
    // ...in any segment, not just the first word.
    TINYCA_CHECK(!deny.check("ls && \"chmod\" 777 /etc").allowed);
    // ...and path-qualified, which is the same defect wearing a different hat:
    // the blacklist names programs, and /bin/rm is that program.
    TINYCA_CHECK(!deny.check("/bin/rm -rf /tmp/x").allowed);
    TINYCA_CHECK(!deny.check("/usr/bin/sudo reboot").allowed);
    TINYCA_CHECK(!deny.check("\"/bin/rm\" -rf /tmp/x").allowed);

    // Something genuinely not on the list still runs.
    TINYCA_CHECK(deny.check("ls -la").allowed);
    TINYCA_CHECK(deny.check("\"ls\" -la").allowed);

    // Whitelist mode: the same unquoting turns a pointless rejection into a
    // correct allow, without letting an unlisted program through.
    tinyca::CommandFilter allow(tinyca::FilterMode::Whitelist, tinyca::split("ls,cat,echo", ','), {}, false, {});
    TINYCA_CHECK(allow.check("\"ls\" -la").allowed);
    TINYCA_CHECK(allow.check("'ls' -la").allowed);
    TINYCA_CHECK(allow.check("\\ls -la").allowed);
    TINYCA_CHECK(!allow.check("\"rm\" -rf /").allowed);
    TINYCA_CHECK(!allow.check("'curl' http://x").allowed);
    // A path-qualified program is NOT whitelisted by its basename: the
    // whitelist says which programs may run, and /opt/evil/ls is not the one
    // the operator listed. Whitelist mode stays strict and fails closed.
    TINYCA_CHECK(!allow.check("/opt/evil/ls -la").allowed);
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
    TINYCA_RUN(test_quoted_separators_do_not_split_segments);
    TINYCA_RUN(test_confirm_commands_flag_allowed_commands);
    TINYCA_RUN(test_head_token_is_unquoted_before_comparison);
    TINYCA_RUN(test_mode_from_string);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
