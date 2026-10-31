// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <string>
#include <vector>

namespace tinyca
{

// Every field has the documented default from SPEC.md §4. Precedence when
// loading: CLI flag > config file > this default.
struct Config
{
    // [llm]
    std::string llm_base_url = "http://127.0.0.1:8080";
    std::string llm_api_path = "/v1/chat/completions";
    std::string llm_model_id = "qwen2.5-coder-1.5b";
    std::string llm_api_key = "none";
    int llm_request_timeout_s = 900;
    int llm_connect_timeout_s = 10;
    int llm_max_tokens = 512;
    double llm_temperature = 0.2;
    double llm_top_p = 0.9;
    int llm_context_tokens = 4096;
    bool llm_stream = true;

    // [protocol]
    std::string protocol_tool_protocol = "fenced"; // fenced | hermes
    bool protocol_enable_grammar = false;
    std::string protocol_grammar_file = "grammars/hermes_tool_call.gbnf";
    bool protocol_strip_think = true;
    bool protocol_strip_think_from_history = true;
    std::string protocol_stop_sequences; // comma-separated; empty = protocol default

    // [agent]
    std::string agent_system_prompt_file = "config/system_prompt.txt";
    int agent_max_iterations = 8;
    int agent_repetition_threshold = 2;
    std::string agent_work_dir = "./workspace";
    std::string agent_transcript_log_path = "./logs/transcript.jsonl";
    long agent_transcript_max_bytes = 0;     // 0 = unlimited
    std::string agent_log_level = "info";    // error | warn | info | debug
    std::string agent_mode = "interactive";  // interactive | oneshot
    std::string agent_color_output = "auto"; // auto | always | never

    // [tools]
    int tools_bash_timeout_s = 60;
    int tools_output_head_lines = 40;
    int tools_output_tail_lines = 40;
    int tools_output_max_bytes = 4096;
    std::string tools_command_filter_mode = "whitelist"; // whitelist | blacklist | none
    std::string tools_whitelist = "ls,cat,grep,find,wc,head,tail,pwd,echo,mkdir,cp,mv,touch,make,"
                                  "gcc,g++,python3,git,sed,awk,diff,wget";
    std::string tools_blacklist = "rm,dd,mkfs,shutdown,reboot,sudo,su,chmod,chown,kill,pkill,"
                                  "killall,mount,umount";
    std::string tools_on_blocked_command = "ask"; // ask | deny
    std::string tools_confirm_mode = "never";     // always | never
    bool tools_allow_metacharacters = false;

    // [skills]
    std::string skills_skills_dir = "./skills";
    bool skills_tier1_enabled = true;
    int skills_max_tier1_skills = 8;

    // CLI-only, not settable via the INI file
    std::string cli_task_prompt;                         // -p/--prompt; non-empty implies agent_mode = "oneshot"
    bool cli_dry_run = false;                            // --dry-run
    bool cli_help_requested = false;                     // --help
    bool cli_version_requested = false;                  // --version
    std::string cli_config_path = "./config/tinyca.ini"; // -c/--config
};

struct ConfigLoadResult
{
    bool ok = true;
    std::string fatal_error;           // set, and ok = false, on anything unrecoverable
    std::vector<std::string> warnings; // missing file, unknown key, bad value, etc.
};

// Parses CLI args and the config file they (or the default path) point at,
// then applies CLI > file > default onto `out`. Does not touch argv beyond
// reading it. A missing config file is a warning, not a failure.
ConfigLoadResult load_config(int argc, char **argv, Config &out);

} // namespace tinyca
