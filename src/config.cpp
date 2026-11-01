// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/config.h"

#include <getopt.h>

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

// std::stoi/stol/stod differ only in name, so the "whole string consumed,
// otherwise reject" logic is shared through one trait rather than written
// out three times.
template <typename T> struct NumParse;
template <> struct NumParse<int>
{
    static int call(const std::string &s, size_t *pos)
    {
        return std::stoi(s, pos);
    }
};
template <> struct NumParse<long>
{
    static long call(const std::string &s, size_t *pos)
    {
        return std::stol(s, pos);
    }
};
template <> struct NumParse<double>
{
    static double call(const std::string &s, size_t *pos)
    {
        return std::stod(s, pos);
    }
};

// A trailing-garbage value ("512abc") is rejected rather than silently
// accepted as 512 -- a typo in the INI file should surface as a warning.
template <typename T> bool parse_number(const std::string &raw, T &out)
{
    const std::string s = trim(raw);
    try
    {
        size_t pos = 0;
        T v = NumParse<T>::call(s, &pos);
        if (pos != s.size())
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

// SPEC.md §4's key list, as data: one row per `section.key`, grouped by the
// field type it writes. These tables are the only place a config key name
// appears -- adding a key means adding one row, not touching any parsing
// logic. Keys are addressed as "section.key" exactly as IniParser emits them.
template <typename T> struct KeyRow
{
    const char *key;
    T Config::*field;
};

struct EnumRow
{
    const char *key;
    std::string Config::*field;
    const char *choices; // comma-separated, validated against the raw value
};

const KeyRow<std::string> kStringKeys[] = {
    {"llm.base_url", &Config::llm_base_url},
    {"llm.api_path", &Config::llm_api_path},
    {"llm.model_id", &Config::llm_model_id},
    {"llm.api_key", &Config::llm_api_key},
    {"protocol.grammar_file", &Config::protocol_grammar_file},
    {"protocol.stop_sequences", &Config::protocol_stop_sequences},
    {"agent.system_prompt_file", &Config::agent_system_prompt_file},
    {"agent.work_dir", &Config::agent_work_dir},
    {"agent.transcript_log_path", &Config::agent_transcript_log_path},
    {"agent.sessions_dir", &Config::agent_sessions_dir},
    {"tools.whitelist", &Config::tools_whitelist},
    {"tools.blacklist", &Config::tools_blacklist},
    {"tools.confirm_commands", &Config::tools_confirm_commands},
    {"skills.skills_dir", &Config::skills_skills_dir},
    {"skills.skill_checks", &Config::skills_skill_checks},
    {"skills.add_skill_name", &Config::skills_add_skill_name},
    {"skills.tier1_exclude", &Config::skills_tier1_exclude},
};

const EnumRow kEnumKeys[] = {
    {"protocol.tool_protocol", &Config::protocol_tool_protocol, "fenced,hermes"},
    {"agent.log_level", &Config::agent_log_level, "error,warn,info,debug"},
    {"agent.mode", &Config::agent_mode, "interactive,oneshot"},
    {"agent.color_output", &Config::agent_color_output, "auto,always,never"},
    {"tools.command_filter_mode", &Config::tools_command_filter_mode, "whitelist,blacklist,none"},
    {"tools.on_blocked_command", &Config::tools_on_blocked_command, "ask,deny"},
    {"tools.confirm_mode", &Config::tools_confirm_mode, "always,never"},
};

const KeyRow<bool> kBoolKeys[] = {
    {"llm.stream", &Config::llm_stream},
    {"protocol.enable_grammar", &Config::protocol_enable_grammar},
    {"protocol.strip_think", &Config::protocol_strip_think},
    {"protocol.strip_think_from_history", &Config::protocol_strip_think_from_history},
    {"tools.allow_metacharacters", &Config::tools_allow_metacharacters},
    {"skills.tier1_enabled", &Config::skills_tier1_enabled},
    {"skills.tier2_enabled", &Config::skills_tier2_enabled},
};

const KeyRow<int> kIntKeys[] = {
    {"llm.request_timeout_s", &Config::llm_request_timeout_s},
    {"llm.connect_timeout_s", &Config::llm_connect_timeout_s},
    {"llm.max_tokens", &Config::llm_max_tokens},
    {"llm.context_tokens", &Config::llm_context_tokens},
    {"agent.max_iterations", &Config::agent_max_iterations},
    {"agent.repetition_threshold", &Config::agent_repetition_threshold},
    {"tools.bash_timeout_s", &Config::tools_bash_timeout_s},
    {"tools.output_head_lines", &Config::tools_output_head_lines},
    {"tools.output_tail_lines", &Config::tools_output_tail_lines},
    {"tools.output_max_bytes", &Config::tools_output_max_bytes},
    {"skills.max_tier1_skills", &Config::skills_max_tier1_skills},
};

const KeyRow<long> kLongKeys[] = {
    {"agent.transcript_max_bytes", &Config::agent_transcript_max_bytes},
    {"agent.transcript_max_bytes_per_run", &Config::agent_transcript_max_bytes_per_run},
};

const KeyRow<double> kDoubleKeys[] = {
    {"llm.temperature", &Config::llm_temperature},
    {"llm.top_p", &Config::llm_top_p},
};

template <typename T, size_t N> const KeyRow<T> *find_row(const KeyRow<T> (&rows)[N], const std::string &key)
{
    for (size_t i = 0; i < N; ++i)
        if (key == rows[i].key)
            return &rows[i];
    return nullptr;
}

// Applies one `section.key = value` pair. Returns false on a *known* key with
// an unusable value (`err` set, caller keeps the default); sets `known` to
// false for a key in no table, which the caller reports separately as an
// unknown-key warning. A missing or bad key is never fatal (dev/PROMPT.md "Style").
bool apply_key(Config &out, const std::string &key, const std::string &value, std::string &err, bool &known)
{
    known = true;

    if (const KeyRow<std::string> *row = find_row(kStringKeys, key))
    {
        out.*(row->field) = value;
        return true;
    }
    for (const auto &row : kEnumKeys)
    {
        if (key != row.key)
            continue;
        if (!is_one_of(value, split(row.choices, ',')))
        {
            err = "invalid value '" + value + "'";
            return false;
        }
        out.*(row.field) = value;
        return true;
    }
    if (const KeyRow<bool> *row = find_row(kBoolKeys, key))
    {
        if (!parse_bool(value, out.*(row->field)))
        {
            err = "invalid boolean '" + value + "'";
            return false;
        }
        return true;
    }
    if (const KeyRow<int> *row = find_row(kIntKeys, key))
    {
        if (!parse_number(value, out.*(row->field)))
        {
            err = "invalid integer '" + value + "'";
            return false;
        }
        return true;
    }
    if (const KeyRow<long> *row = find_row(kLongKeys, key))
    {
        if (!parse_number(value, out.*(row->field)))
        {
            err = "invalid integer '" + value + "'";
            return false;
        }
        return true;
    }
    if (const KeyRow<double> *row = find_row(kDoubleKeys, key))
    {
        if (!parse_number(value, out.*(row->field)))
        {
            err = "invalid number '" + value + "'";
            return false;
        }
        return true;
    }

    known = false;
    return false;
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
    bool has_resume = false;
    std::string resume_path;
    bool has_resume_prompt = false;
    std::string resume_prompt;
    bool has_resume_work_dir = false;
    std::string resume_work_dir;
    bool has_session = false;
    std::string session;
    bool has_list_sessions = false;
    bool has_rename_to = false;
    std::string rename_to;
    bool has_positional_task = false;
    std::string positional_task;
};

enum
{
    OPT_PROTOCOL = 1001,
    OPT_NO_CONFIRM = 1002,
    OPT_DRY_RUN = 1003,
    OPT_VERSION = 1004,
    OPT_HELP = 1005,
    OPT_RESUME = 1006,
    OPT_RESUME_PROMPT = 1007,
    OPT_RESUME_WORK_DIR = 1008,
    OPT_SESSION = 1009,
    OPT_LIST_SESSIONS = 1010,
    OPT_RENAME_TO = 1011
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
                                              {"resume", required_argument, nullptr, OPT_RESUME},
                                              {"resume-prompt", required_argument, nullptr, OPT_RESUME_PROMPT},
                                              {"resume-work-dir", required_argument, nullptr, OPT_RESUME_WORK_DIR},
                                              {"session", required_argument, nullptr, OPT_SESSION},
                                              {"list-sessions", no_argument, nullptr, OPT_LIST_SESSIONS},
                                              {"rename-to", required_argument, nullptr, OPT_RENAME_TO},
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
        case OPT_RESUME:
            o.has_resume = true;
            o.resume_path = optarg;
            break;
        case OPT_RESUME_PROMPT:
            o.has_resume_prompt = true;
            o.resume_prompt = optarg;
            break;
        case OPT_RESUME_WORK_DIR:
            o.has_resume_work_dir = true;
            o.resume_work_dir = optarg;
            break;
        case OPT_SESSION:
            o.has_session = true;
            o.session = optarg;
            break;
        case OPT_LIST_SESSIONS:
            o.has_list_sessions = true;
            break;
        case OPT_RENAME_TO:
            o.has_rename_to = true;
            o.rename_to = optarg;
            break;
        case '?':
        default:
            warnings.push_back(std::string("unrecognized CLI option near index ") + std::to_string(optind));
            break;
        }
    }

    // SPEC.md §12's positional [task]: everything left after the options is
    // joined with single spaces into one task string. -p/--prompt wins (and
    // implies oneshot); a bare positional task leaves `mode` as configured,
    // i.e. "run this, then stay interactive".
    if (!o.has_prompt && optind < argc)
    {
        std::string task;
        for (int i = optind; i < argc; ++i)
        {
            if (!task.empty())
                task += " ";
            task += argv[i];
        }
        if (!task.empty())
        {
            o.has_positional_task = true;
            o.positional_task = task;
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

    for (const auto &entry : kv)
    {
        std::string err;
        bool known = true;
        if (apply_key(out, entry.first, entry.second, err, known))
            continue;
        if (known)
            result.warnings.push_back("config key '" + entry.first + "': " + err + " -- keeping default");
        else
            result.warnings.push_back("unknown config key: " + entry.first);
    }

    // Whether the transcript path was chosen deliberately. A session name
    // (or the generated default) only decides the path when it was not
    // (SPEC.md §12.3), so existing configs and tests keep their own file.
    out.cli_transcript_path_explicit = kv.find("agent.transcript_log_path") != kv.end();

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
    else if (cli.has_positional_task)
    {
        out.cli_task_prompt = cli.positional_task; // mode untouched: §12's "[task]"
    }
    out.cli_help_requested = cli.has_help;
    out.cli_version_requested = cli.has_version;
    if (cli.has_session)
        out.cli_session_name = cli.session;
    out.cli_list_sessions = cli.has_list_sessions;
    if (cli.has_rename_to)
        out.cli_rename_to = cli.rename_to;
    if (cli.has_resume)
    {
        out.cli_resume_path = cli.resume_path;
        // A resumed session continues IN ITS OWN log: the transcript being
        // replayed becomes the transcript being written, so one file remains
        // the complete record of that session (SPEC.md §12.2). CLI beats the
        // config file here, same as every other override.
        out.agent_transcript_log_path = cli.resume_path;
    }
    if (cli.has_resume_work_dir)
    {
        if (is_one_of(cli.resume_work_dir, {"ask", "continue", "quit"}))
            out.cli_resume_work_dir_policy = cli.resume_work_dir;
        else
            result.warnings.push_back("--resume-work-dir: invalid value '" + cli.resume_work_dir +
                                      "' (ask|continue|quit); falling back to the default");
    }
    if (cli.has_resume_prompt)
    {
        if (is_one_of(cli.resume_prompt, {"ask", "update", "quit"}))
            out.cli_resume_prompt_policy = cli.resume_prompt;
        else
            result.warnings.push_back("--resume-prompt: invalid value '" + cli.resume_prompt +
                                      "' (ask|update|quit); falling back to the default");
    }

    // skills.skill_checks is a set, not an enum, so it is validated here
    // rather than by the enum table: an unknown token is dropped with a
    // warning instead of silently disabling a safeguard the operator asked
    // for (SPEC.md §7.1).
    {
        std::vector<std::string> kept;
        for (const auto &token : split_trimmed(out.skills_skill_checks, ','))
            if (token == "none" || token == "filter_on_create" || token == "confirm_on_create" ||
                token == "filter_at_startup")
                kept.push_back(token);
            else
                result.warnings.push_back("skills.skill_checks: unknown value '" + token + "' ignored");
        std::string normalized;
        for (const auto &token : kept)
            normalized += (normalized.empty() ? "" : ",") + token;
        out.skills_skill_checks = normalized;
    }

    // A cap has to fit at least one entry plus the cutoff marker (~131 bytes)
    // or the transcript comes out empty: the first entry already overflows and
    // the marker that would explain it does not fit either. Warn loudly rather
    // than let a plausible-looking small number silently disable diagnostics.
    const long kMinUsefulTranscriptBytes = 256;
    if (out.agent_transcript_max_bytes > 0 && out.agent_transcript_max_bytes < kMinUsefulTranscriptBytes)
        result.warnings.push_back("agent.transcript_max_bytes=" + std::to_string(out.agent_transcript_max_bytes) +
                                  " is below the " + std::to_string(kMinUsefulTranscriptBytes) +
                                  " bytes needed for one entry plus the cutoff marker: the transcript may stay empty");
    if (out.agent_transcript_max_bytes_per_run > 0 &&
        out.agent_transcript_max_bytes_per_run < kMinUsefulTranscriptBytes)
        result.warnings.push_back(
            "agent.transcript_max_bytes_per_run=" + std::to_string(out.agent_transcript_max_bytes_per_run) +
            " is below the " + std::to_string(kMinUsefulTranscriptBytes) +
            " bytes needed for one entry plus the cutoff marker: this run may log nothing");

    // SPEC.md §9.2: the per-run cap lives inside the total-file cap, so a
    // per-run value larger than the total is incoherent -- clamp it down and
    // say so rather than letting one cap silently shadow the other. Only
    // meaningful when both are enabled (0 = unlimited on either).
    if (out.agent_transcript_max_bytes > 0 && out.agent_transcript_max_bytes_per_run > out.agent_transcript_max_bytes)
    {
        result.warnings.push_back("agent.transcript_max_bytes_per_run (" +
                                  std::to_string(out.agent_transcript_max_bytes_per_run) +
                                  ") exceeds agent.transcript_max_bytes (" +
                                  std::to_string(out.agent_transcript_max_bytes) + "): clamping it to the total cap");
        out.agent_transcript_max_bytes_per_run = out.agent_transcript_max_bytes;
    }

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
