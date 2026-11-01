// BSD 3-Clause License — see LICENSE at the repository root.
//
// CLI entry point: argument/config loading, system prompt + skills assembly,
// wiring every component together, and the interactive REPL (SPEC.md §12)
// with TAB completion (§12.1) and colored output (§15).
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
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
#include "tinyca/resume.h"
#include "tinyca/session.h"
#include "tinyca/skills.h"
#include "tinyca/string_util.h"

#include "json.hpp"

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

// Absolute form of `path`, or `path` unchanged if it cannot be resolved (the
// directory may not exist yet). Used for the skills directory, which has to be
// absolute to be useful on the child's PATH: commands run with cwd = work_dir,
// so a relative "./skills" would resolve inside the work directory.
std::string absolute_or_same(const std::string &path)
{
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) != nullptr)
        return std::string(buf);
    return path;
}

bool file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
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
    LineEditor(std::vector<std::string> commands, std::vector<std::string> skill_names,
               std::vector<std::string> session_names)
        : commands_(std::move(commands)), skill_names_(std::move(skill_names)), session_names_(std::move(session_names))
    {
    }

    // Returns false on EOF (Ctrl+D / closed stdin).
    //
    // Raw mode is entered and restored inside this call, so the terminal is
    // only ever raw while a line is being typed. There is no cancel key:
    // SIGINT is blocked in every thread (ControlThread::start) and consumed
    // by sigwait, so Ctrl+C typed at this prompt aborts no task -- it is the
    // control thread's, and there is no task in flight while we are here.
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
        // "/rename <TAB>" completes stored session names -- renaming to a name
        // that is already taken is the one error this command can make, so
        // showing the taken ones is the useful completion (SPEC.md §12.1).
        const std::string rename_prefix = "/rename ";
        if (starts_with(line, rename_prefix))
        {
            const std::string typed = line.substr(rename_prefix.size());
            for (const auto &name : session_names_)
                if (starts_with(name, typed))
                    candidates.push_back(rename_prefix + name);
            return candidates;
        }
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
    std::vector<std::string> session_names_;
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
                 "  -p, --prompt <task>     run one task and exit (sets agent.mode = oneshot)\n"
                 "  -m, --model <id>        override llm.model_id\n"
                 "  -u, --url <base_url>    override llm.base_url\n"
                 "      --protocol <p>      fenced | hermes\n"
                 "      --no-confirm        confirm_mode = never\n"
                 "      --dry-run           parse and print actions, execute nothing\n"
                 "  -v, --verbose           log_level = debug\n"
                 "      --resume <file>     replay a prior transcript.jsonl as starting history\n"
                 "      --resume-prompt <p> ask | update | quit -- what to do when a resumed\n"
                 "                          session's system prompt differs (default: ask on a\n"
                 "                          TTY, quit otherwise)\n"
                 "      --resume-work-dir <p>  ask | continue | quit -- same, for a resumed\n"
                 "                          session recorded in a different work_dir\n"
                 "      --session <name>    create or continue a named session\n"
                 "      --list-sessions     list stored sessions and exit\n"
                 "      --rename-to <name>  with --session <old>: rename it and exit\n"
                 "      --version, --help\n"
                 "\n"
                 "Interactive commands: /reset /skills /sessions /rename <name> /history\n"
                 "                      /toggle_show_thinking /help /quit\n";
}

// skills.skill_checks is a comma-separated set (SPEC.md §7.1).
bool check_enabled(const Config &config, const std::string &which)
{
    for (const auto &token : split_trimmed(config.skills_skill_checks, ','))
        if (token == which)
            return true;
    return false;
}

