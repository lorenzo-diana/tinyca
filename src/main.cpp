// BSD 3-Clause License — see LICENSE at the repository root.
//
// CLI entry point: argument/config loading, system prompt + skills assembly,
// wiring every component together, and the interactive REPL (SPEC.md §12)
// with TAB completion (§12.1) and colored output (§15).
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include "tinyca/agent.h"
#include "tinyca/codec.h"
#include "tinyca/command_filter.h"
#include "tinyca/config.h"
#include "tinyca/control.h"
#include "tinyca/executor.h"
#include "tinyca/fenced_protocol.h"
#include "tinyca/hermes_protocol.h"
#include "tinyca/http_transport.h"
#include "tinyca/logger.h"
#include "tinyca/skills.h"
#include "tinyca/string_util.h"

namespace
{

using namespace tinyca;

void mkdir_p(const std::string &path)
{
    if (path.empty() || path == "." || path == "/" || path == "./")
        return;
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0)
        mkdir_p(path.substr(0, pos));
    mkdir(path.c_str(), 0755); // ignore EEXIST and friends
}

std::string read_file_or_empty(const std::string &path, bool &found)
{
    std::ifstream f(path);
    found = f.is_open();
    if (!found)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- terminal color (SPEC.md §15) ----

struct Colors
{
    bool enabled = false;
    const char *user_input() const
    {
        return enabled ? "\x1b[1;36m" : "";
    }
    const char *think() const
    {
        return enabled ? "\x1b[2;37m" : "";
    }
    const char *assistant() const
    {
        return enabled ? "\x1b[32m" : "";
    }
    const char *action() const
    {
        return enabled ? "\x1b[1;33m" : "";
    }
    const char *reset() const
    {
        return enabled ? "\x1b[0m" : "";
    }
};

Colors resolve_colors(const std::string &mode)
{
    Colors c;
    if (mode == "always")
        c.enabled = true;
    else if (mode == "never")
        c.enabled = false;
    else
        c.enabled = isatty(STDOUT_FILENO) != 0;
    return c;
}

// ---- TAB-completing line editor (SPEC.md §12.1) ----

class LineEditor
{
  public:
    LineEditor(std::vector<std::string> commands, std::vector<std::string> skill_names)
        : commands_(std::move(commands)), skill_names_(std::move(skill_names))
    {
    }

    // Returns false on EOF (Ctrl+D / closed stdin).
    bool read_line(const std::string &prompt, std::string &out)
    {
        if (isatty(STDIN_FILENO) == 0)
        {
            std::cout << prompt;
            return static_cast<bool>(std::getline(std::cin, out));
        }

        std::cout << prompt << std::flush;
        termios orig;
        tcgetattr(STDIN_FILENO, &orig);
        termios raw = orig;
        raw.c_lflag &= ~(static_cast<unsigned long>(ICANON) | static_cast<unsigned long>(ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        std::string line;
        bool eof = false;
        while (true)
        {
            char c = 0;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0)
            {
                eof = true;
                break;
            }
            if (c == '\n' || c == '\r')
            {
                std::cout << "\n";
                break;
            }
            if (c == 127 || c == 8) // backspace / DEL
            {
                if (!line.empty())
                {
                    line.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
                continue;
            }
            if (c == '\t')
            {
                std::vector<std::string> matches = complete(line);
                if (matches.size() == 1)
                {
                    std::string suffix = matches[0].substr(line.size());
                    line += suffix;
                    std::cout << suffix << std::flush;
                }
                else if (matches.size() > 1)
                {
                    std::cout << "\n";
                    for (const auto &m : matches)
                        std::cout << m << "  ";
                    std::cout << "\n" << prompt << line << std::flush;
                }
                continue;
            }
            if (c >= 32 && c < 127)
            {
                line += c;
                std::cout << c << std::flush;
            }
        }

        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        out = line;
        return !eof;
    }

  private:
    std::vector<std::string> complete(const std::string &line) const
    {
        std::vector<std::string> candidates;
        if (starts_with(line, "/"))
        {
            for (const auto &cmd : commands_)
                if (starts_with(cmd, line))
                    candidates.push_back(cmd);
        }
        else if (starts_with(line, "show_skill:"))
        {
            const std::string prefix = "show_skill:";
            std::string typed = line.substr(prefix.size());
            for (const auto &s : skill_names_)
                if (starts_with(s, typed))
                    candidates.push_back(prefix + s);
        }
        return candidates;
    }

    std::vector<std::string> commands_;
    std::vector<std::string> skill_names_;
};

bool confirm_prompt(const std::string &message)
{
    std::cout << "\n" << message << " [y/N] " << std::flush;
    std::string response;
    if (!std::getline(std::cin, response))
        return false;
    return !response.empty() && (response[0] == 'y' || response[0] == 'Y');
}

void print_help()
{
    std::cout << "tinyca [options] [task]\n"
                 "  -c, --config <file>     default ./config/tinyca.ini\n"
                 "  -p, --prompt <task>     one-shot mode; implies --mode oneshot\n"
                 "  -m, --model <id>        override llm.model_id\n"
                 "  -u, --url <base_url>    override llm.base_url\n"
                 "      --protocol <p>      fenced | hermes\n"
                 "      --no-confirm        confirm_mode = never\n"
                 "      --dry-run           parse and print actions, execute nothing\n"
                 "  -v, --verbose           log_level = debug\n"
                 "      --version, --help\n"
                 "\n"
                 "Interactive commands: /reset /skills /history /toggle_show_thinking /help /quit\n";
}

// Handles one REPL slash command. Returns false if the REPL should exit.
bool handle_slash_command(const std::string &cmd, std::vector<Message> &history, const Message &system_message,
                          const SkillRegistry &skills, bool &show_thinking)
{
    if (cmd == "/quit")
        return false;
    if (cmd == "/reset")
    {
        history.clear();
        history.push_back(system_message);
        std::cout << "history reset.\n";
    }
    else if (cmd == "/skills")
    {
        for (const auto &s : skills.skills())
            std::cout << "- " << s.name << ": " << s.description << "\n  usage: " << s.usage << "\n";
    }
    else if (cmd == "/history")
    {
        for (const auto &m : history)
            std::cout << "[" << m.role << "] " << m.content << "\n";
    }
    else if (cmd == "/toggle_show_thinking")
    {
        show_thinking = !show_thinking;
        std::cout << "show_thinking: " << (show_thinking ? "on" : "off") << "\n";
    }
    else if (cmd == "/help")
    {
        print_help();
    }
    else
    {
        std::cout << "unknown command: " << cmd << " (try /help)\n";
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Config config;
    ConfigLoadResult load_result = load_config(argc, argv, config);
    for (const auto &w : load_result.warnings)
        std::cerr << "warning: " << w << "\n";
    if (config.cli_help_requested)
    {
        print_help();
        return 0;
    }
    if (config.cli_version_requested)
    {
        std::cout << "tinyca 0.1.0\n";
        return 0;
    }
    if (!load_result.ok)
    {
        std::cerr << "fatal: " << load_result.fatal_error << "\n";
        return 1;
    }

    mkdir_p(config.agent_work_dir);
    size_t slash = config.agent_transcript_log_path.find_last_of('/');
    if (slash != std::string::npos)
        mkdir_p(config.agent_transcript_log_path.substr(0, slash));

    bool found;
    std::string system_prompt = read_file_or_empty(config.agent_system_prompt_file, found);
    if (!found)
    {
        std::cerr << "warning: system_prompt_file not found: " << config.agent_system_prompt_file
                  << " (using empty system prompt)\n";
    }

    SkillRegistry skills;
    std::vector<std::string> skill_warnings;
    skills.discover(config.skills_skills_dir, skill_warnings);
    for (const auto &w : skill_warnings)
        std::cerr << "warning: " << w << "\n";
    if (config.skills_tier1_enabled)
        system_prompt += "\n" + skills.render_tier1(static_cast<size_t>(config.skills_max_tier1_skills));

    std::string grammar_text;
    if (config.protocol_enable_grammar)
    {
        bool grammar_found;
        grammar_text = read_file_or_empty(config.protocol_grammar_file, grammar_found);
        if (!grammar_found)
        {
            std::cerr << "fatal: protocol.enable_grammar=true but grammar_file not found: "
                      << config.protocol_grammar_file << "\n";
            return 1;
        }
    }

    std::unique_ptr<IProtocol> protocol;
    if (config.protocol_tool_protocol == "hermes")
        protocol.reset(new HermesProtocol(grammar_text));
    else
        protocol.reset(new FencedProtocol());

    OpenAIChatCodec codec;
    HttpTransport transport(config.llm_base_url, config.llm_connect_timeout_s, config.llm_request_timeout_s);
    Executor executor(config.agent_work_dir, config.tools_bash_timeout_s, config.tools_output_head_lines,
                      config.tools_output_tail_lines, config.tools_output_max_bytes);

    std::vector<std::string> auto_allowed = skills.paths();
    CommandFilter filter(CommandFilter::mode_from_string(config.tools_command_filter_mode),
                         split(config.tools_whitelist, ','), split(config.tools_blacklist, ','),
                         config.tools_allow_metacharacters, auto_allowed);

    Logger logger(config.agent_transcript_log_path, config.agent_transcript_max_bytes,
                  Logger::level_from_string(config.agent_log_level));

    ControlThread control(&transport, &executor);
    control.start(); // first, before any other thread -- SPEC.md §9.1 / control.h

    Agent agent(transport, codec, *protocol, executor, filter, logger, control, config);
    agent.set_confirm_callback(confirm_prompt);

    Colors colors = resolve_colors(config.agent_color_output);
    bool show_thinking = false;
    agent.set_on_assistant_text_chunk(
        [&](const std::string &chunk) { std::cout << colors.assistant() << chunk << colors.reset() << std::flush; });
    agent.set_on_think_chunk([&](const std::string &chunk) {
        if (show_thinking)
        {
            std::cout << colors.think() << chunk << colors.reset() << std::flush;
        }
    });

    Message system_message;
    system_message.role = "system";
    system_message.content = system_prompt;

    std::vector<Message> history;
    history.push_back(system_message);

    // If streaming was on, the final text was already shown live via
    // on_assistant_text_chunk_ -- printing `result` again would duplicate
    // it. Abort/control messages ("aborted: ...", "iteration limit
    // reached") are never streamed, so they always need printing here.
    auto print_final_result = [&](const std::string &result) {
        std::cout << "\n"; // always separate from whatever was streamed live, if anything
        if (!agent.last_result_was_streamed())
        {
            std::cout << colors.assistant() << result << colors.reset() << "\n";
        }
    };

    if (config.agent_mode == "oneshot")
    {
        if (config.cli_task_prompt.empty())
        {
            std::cerr << "fatal: mode=oneshot requires a task (-p/--prompt)\n";
            control.stop();
            return 1;
        }
        std::string result = agent.run_task(history, config.cli_task_prompt);
        print_final_result(result);
        control.stop();
        return 0;
    }

    // Interactive REPL.
    std::vector<std::string> skill_names;
    for (const auto &s : skills.skills())
        skill_names.push_back(s.name);
    LineEditor editor({"/reset", "/skills", "/history", "/toggle_show_thinking", "/help", "/quit"}, skill_names);

    std::cout << "TinyCA ready. Type a task, or /help for commands.\n";
    if (!config.cli_task_prompt.empty())
    {
        std::string result = agent.run_task(history, config.cli_task_prompt);
        print_final_result(result);
    }

    while (true)
    {
        std::string line;
        if (!editor.read_line(std::string(colors.user_input()) + "> " + colors.reset(), line))
            break; // EOF
        std::string trimmed = trim(line);
        if (trimmed.empty())
            continue;
        if (starts_with(trimmed, "/"))
        {
            if (!handle_slash_command(trimmed, history, system_message, skills, show_thinking))
                break;
            continue;
        }

        std::string result = agent.run_task(history, trimmed);
        print_final_result(result);
    }

    control.stop();
    return 0;
}
