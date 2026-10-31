// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/config.h"

#include <getopt.h>

#include <functional>
#include <map>
#include <stdexcept>

#include "tinyca/ini_parser.h"
#include "tinyca/string_util.h"

namespace tinyca
{

namespace
{

bool parse_bool(const std::string &raw, bool &out)
{
    std::string v = to_lower(trim(raw));
    if (v == "true" || v == "1")
    {
        out = true;
        return true;
    }
    if (v == "false" || v == "0")
    {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string &raw, int &out)
{
    try
    {
        size_t pos = 0;
        int v = std::stoi(trim(raw), &pos);
        if (pos != trim(raw).size())
            return false;
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parse_long(const std::string &raw, long &out)
{
    try
    {
        size_t pos = 0;
        long v = std::stol(trim(raw), &pos);
        if (pos != trim(raw).size())
            return false;
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parse_double(const std::string &raw, double &out)
{
    try
    {
        size_t pos = 0;
        double v = std::stod(trim(raw), &pos);
        if (pos != trim(raw).size())
            return false;
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool is_one_of(const std::string &v, const std::vector<std::string> &choices)
{
    for (const auto &c : choices)
        if (v == c)
            return true;
    return false;
}

using Setter = std::function<bool(Config &, const std::string &, std::string &)>;

bool set_string_field(std::string &field, const std::string &value, std::string &)
{
    field = value;
    return true;
}

bool set_enum_field(std::string &field, const std::string &value, const std::vector<std::string> &choices,
                    std::string &err)
{
    if (!is_one_of(value, choices))
    {
        err = "invalid value '" + value + "'";
        return false;
    }
    field = value;
    return true;
}

bool set_bool_field(bool &field, const std::string &value, std::string &err)
{
    bool v;
    if (!parse_bool(value, v))
    {
        err = "invalid boolean '" + value + "'";
        return false;
    }
    field = v;
    return true;
}

bool set_int_field(int &field, const std::string &value, std::string &err)
{
    int v;
    if (!parse_int(value, v))
    {
        err = "invalid integer '" + value + "'";
        return false;
    }
    field = v;
    return true;
}

bool set_long_field(long &field, const std::string &value, std::string &err)
{
    long v;
    if (!parse_long(value, v))
    {
        err = "invalid integer '" + value + "'";
        return false;
    }
    field = v;
    return true;
}

bool set_double_field(double &field, const std::string &value, std::string &err)
{
    double v;
    if (!parse_double(value, v))
    {
        err = "invalid number '" + value + "'";
        return false;
    }
    field = v;
    return true;
}

std::map<std::string, Setter> build_key_table()
{
    std::map<std::string, Setter> t;

    t["llm.base_url"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.llm_base_url, v, e);
    };
    t["llm.api_path"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.llm_api_path, v, e);
    };
    t["llm.model_id"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.llm_model_id, v, e);
    };
    t["llm.api_key"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.llm_api_key, v, e);
    };
    t["llm.request_timeout_s"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.llm_request_timeout_s, v, e);
    };
    t["llm.connect_timeout_s"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.llm_connect_timeout_s, v, e);
    };
    t["llm.max_tokens"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.llm_max_tokens, v, e);
    };
    t["llm.temperature"] = [](Config &c, const std::string &v, std::string &e) {
        return set_double_field(c.llm_temperature, v, e);
    };
    t["llm.top_p"] = [](Config &c, const std::string &v, std::string &e) {
        return set_double_field(c.llm_top_p, v, e);
    };
    t["llm.context_tokens"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.llm_context_tokens, v, e);
    };
    t["llm.stream"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.llm_stream, v, e);
    };