// Reads the system prompt file and appends the skills index to it
// (SPEC.md §7). Discovery, the startup screen and the Tier 1/Tier 2 text all
// happen here, in this order, because each step depends on the previous one:
// a skill rejected by the screen must not be advertised, and the Tier 2
// trigger is only announced when it will actually be honoured.
std::string build_system_prompt(const Config &config, SkillRegistry &skills)
{
    bool found = false;
    std::string prompt = read_file_or_empty(config.agent_system_prompt_file, found);
    if (!found)
        std::cerr << "warning: system_prompt_file not found: " << config.agent_system_prompt_file
                  << " (using empty system prompt)\n";

    std::vector<std::string> skill_warnings;
    skills.discover(config.skills_skills_dir, skill_warnings);
    for (const auto &w : skill_warnings)
        std::cerr << "warning: " << w << "\n";

    // (iii) filter_at_startup (SPEC.md §7.1): screen every discovered skill's
    // body and drop the ones that invoke a program the operator's filter
    // forbids. Done before the index is rendered, so a rejected skill is never
    // advertised, and before the real CommandFilter is built from the
    // survivors, so it is never auto-allowed either.
    if (check_enabled(config, "filter_at_startup"))
    {
        CommandFilter screening_filter(CommandFilter::mode_from_string(config.tools_command_filter_mode),
                                       split(config.tools_whitelist, ','), split(config.tools_blacklist, ','),
                                       config.tools_allow_metacharacters, skills.auto_allow_tokens());
        std::vector<std::string> rejections;
        skills.validate_bodies(screening_filter, rejections);
        for (const auto &r : rejections)
            std::cerr << "warning: skill excluded (not advertised, not auto-allowed) -- " << r << "\n";
    }

    if (config.skills_tier1_enabled)
        prompt += "\n" + skills.render_tier1(static_cast<size_t>(config.skills_max_tier1_skills),
                                             split_trimmed(config.skills_tier1_exclude, ','));
    // Telling a model about a capability that is switched off is how you get it
    // emitted and then ignored.
    if (config.skills_tier2_enabled && !skills.skills().empty())
        prompt += "\nTo read a skill's source before using it, reply with exactly:\nshow_skill: <name>\n";

    return prompt;
}

// Picks up skills that appeared while the agent was working -- add_skill
// writing one, or anything else creating a file in skills_dir (SPEC.md §7.1).
// Screens each new skill when filter_on_create is enabled, makes the survivors
// runnable immediately, and returns one note per change for the caller to
// append to history.
//
// The system prompt is deliberately NOT rebuilt: history is append-only and
// rewriting the prefix would invalidate llama-server's KV cache (minutes on
// this hardware). The Tier 1 index picks the skill up at the next start, since
// the prompt is generated from a live scan every time TinyCA starts.
// Cost: one opendir+stat pass over skills_dir after every batch of commands,
// next to a turn that costs seconds of inference. Cheap enough to prefer over
// tracking directory state, which would have to be invalidated correctly to be
// worth anything.
void register_new_skills(const Config &config, CommandFilter &filter, std::vector<std::string> &known_names,
                         std::vector<std::string> &notes)
{
    SkillRegistry rescanned;
    std::vector<std::string> rescan_warnings;
    rescanned.discover(config.skills_skills_dir, rescan_warnings);

    for (const auto &found : rescanned.skills())
    {
        bool already_known = false;
        for (const auto &name : known_names)
            if (name == found.name)
                already_known = true;
        if (already_known)
            continue;
        known_names.push_back(found.name);

        if (check_enabled(config, "filter_on_create"))
        {
            // Screened on a copy: validate_bodies drops what it rejects, and
            // the scan above is the only one needed -- re-reading the
            // directory per new skill bought nothing.
            SkillRegistry screened = rescanned;
            std::vector<std::string> rejections;
            screened.validate_bodies(filter, rejections);

            std::string reason;
            for (const auto &r : rejections)
                if (r.compare(0, found.name.size() + 2, found.name + ": ") == 0)
                    reason = r;
            if (!reason.empty())
            {
                // Deleted, not merely hidden: the file is executable and its
                // path would otherwise stay invocable by name.
                std::remove(found.path.c_str());
                std::cerr << "warning: refused new skill -- " << reason << "\n";
                notes.push_back("The skill '" + found.name +
                                "' was rejected and deleted: it runs a program the command filter forbids. "
                                "Write a skill that only uses allowed commands.");
                continue;
            }
        }

        for (const auto &token : SkillRegistry::invocation_tokens(found))
            filter.add_auto_allowed(token);
        notes.push_back("New skill available now: " + found.name + " -- " + found.description +
                        "\n  usage: " + found.usage);
        std::cout << "(new skill registered: " << found.name << ")\n";
    }
}

