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
    std::string agent_work_dir = "./workspace"; // --work-dir overrides this

    // What to do when a RESUMED session disagrees with this run (SPEC.md
    // §12.2): the system prompt it recorded is not the one just generated (a
    // skill was added, removed or renamed), or the work_dir it recorded is not
    // the one this run would use. Both default to "ask", and both are only
    // consulted when there is an actual difference.
    //
    // "ask" is safe unattended as well: with no one to answer, the question
    // reads EOF, which counts as "no" and aborts without touching the
    // transcript. --on-prompt-change / --on-work-dir-change override these.
    std::string agent_on_prompt_change = "ask";   // ask | update | quit
    std::string agent_on_work_dir_change = "ask"; // ask | continue | quit
    std::string agent_transcript_log_path = "./logs/transcript.jsonl";
    // Where named sessions live (SPEC.md §12.3). A session is one file in
    // here, so the directory is the registry.
    std::string agent_sessions_dir = "./logs/sessions";
    long agent_transcript_max_bytes = 0;         // total file size; 0 = unlimited
    long agent_transcript_max_bytes_per_run = 0; // bytes appended by one run; 0 = unlimited
    std::string agent_log_level = "info";        // error | warn | info | debug
    std::string agent_mode = "interactive";      // interactive | oneshot
    std::string agent_color_output = "auto";     // auto | always | never
    // Show the model's reasoning as it streams. Off by default: it is long
    // (measured 206 of 210 chunks on a thinking model) and it is not the
    // answer. /toggle_show_thinking flips this at runtime; the key exists so a
    // oneshot run can start with it on, which the toggle alone cannot do.
    bool agent_show_thinking = false;

    // [appearance] -- terminal styling, "fg[,bg[,attr]]" per element
    // (SPEC.md §15). The defaults reproduce the colours TinyCA always used,
    // plus a background band for the two things that are the agent acting
    // rather than the model talking: the command it runs with its output, and
    // a question it is asking.
    std::string appearance_user_input = "cyan,,bold";
    std::string appearance_think = "white,,dim";
    std::string appearance_assistant = "green";
    std::string appearance_action = "yellow,bright_black,bold";
    std::string appearance_tool_output = ",bright_black";
    std::string appearance_confirm = "white,blue,bold";
    // TinyCA's own voice, which must never be mistaken for the model's: a
    // warning, an error, and the ordinary notices ("new session: ...").
    std::string appearance_warning = "yellow,,bold";
    std::string appearance_error = "red,,bold";
    std::string appearance_notice = "bright_black";
    // The bottom status bar: the bar itself, and the two words that report a
    // state rather than describe one (YES/ON against NO/OFF), which are read
    // by colour before they are read as text.
    std::string appearance_status = "0,bright_blue,bold";
    // Off for terminals that mishandle a scroll region; the bar is also
    // skipped whenever stdout is not a terminal.
    bool appearance_status_bar = true;
    std::string appearance_status_yes = "22,bright_blue,bold";
    std::string appearance_status_no = "88,bright_blue,bold";

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
    // Programs that must be confirmed before every run even though the filter
    // allows them (SPEC.md §8.1). Comma-separated head tokens. Independent of
    // confirm_mode, exactly like on_blocked_command: --no-confirm does NOT
    // clear it. Empty by default so a run with no config file behaves as
    // before; the shipped config/tinyca.ini sets the interpreters.
    std::string tools_confirm_commands;

    // [skills]
    std::string skills_skills_dir = "./skills";
    bool skills_tier1_enabled = true;
    // Tier 2 (SPEC.md §7): off by default. It asks the model to emit an exact
    // trigger line, which is the kind of instruction-following a 1.5B model
    // cannot be relied on for -- enable it with a more capable model.
    bool skills_tier2_enabled = false;
    // Which of the three safeguards around model-written skills to enforce
    // (SPEC.md §7.1), comma-separated, any subset of:
    //   filter_on_create   - screen a newly created skill's body and delete it
    //                        if it invokes a program the filter forbids
    //   confirm_on_create  - ask the user before the skill-creating skill runs
    //   filter_at_startup  - screen every skill at startup; failures are
    //                        dropped (not advertised, not auto-allowed)
    // "none" disables all three.
    std::string skills_skill_checks = "filter_on_create,filter_at_startup";
    // Name of the skill that creates other skills; its invocation is what
    // `confirm_on_create` confirms.
    std::string skills_add_skill_name = "add_skill";
    // Skills that stay discovered, auto-allowed and runnable but are NOT
    // advertised in the Tier 1 index (SPEC.md §7). Comma-separated names.
    // Default excludes add_skill: a 1.5B model offered a "register a skill"
    // capability treats it as a shortcut for doing the work -- measured, it
    // abandoned a compile-and-run task to invent a download URL and register
    // a junk skill. Clear this when pointing TinyCA at a capable model.
    std::string skills_tier1_exclude = "add_skill";
    int skills_max_tier1_skills = 8;

    // CLI-only, not settable via the INI file
    std::string cli_task_prompt;                         // -p/--prompt; non-empty implies agent_mode = "oneshot"
    bool cli_dry_run = false;                            // --dry-run
    bool cli_help_requested = false;                     // --help
    bool cli_version_requested = false;                  // --version
    std::string cli_config_path = "./config/tinyca.ini"; // -c/--config
    // --on-prompt-change / --on-work-dir-change. Empty means "not given on the
    // command line", in which case agent.on_prompt_change and
    // agent.on_work_dir_change decide. Both apply only when attaching to an
    // EXISTING session, and are reported as having no effect otherwise.
    std::string cli_on_prompt_change;
    std::string cli_on_work_dir_change;
    // --session <name|path>: the session this run belongs to, created if it is
    // new. A bare name lives in sessions_dir; anything else is taken as a
    // transcript path. Empty means "no session named", i.e. a fresh one named
    // after the moment.
    std::string cli_session_name;
    // --no-replay: attach to an existing session WITHOUT replaying its
    // conversation into the model's context -- the same transcript, a fresh
    // conversation. Cheap on a small context window; the default is to replay,
    // because a session the user named is one they expect to be remembered.
    bool cli_no_replay = false;
    bool cli_list_sessions = false;            // --list-sessions
    std::string cli_rename_to;                 // --rename-to <name>, with --session <old>
    bool cli_transcript_path_explicit = false; // agent.transcript_log_path was set by config/CLI
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