    t["protocol.tool_protocol"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.protocol_tool_protocol, v, {"fenced", "hermes"}, e);
    };
    t["protocol.enable_grammar"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.protocol_enable_grammar, v, e);
    };
    t["protocol.grammar_file"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.protocol_grammar_file, v, e);
    };
    t["protocol.strip_think"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.protocol_strip_think, v, e);
    };
    t["protocol.strip_think_from_history"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.protocol_strip_think_from_history, v, e);
    };
    t["protocol.stop_sequences"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.protocol_stop_sequences, v, e);
    };

    t["agent.system_prompt_file"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.agent_system_prompt_file, v, e);
    };
    t["agent.max_iterations"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.agent_max_iterations, v, e);
    };
    t["agent.repetition_threshold"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.agent_repetition_threshold, v, e);
    };
    t["agent.work_dir"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.agent_work_dir, v, e);
    };
    t["agent.transcript_log_path"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.agent_transcript_log_path, v, e);
    };
    t["agent.transcript_max_bytes"] = [](Config &c, const std::string &v, std::string &e) {
        return set_long_field(c.agent_transcript_max_bytes, v, e);
    };
    t["agent.log_level"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.agent_log_level, v, {"error", "warn", "info", "debug"}, e);
    };
    t["agent.mode"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.agent_mode, v, {"interactive", "oneshot"}, e);
    };
    t["agent.color_output"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.agent_color_output, v, {"auto", "always", "never"}, e);
    };

    t["tools.bash_timeout_s"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.tools_bash_timeout_s, v, e);
    };
    t["tools.output_head_lines"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.tools_output_head_lines, v, e);
    };
    t["tools.output_tail_lines"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.tools_output_tail_lines, v, e);
    };
    t["tools.output_max_bytes"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.tools_output_max_bytes, v, e);
    };
    t["tools.command_filter_mode"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.tools_command_filter_mode, v, {"whitelist", "blacklist", "none"}, e);
    };
    t["tools.whitelist"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.tools_whitelist, v, e);
    };
    t["tools.blacklist"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.tools_blacklist, v, e);
    };
    t["tools.on_blocked_command"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.tools_on_blocked_command, v, {"ask", "deny"}, e);
    };
    t["tools.confirm_mode"] = [](Config &c, const std::string &v, std::string &e) {
        return set_enum_field(c.tools_confirm_mode, v, {"always", "never"}, e);
    };
    t["tools.allow_metacharacters"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.tools_allow_metacharacters, v, e);
    };

    t["skills.skills_dir"] = [](Config &c, const std::string &v, std::string &e) {
        return set_string_field(c.skills_skills_dir, v, e);
    };
    t["skills.tier1_enabled"] = [](Config &c, const std::string &v, std::string &e) {
        return set_bool_field(c.skills_tier1_enabled, v, e);
    };
    t["skills.max_tier1_skills"] = [](Config &c, const std::string &v, std::string &e) {
        return set_int_field(c.skills_max_tier1_skills, v, e);
    };

    return t;
}

struct CliOverrides
{
    bool has_config_path = false;
    std::string config_path;
    bool has_prompt = false;
    std::string prompt;
    bool has_model = false;
    std::string model;
    bool has_url = false;
    std::string url;
    bool has_protocol = false;
    std::string protocol;
    bool has_no_confirm = false;
    bool has_dry_run = false;
    bool has_verbose = false;
    bool has_help = false;
    bool has_version = false;
};

enum
{
    OPT_PROTOCOL = 1001,
    OPT_NO_CONFIRM = 1002,
    OPT_DRY_RUN = 1003,
    OPT_VERSION = 1004,
    OPT_HELP = 1005
};

CliOverrides parse_cli(int argc, char **argv, std::vector<std::string> &warnings)
{
    CliOverrides o;
    static const struct option long_opts[] = {{"config", required_argument, nullptr, 'c'},
                                              {"prompt", required_argument, nullptr, 'p'},
                                              {"model", required_argument, nullptr, 'm'},
                                              {"url", required_argument, nullptr, 'u'},
                                              {"protocol", required_argument, nullptr, OPT_PROTOCOL},
                                              {"no-confirm", no_argument, nullptr, OPT_NO_CONFIRM},
                                              {"dry-run", no_argument, nullptr, OPT_DRY_RUN},
                                              {"verbose", no_argument, nullptr, 'v'},
                                              {"version", no_argument, nullptr, OPT_VERSION},
                                              {"help", no_argument, nullptr, OPT_HELP},
                                              {nullptr, 0, nullptr, 0}};

    optind = 1; // reset getopt's global state -- load_config may be called repeatedly in tests
    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, "c:p:m:u:v", long_opts, nullptr)) != -1)
    {
        switch (c)
        {
        case 'c':
            o.has_config_path = true;
            o.config_path = optarg;
            break;
        case 'p':
            o.has_prompt = true;
            o.prompt = optarg;
            break;
        case 'm':
            o.has_model = true;
            o.model = optarg;
            break;
        case 'u':
            o.has_url = true;
            o.url = optarg;
            break;
        case OPT_PROTOCOL:
            o.has_protocol = true;
            o.protocol = optarg;
            break;
        case OPT_NO_CONFIRM:
            o.has_no_confirm = true;
            break;
        case OPT_DRY_RUN:
            o.has_dry_run = true;
            break;
        case 'v':
            o.has_verbose = true;
            break;
        case OPT_VERSION:
            o.has_version = true;
            break;
        case OPT_HELP:
            o.has_help = true;
            break;
        case '?':
        default:
            warnings.push_back(std::string("unrecognized CLI option near index ") + std::to_string(optind));
            break;
        }
    }
    return o;
}

} // namespace