// Replays a recorded session into `history`, reconciling what has changed since
// it was written (SPEC.md §12.2). Returns false when the user declines, in
// which case nothing was written and the caller must exit.
//
// Reconciliation is transactional: every difference is decided BEFORE anything
// is recorded, so a resume declined at any point leaves the transcript
// byte-identical.
bool resume_into(const Config &config, const std::string &system_prompt, const std::string &work_dir_abs,
                 Logger &logger, std::vector<Message> &history)
{
    ResumedSession session;
    std::string resume_err;
    if (!load_session(config.cli_resume_path, session, resume_err))
    {
        std::cerr << "warning: --resume: " << resume_err << " -- starting a fresh session\n";
    }
    else
    {
        // Reconciliation is transactional: every difference is decided
        // BEFORE anything is written, so a resume the user declines at
        // any point leaves the transcript byte-identical. Two things can
        // differ, and both are asked about before either is recorded.

        // 1. The system prompt. It is regenerated every startup (skills
        // are scanned live), so a resumed session may have run with a
        // different one. Adopting it silently would change the agent's
        // instructions behind the user's back; ignoring the difference
        // would replay history against a prompt that no longer describes
        // the available skills.
        const bool prompt_differs = session.found_system_prompt && session.stored_system_prompt != system_prompt;
        bool adopt_prompt = false;
        if (prompt_differs)
        {
            std::string policy = config.cli_resume_prompt_policy;
            if (policy.empty())
                policy = (isatty(STDIN_FILENO) != 0) ? "ask" : "quit";

            std::cerr << "The system prompt has changed since this session was recorded: "
                      << describe_prompt_difference(session.stored_system_prompt, system_prompt) << "\n";
            if (policy == "ask")
            {
                std::cout << "Update the session to the new system prompt? The whole conversation is then\n"
                             "re-sent to the model on the next request, which reprocesses the full prompt\n"
                             "(minutes on slow hardware). Answering no quits and leaves the transcript\n"
                             "untouched.\n";
                policy = confirm_prompt("Update the system prompt and continue?") ? "update" : "quit";
            }
            if (policy == "quit")
            {
                std::cerr << "resume aborted: the transcript was not modified. Re-run with "
                             "--resume-prompt update to adopt the new prompt.\n";
                return false;
            }
            adopt_prompt = true;
        }
        else if (!session.found_system_prompt)
        {
            std::cerr << "warning: --resume: the transcript records no system prompt (older format?) -- "
                         "continuing with the current one\n";
        }

        // 2. The work directory. Resuming elsewhere is sometimes exactly
        // what you want, but doing it silently means every relative path
        // in the replayed history ("sort.c", "./sort_test") now refers to
        // something else.
        const bool work_dir_differs = session.found_work_dir && session.stored_work_dir != work_dir_abs;
        bool adopt_work_dir = false;
        if (work_dir_differs)
        {
            std::string wd_policy = config.cli_resume_work_dir_policy;
            if (wd_policy.empty())
                wd_policy = (isatty(STDIN_FILENO) != 0) ? "ask" : "quit";

            std::cerr << "This session last ran with work_dir:\n  " << session.stored_work_dir
                      << "\nbut this run would use:\n  " << work_dir_abs << "\n";
            if (wd_policy == "ask")
            {
                std::cout << "Continuing here is fine, but file paths in the replayed history refer to the\n"
                             "old directory. Answering no quits, so you can restart from there or with\n"
                             "agent.work_dir = "
                          << session.stored_work_dir << " in your config.\n";
                wd_policy =
                    confirm_prompt("Continue in the current directory and update the session?") ? "continue" : "quit";
            }
            if (wd_policy == "quit")
            {
                std::cerr << "resume aborted: the transcript was not modified. Re-run with "
                             "--resume-work-dir continue to move the session here.\n";
                return false;
            }
            adopt_work_dir = true;
        }
        else if (!session.found_work_dir)
        {
            std::cerr << "warning: --resume: the transcript records no work_dir (older format?) -- "
                         "continuing with "
                      << work_dir_abs << "\n";
        }

        // Both decisions are in and both were "proceed" -- commit them.
        // Recorded as new entries rather than by rewriting the file: the
        // transcript is append-only, and what it must describe is what was
        // actually sent and where it ran, from here on.
        if (adopt_prompt)
        {
            logger.transcript("system_prompt_update",
                              "{\"reason\":\"resumed session adopted the current system prompt\",\"prompt\":" +
                                  nlohmann::json(system_prompt).dump() + "}");
            std::cerr << "system prompt updated; the next request re-sends the whole session\n";
        }
        if (adopt_work_dir)
        {
            logger.transcript("work_dir_update",
                              "{\"reason\":\"resumed session moved to the current work directory\",\"work_dir\":" +
                                  nlohmann::json(work_dir_abs).dump() +
                                  ",\"previous\":" + nlohmann::json(session.stored_work_dir).dump() + "}");
            std::cerr << "work_dir updated to " << work_dir_abs << "\n";
        }

        for (const auto &m : session.history)
            history.push_back(m);
        std::cerr << "resumed " << session.history.size() << " message(s) from " << config.cli_resume_path << "\n";
    }
    return true;
}

