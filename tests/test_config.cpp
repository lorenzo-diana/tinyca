// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/config.h"

#include <vector>

#include "test_util.h"

namespace
{

// Builds an argv-style array from a list of strings. The returned vector of
// char* points into `storage`, which the caller must keep alive for as long
// as the argv array is used.
std::vector<char*> make_argv(std::vector<std::string>& storage)
{
    std::vector<char*> argv;
    for (auto& s : storage)
    {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    return argv;
}

bool has_warning_containing(const std::vector<std::string>& warnings, const std::string& needle)
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

void test_missing_file_uses_defaults()
{
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/does_not_exist.ini"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK(has_warning_containing(result.warnings, "config file not found"));
    // Defaults untouched.
    TINYCA_CHECK_EQ(cfg.llm_base_url, std::string("http://127.0.0.1:8080"));
    TINYCA_CHECK_EQ(cfg.llm_max_tokens, 512);
    TINYCA_CHECK_EQ(cfg.protocol_tool_protocol, std::string("fenced"));
}

void test_comments_sections_whitespace_trimmed()
{
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/basic.ini"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK(!has_warning_containing(result.warnings, "config file not found"));
    TINYCA_CHECK_EQ(cfg.llm_base_url, std::string("http://10.0.0.5:9090"));
    TINYCA_CHECK_EQ(cfg.llm_model_id, std::string("qwen-test-model"));
    TINYCA_CHECK_EQ(cfg.llm_max_tokens, 256);
    TINYCA_CHECK_EQ(cfg.protocol_tool_protocol, std::string("hermes"));
    TINYCA_CHECK_EQ(cfg.agent_mode, std::string("oneshot"));
    // A key never mentioned in the file keeps its default.
    TINYCA_CHECK_EQ(cfg.llm_temperature, 0.2);
}

void test_missing_key_keeps_default()
{
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/basic.ini"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);
    // tools.* is entirely absent from basic.ini.
    TINYCA_CHECK_EQ(cfg.tools_bash_timeout_s, 60);
    TINYCA_CHECK_EQ(cfg.tools_command_filter_mode, std::string("whitelist"));
}

void test_unknown_key_warns_but_does_not_fail()
{
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/unknown_key.ini"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK(has_warning_containing(result.warnings, "unknown config key: llm.this_key_does_not_exist"));
    TINYCA_CHECK(
        has_warning_containing(result.warnings, "unknown config key: totally_bogus_section.also_unknown"));
    // The valid key in the same file still applies.
    TINYCA_CHECK_EQ(cfg.llm_base_url, std::string("http://127.0.0.1:1234"));
}

void test_malformed_lines_warn_and_skip()
{
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/malformed.ini"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK(has_warning_containing(result.warnings, "malformed line"));
    TINYCA_CHECK(has_warning_containing(result.warnings, "empty key"));
    TINYCA_CHECK_EQ(cfg.llm_base_url, std::string("http://only.this.one.survives"));
}

void test_cli_overrides_file_overrides_default()
{
    // File sets model_id = qwen-test-model; CLI overrides it again.
    std::vector<std::string> argv_storage = {"tinyca", "-c", "tests/fixtures/ini/basic.ini", "-m",
                                               "cli-wins-model", "-u", "http://cli-wins:1"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK_EQ(cfg.llm_model_id, std::string("cli-wins-model"));
    TINYCA_CHECK_EQ(cfg.llm_base_url, std::string("http://cli-wins:1"));
    // Untouched-by-CLI value from the file still applies.
    TINYCA_CHECK_EQ(cfg.llm_max_tokens, 256);
}

void test_cli_flags_no_confirm_dry_run_verbose_prompt()
{
    std::vector<std::string> argv_storage = {"tinyca", "--no-confirm", "--dry-run", "-v", "-p",
                                               "do the thing", "--protocol", "hermes"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    cfg.tools_confirm_mode = "always"; // simulate a file that set this; CLI must still win
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK_EQ(cfg.tools_confirm_mode, std::string("never"));
    TINYCA_CHECK(cfg.cli_dry_run);
    TINYCA_CHECK_EQ(cfg.agent_log_level, std::string("debug"));
    TINYCA_CHECK_EQ(cfg.cli_task_prompt, std::string("do the thing"));
    TINYCA_CHECK_EQ(cfg.agent_mode, std::string("oneshot"));
    TINYCA_CHECK_EQ(cfg.protocol_tool_protocol, std::string("hermes"));
}

void test_grammar_fenced_is_ignored_with_warning()
{
    std::vector<std::string> argv_storage = {"tinyca"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    cfg.protocol_enable_grammar = true;
    cfg.protocol_tool_protocol = "fenced";
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK(!cfg.protocol_enable_grammar);
    TINYCA_CHECK(has_warning_containing(result.warnings, "grammar is ignored"));
}

void test_grammar_with_strip_think_false_is_fatal()
{
    std::vector<std::string> argv_storage = {"tinyca"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    cfg.protocol_enable_grammar = true;
    cfg.protocol_tool_protocol = "hermes";
    cfg.protocol_strip_think = false;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(!result.ok);
    TINYCA_CHECK(!result.fatal_error.empty());
}

void test_grammar_hermes_overrides_api_path()
{
    std::vector<std::string> argv_storage = {"tinyca"};
    auto argv = make_argv(argv_storage);
    tinyca::Config cfg;
    cfg.protocol_enable_grammar = true;
    cfg.protocol_tool_protocol = "hermes";
    cfg.protocol_strip_think = true;
    auto result = tinyca::load_config(static_cast<int>(argv.size()), argv.data(), cfg);

    TINYCA_CHECK(result.ok);
    TINYCA_CHECK_EQ(cfg.llm_api_path, std::string("/completion"));
    TINYCA_CHECK(has_warning_containing(result.warnings, "overriding llm.api_path"));
}

void run_all_tests()
{
    TINYCA_RUN(test_missing_file_uses_defaults);
    TINYCA_RUN(test_comments_sections_whitespace_trimmed);
    TINYCA_RUN(test_missing_key_keeps_default);
    TINYCA_RUN(test_unknown_key_warns_but_does_not_fail);
    TINYCA_RUN(test_malformed_lines_warn_and_skip);
    TINYCA_RUN(test_cli_overrides_file_overrides_default);
    TINYCA_RUN(test_cli_flags_no_confirm_dry_run_verbose_prompt);
    TINYCA_RUN(test_grammar_fenced_is_ignored_with_warning);
    TINYCA_RUN(test_grammar_with_strip_think_false_is_fatal);
    TINYCA_RUN(test_grammar_hermes_overrides_api_path);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