ConfigLoadResult load_config(int argc, char **argv, Config &out)
{
    ConfigLoadResult result;

    std::vector<std::string> cli_warnings;
    CliOverrides cli = parse_cli(argc, argv, cli_warnings);
    for (const auto &w : cli_warnings)
        result.warnings.push_back(w);

    if (cli.has_config_path)
        out.cli_config_path = cli.config_path;

    std::map<std::string, std::string> kv;
    bool file_found = false;
    std::vector<std::string> ini_warnings;
    IniParser::parse_file(out.cli_config_path, kv, ini_warnings, file_found);
    for (const auto &w : ini_warnings)
        result.warnings.push_back(w);
    if (!file_found)
        result.warnings.push_back("config file not found: " + out.cli_config_path + " -- using built-in defaults");

    static const std::map<std::string, Setter> key_table = build_key_table();
    for (const auto &entry : kv)
    {
        auto it = key_table.find(entry.first);
        if (it == key_table.end())
        {
            result.warnings.push_back("unknown config key: " + entry.first);
            continue;
        }
        std::string err;
        if (!it->second(out, entry.second, err))
            result.warnings.push_back("config key '" + entry.first + "': " + err + " -- keeping default");
    }

    // CLI overrides last -- CLI > file > default.
    if (cli.has_model)
        out.llm_model_id = cli.model;
    if (cli.has_url)
        out.llm_base_url = cli.url;
    if (cli.has_protocol)
    {
        if (is_one_of(cli.protocol, {"fenced", "hermes"}))
            out.protocol_tool_protocol = cli.protocol;
        else
            result.warnings.push_back("--protocol: invalid value '" + cli.protocol + "'");
    }
    if (cli.has_no_confirm)
        out.tools_confirm_mode = "never";
    if (cli.has_dry_run)
        out.cli_dry_run = true;
    if (cli.has_verbose)
        out.agent_log_level = "debug";
    if (cli.has_prompt)
    {
        out.cli_task_prompt = cli.prompt;
        out.agent_mode = "oneshot";
    }
    out.cli_help_requested = cli.has_help;
    out.cli_version_requested = cli.has_version;

    // SPEC.md §6.3: fenced + grammar -> warn and ignore (grammar has no effect).
    if (out.protocol_enable_grammar && out.protocol_tool_protocol == "fenced")
    {
        result.warnings.push_back(
            "protocol.enable_grammar=true with tool_protocol=fenced: grammar is ignored (SPEC.md §6.3)");
        out.protocol_enable_grammar = false;
    }

    // SPEC.md §6.3: grammar constrains output to a tool call, which forbids a
    // preceding <think> block -- fatal, not a warning.
    if (out.protocol_enable_grammar && !out.protocol_strip_think)
    {
        result.ok = false;
        result.fatal_error = "protocol.enable_grammar=true requires protocol.strip_think=true (SPEC.md §6.3)";
        return result;
    }

    // SPEC.md §6.3 / §4.1: grammar is only honoured on the native /completion
    // endpoint -- auto-override rather than ask the user to get this right.
    if (out.protocol_enable_grammar)
    {
        result.warnings.push_back("protocol.enable_grammar=true: overriding llm.api_path to "
                                  "/completion (SPEC.md §6.3)");
        out.llm_api_path = "/completion";
    }

    return result;
}

} // namespace tinyca