// Programs that must be confirmed before every run (SPEC.md §8.1), from the
// two places that ask for it: tools.confirm_commands, and -- when
// skills.skill_checks enables confirm_on_create -- the skill that creates
// skills, whose effect outlives the command that made it.
std::vector<std::string> confirm_required_tokens(const Config &config, const SkillRegistry &skills)
{
    std::vector<std::string> tokens = split_trimmed(config.tools_confirm_commands, ',');
    if (!check_enabled(config, "confirm_on_create"))
        return tokens;

    const std::vector<std::string> add_skill_tokens = skills.invocation_tokens_for(config.skills_add_skill_name);
    if (add_skill_tokens.empty())
        std::cerr << "warning: skills.skill_checks has confirm_on_create but no skill named '"
                  << config.skills_add_skill_name << "' was discovered\n";
    tokens.insert(tokens.end(), add_skill_tokens.begin(), add_skill_tokens.end());
    return tokens;
}

// One session listing, used by --list-sessions and by the REPL's /sessions.
// `current` is marked with "*"; pass an empty string when there is no current
// session to mark (a session name is never empty, so nothing matches).
void print_sessions(const std::string &sessions_dir, const std::string &current)
{
    const std::vector<SessionInfo> sessions = list_sessions(sessions_dir);
    if (sessions.empty())
    {
        std::cout << "no sessions in " << sessions_dir << "\n";
        return;
    }
    std::cout << "sessions in " << sessions_dir << " (newest first):\n";
    for (const auto &info : sessions)
    {
        char when[32] = {0};
        const time_t t = static_cast<time_t>(info.mtime_unix);
        struct tm local;
        if (localtime_r(&t, &local) != nullptr)
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
        std::cout << (info.name == current ? "* " : "  ") << info.name << "\n      " << when << "  " << info.tasks
                  << " task(s), " << info.entries << " entries, " << info.bytes << " bytes\n";
    }
}

// --list-sessions and --session <old> --rename-to <new> act on the session
// store and exit; neither needs a model, a work directory or a filter. Returns
// the process exit code, or -1 for "this run is not a maintenance command,
// carry on".
int run_maintenance_command(const Config &config)
{
    if (!config.cli_rename_to.empty())
    {
        if (config.cli_session_name.empty())
        {
            std::cerr << "fatal: --rename-to needs --session <name> to say which session to rename\n";
            return 1;
        }
        std::string rename_err;
        if (!rename_session(config.agent_sessions_dir, config.cli_session_name, config.cli_rename_to, rename_err))
        {
            std::cerr << "fatal: --rename-to: " << rename_err << "\n";
            return 1;
        }
        std::cout << "renamed session '" << config.cli_session_name << "' to '" << config.cli_rename_to << "'\n";
        return 0;
    }

    if (config.cli_list_sessions)
    {
        print_sessions(config.agent_sessions_dir, std::string());
        return 0;
    }

    return -1;
}

// Decides which transcript this run writes to, and what the session is called.
// In precedence order:
//   --resume <name|path>   the session being resumed
//   --session <name>       that named session, created if new
//   an explicit agent.transcript_log_path
//   otherwise              a new session named YYYY-MM-DD_hh-mm-ss_<work dir>
// Every session therefore has a name, including the ones nobody named.
// Returns false on an unusable --session name, which is fatal.
bool resolve_session_file(Config &config, std::string &session_name)
{
    if (!config.cli_resume_path.empty())
    {
        if (!config.cli_session_name.empty())
            std::cerr << "warning: --session is ignored when --resume is given; resuming '" << config.cli_resume_path
                      << "'\n";

        // A bare name resolves inside sessions_dir; anything else is taken as
        // a path, so existing --resume invocations keep working.
        if (valid_session_name(config.cli_resume_path) &&
            session_exists(config.agent_sessions_dir, config.cli_resume_path))
        {
            session_name = config.cli_resume_path;
            config.cli_resume_path = session_path(config.agent_sessions_dir, session_name);
        }
        else if (!file_exists(config.cli_resume_path))
        {
            // Neither a stored session nor a readable file. Carrying on would
            // start a fresh session logging to a file named after the typo,
            // in the current directory, invisible to --list-sessions -- so
            // this is refused instead.
            std::cerr << "fatal: --resume: no stored session or transcript file named '" << config.cli_resume_path
                      << "'. Use --list-sessions to see what is there.\n";
            return false;
        }
        config.agent_transcript_log_path = config.cli_resume_path;
        return true;
    }
    if (!config.cli_session_name.empty())
    {
        if (!valid_session_name(config.cli_session_name))
        {
            std::cerr << "fatal: --session: '" << config.cli_session_name
                      << "' is not a usable session name (letters, digits, '_', '-', '.' only)\n";
            return false;
        }
        session_name = config.cli_session_name;
        const bool existed = session_exists(config.agent_sessions_dir, session_name);
        config.agent_transcript_log_path = session_path(config.agent_sessions_dir, session_name);
        std::cerr << (existed ? "continuing session: " : "new session: ") << session_name << "\n";
        return true;
    }
    if (!config.cli_transcript_path_explicit)
    {
        // Unnamed: name it after the moment and the work directory, keeping
        // the name unique -- two sessions started in the same second in the
        // same place must not share one transcript.
        session_name =
            unique_session_name(config.agent_sessions_dir, default_session_name(absolute_or_same(config.agent_work_dir),
                                                                                static_cast<long>(time(nullptr))));
        config.agent_transcript_log_path = session_path(config.agent_sessions_dir, session_name);
        std::cerr << "new session: " << session_name << "\n";
    }
    return true;
}

// SPEC.md §7's Tier 2, user-initiated (what §12.1's `show_skill:` TAB
// completion implies): print a skill's source and append it to history as a
// `user` message, so the next task is answered by a model that has the source
// in context. No LLM call, no execution -- this is a local lookup.
void handle_show_skill(const std::string &line, const Config &config, const SkillRegistry &skills,
                       std::vector<Message> &history)
{
    const std::string prefix = "show_skill:";
    std::string name = trim(line.substr(prefix.size()));
    if (!config.skills_tier2_enabled)
    {
        std::cout << "Tier 2 skills are disabled; set skills.tier2_enabled = true to use show_skill:\n";
        return;
    }
    if (name.empty())
    {
        std::cout << "usage: show_skill: <name>\n";
        return;
    }
    std::string body;
    if (!skills.load_tier2(name, body))
    {
        std::cout << "no skill named '" << name << "'. Available:";
        for (const auto &s : skills.skills())
            std::cout << " " << s.name;
        std::cout << "\n";
        return;
    }
    std::cout << body << "\n";
    Message m;
    m.role = "user";
    m.content = "Source of skill '" + name + "':\n" + body;
    history.push_back(m);
    std::cout << "(added to the conversation history)\n";
}

// Handles one REPL slash command. Returns false if the REPL should exit.
bool handle_slash_command(const std::string &cmd, std::vector<Message> &history, const Message &system_message,
                          const SkillRegistry &skills, bool &show_thinking, const Config &config,
                          std::string &session_name, Logger &logger)
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
    else if (cmd == "/sessions")
    {
        const std::vector<SessionInfo> sessions = list_sessions(config.agent_sessions_dir);
        if (sessions.empty())
            std::cout << "no sessions in " << config.agent_sessions_dir << "\n";
        for (const auto &info : sessions)
            std::cout << (info.name == session_name ? "* " : "  ") << info.name << "  (" << info.tasks << " task(s), "
                      << info.bytes << " bytes)\n";
    }
    else if (starts_with(cmd, "/rename"))
    {
        // Renames the session being written right now: move the file, point
        // the logger at it, and record the move in the file itself so the
        // transcript still explains its own history (append-only, like
        // system_prompt_update and work_dir_update).
        const std::string new_name = trim(cmd.substr(std::string("/rename").size()));
        if (session_name.empty())
        {
            std::cout << "this run has no named session (agent.transcript_log_path was set explicitly)\n";
        }
        else if (new_name.empty())
        {
            std::cout << "usage: /rename <new-name>   (current: " << session_name << ")\n";
        }
        else
        {
            std::string rename_err;
            if (!rename_session(config.agent_sessions_dir, session_name, new_name, rename_err))
            {
                std::cout << "cannot rename: " << rename_err << "\n";
            }
            else
            {
                const std::string previous = session_name;
                session_name = new_name;
                logger.set_path(session_path(config.agent_sessions_dir, new_name));
                logger.transcript("session_rename", "{\"previous\":" + nlohmann::json(previous).dump() +
                                                        ",\"current\":" + nlohmann::json(new_name).dump() + "}");
                std::cout << "session renamed to " << new_name << "\n";
            }
        }
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

    // Commands that act on the session store and exit, before any of the
    // agent's own setup happens.
    const int maintenance_status = run_maintenance_command(config);
    if (maintenance_status >= 0)
        return maintenance_status;

    mkdir_p(config.agent_work_dir);

    // Which file is this session? (SPEC.md §12.3)
    mkdir_p(config.agent_sessions_dir);
    std::string session_name;
    if (!resolve_session_file(config, session_name))
        return 1;

    size_t slash = config.agent_transcript_log_path.find_last_of('/');
    if (slash != std::string::npos)
        mkdir_p(config.agent_transcript_log_path.substr(0, slash));

    // Assembled from the prompt file plus a live scan of skills_dir, so it
    // always describes exactly the skills that are runnable in this session.
    SkillRegistry skills;
    const std::string system_prompt = build_system_prompt(config, skills);

    std::string grammar_text;
    if (config.protocol_enable_grammar)
    {
        bool grammar_found = false;
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
    // Skills are invoked by bare filename (SPEC.md §7); their directory has to
    // be on the child's PATH for that to work from inside work_dir.
    const std::string skills_dir_abs = absolute_or_same(config.skills_skills_dir);
    executor.set_path_prepend(skills_dir_abs);
    // add_skill needs to know where to write; it runs with cwd = work_dir.
    executor.set_env("TINYCA_SKILLS_DIR", skills_dir_abs);

    std::vector<std::string> auto_allowed = skills.auto_allow_tokens();
    CommandFilter filter(CommandFilter::mode_from_string(config.tools_command_filter_mode),
                         split(config.tools_whitelist, ','), split(config.tools_blacklist, ','),
                         config.tools_allow_metacharacters, auto_allowed);

    const std::vector<std::string> confirm_tokens = confirm_required_tokens(config, skills);
    if (!confirm_tokens.empty())
        filter.set_confirm_required(confirm_tokens);

    Logger logger(config.agent_transcript_log_path, config.agent_transcript_max_bytes,
                  config.agent_transcript_max_bytes_per_run, Logger::level_from_string(config.agent_log_level));

    // Absolute, because the point is to compare it across invocations started
    // from different working directories (SPEC.md §12.2). The entry itself is
    // written below, after any resume reconciliation -- a resume the user
    // declines must leave the transcript byte-identical.
    const std::string work_dir_abs = absolute_or_same(config.agent_work_dir);

    ControlThread control(&transport, &executor);
    control.start(); // first, before any other thread -- SPEC.md §9.1 / control.h

    Agent agent(transport, codec, *protocol, executor, filter, logger, control, config);
    agent.set_confirm_callback(confirm_prompt);
    if (config.skills_tier2_enabled)
        agent.set_tier2_source(&skills);

    // A skill created mid-session (SPEC.md §7.1). Re-scan, screen the new
    // ones if filter_on_create is enabled, make the survivors runnable right
    // away, and hand back one note per change for the Agent to append to
    // history. The system prompt is deliberately NOT rebuilt: rewriting it
    // would invalidate llama-server's KV prefix (minutes on this hardware),
    // and history is append-only. The Tier 1 index picks the skill up on the
    // next start.
    std::vector<std::string> known_skill_names;
    for (const auto &s : skills.skills())
        known_skill_names.push_back(s.name);

    agent.set_on_skills_may_have_changed(
        [&](std::vector<std::string> &notes) { register_new_skills(config, filter, known_skill_names, notes); });

    Colors colors = resolve_colors(config.agent_color_output);
    bool show_thinking = false;
    agent.set_on_assistant_text_chunk(
        [&](const std::string &chunk) { std::cout << colors.assistant() << chunk << colors.reset() << std::flush; });
    // SPEC.md §15: the command about to run is bold yellow and prefixed with
    // "▶"; its output is printed unstyled, since ANSI codes mixed into
    // arbitrary command output risk breaking it.
    agent.set_on_action([&](const std::string &command) {
        std::cout << "\n" << colors.action() << "▶ " << command << colors.reset() << "\n" << std::flush;
    });
    agent.set_on_tool_output([&](const std::string &output) { std::cout << output << "\n" << std::flush; });
    agent.set_on_think_chunk([&](const std::string &chunk) {
        if (show_thinking)
            std::cout << colors.think() << chunk << colors.reset() << std::flush;
    });

    Message system_message;
    system_message.role = "system";
    system_message.content = system_prompt;

    std::vector<Message> history;
    history.push_back(system_message);

    if (!config.cli_resume_path.empty() && !resume_into(config, system_prompt, work_dir_abs, logger, history))
    {
        control.stop();
        return 1;
    }

    // Recorded every run, so a later --resume can tell whether it is being
    // resumed in the same place. Written here rather than at startup because
    // everything above can still decide to abort, and an aborted resume must
    // not touch the file.
    logger.transcript("session_start", "{\"work_dir\":" + nlohmann::json(work_dir_abs).dump() + "}");

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
    //
    // AUDIT NOTE: the cli_task_prompt branch below is currently unreachable.
    // -p/--prompt is the only thing that sets cli_task_prompt and it also
    // forces agent_mode = "oneshot" (src/config.cpp), which returns above.
    // It exists for SPEC.md §12's positional `[task]` argument -- "run this
    // one task, then stay interactive" -- which is not wired up: parse_cli
    // ignores non-option arguments.
    std::vector<std::string> skill_names;
    for (const auto &s : skills.skills())
        skill_names.push_back(s.name);
    std::vector<std::string> session_names;
    for (const auto &info : list_sessions(config.agent_sessions_dir))
        session_names.push_back(info.name);
    LineEditor editor(
        {"/reset", "/skills", "/sessions", "/rename", "/history", "/toggle_show_thinking", "/help", "/quit"},
        skill_names, session_names);

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
            if (!handle_slash_command(trimmed, history, system_message, skills, show_thinking, config, session_name,
                                      logger))
                break;
            continue;
        }
        if (starts_with(trimmed, "show_skill:"))
        {
            handle_show_skill(trimmed, config, skills, history);
            continue;
        }

        std::string result = agent.run_task(history, trimmed);
        print_final_result(result);
    }

    control.stop();
    return 0;
}
