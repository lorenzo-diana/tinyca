// BSD 3-Clause License — see LICENSE at the repository root.
// Resuming a recorded session (SPEC.md §12.2): what is replayed, what is not,
// and how a changed system prompt is detected.
#include "tinyca/resume.h"

#include <cstdio>
#include <fstream>

#include "test_util.h"

namespace
{

using tinyca::describe_prompt_difference;
using tinyca::load_session;
using tinyca::ResumedSession;
using tinyca::render_restored_conversation;
using tinyca::resumed_bool_option;

// A transcript in the exact shape Logger writes, including entry kinds that
// resume must ignore and one deliberately truncated line.
void write_transcript(const std::string& path, const std::string& system_prompt)
{
    std::ofstream f(path, std::ios::trunc);
    f << R"({"ts":1,"kind":"user_task","payload":{"text":"list the files"}})" << "\n";
    f << R"({"ts":2,"kind":"request","payload":{"messages":[{"role":"system","content":")" << system_prompt
      << R"("},{"role":"user","content":"list the files"}],"model":"m"}})" << "\n";
    f << R"({"ts":3,"kind":"response","payload":{"content":"```bash\nls -l\n```","role":"assistant"}})" << "\n";
    f << R"({"ts":4,"kind":"tool_result","payload":{"content":"a\nb\n[exit code: 0]","ok":true}})" << "\n";
    f << R"({"ts":5,"kind":"skills_changed","payload":{"note":"New skill available now: x"}})" << "\n";
    f << R"({"ts":6,"kind":"response","payload":{"content":"Two files.","role":"assistant"}})" << "\n";
    f << R"({"ts":7,"kind":"transcript_cutoff","payload":{"reas)" << "\n"; // truncated on purpose
}

// A session records the directory it ran in, and a session that moved records
// the move; the most recent entry is what counts (SPEC.md §12.2).
void test_work_dir_is_recovered_and_the_latest_one_wins()
{
    const std::string path = "build/tests/resume_workdir.jsonl";
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"ts":1,"kind":"session_start","payload":{"work_dir":"/home/a/first"}})" << "\n";
        f << R"({"ts":2,"kind":"user_task","payload":{"text":"hi"}})" << "\n";
        f << R"({"ts":3,"kind":"work_dir_update","payload":{"work_dir":"/home/a/second","previous":"/home/a/first"}})"
          << "\n";
    }

    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));
    TINYCA_CHECK(session.found_work_dir);
    TINYCA_CHECK_EQ(session.stored_work_dir, std::string("/home/a/second"));
    // session_start / work_dir_update are session metadata, never replayed as
    // conversation turns.
    TINYCA_CHECK_EQ(session.history.size(), static_cast<size_t>(1));
}

// An older transcript has no such entry: reported, not guessed.
void test_transcript_without_a_work_dir()
{
    const std::string path = "build/tests/resume_no_workdir.jsonl";
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"ts":1,"kind":"user_task","payload":{"text":"hi"}})" << "\n";
    }
    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));
    TINYCA_CHECK(!session.found_work_dir);
    TINYCA_CHECK(session.stored_work_dir.empty());
}

void test_missing_file_is_reported_not_crashed()
{
    ResumedSession session;
    std::string err;
    TINYCA_CHECK(!load_session("build/tests/does_not_exist.jsonl", session, err));
    TINYCA_CHECK(err.find("cannot open transcript file") != std::string::npos);
    TINYCA_CHECK(session.history.empty());
}

void test_replays_turns_and_skips_derived_entries()
{
    const std::string path = "build/tests/resume_basic.jsonl";
    write_transcript(path, "PROMPT A");

    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));

    // user, assistant, tool, assistant -- request/skills_changed/cutoff and
    // the truncated line are all skipped.
    TINYCA_CHECK_EQ(session.history.size(), static_cast<size_t>(4));
    TINYCA_CHECK_EQ(session.history[0].role, std::string("user"));
    TINYCA_CHECK_EQ(session.history[1].role, std::string("assistant"));
    TINYCA_CHECK_EQ(session.history[2].role, std::string("tool"));
    TINYCA_CHECK_EQ(session.history[3].content, std::string("Two files."));
    // No replayed entry is ever a system message: the current prompt wins.
    for (const auto& m : session.history)
    {
        TINYCA_CHECK(m.role != "system");
    }
}

void test_stored_system_prompt_is_recovered_from_the_first_request()
{
    const std::string path = "build/tests/resume_prompt.jsonl";
    write_transcript(path, "PROMPT A");

    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));
    TINYCA_CHECK(session.found_system_prompt);
    TINYCA_CHECK_EQ(session.stored_system_prompt, std::string("PROMPT A"));
}

// A transcript whose requests carry no system message (an older file, or a
// session that ran with an empty prompt) must be flagged rather than reported
// as "the prompt matches".
void test_transcript_without_a_system_prompt()
{
    const std::string path = "build/tests/resume_no_prompt.jsonl";
    std::ofstream f(path, std::ios::trunc);
    f << R"({"ts":1,"kind":"user_task","payload":{"text":"hi"}})" << "\n";
    f << R"({"ts":2,"kind":"request","payload":{"messages":[{"role":"user","content":"hi"}]}})" << "\n";
    f.close();

    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));
    TINYCA_CHECK(!session.found_system_prompt);
    TINYCA_CHECK(session.stored_system_prompt.empty());
    TINYCA_CHECK_EQ(session.history.size(), static_cast<size_t>(1));
}

void test_describe_prompt_difference()
{
    TINYCA_CHECK_EQ(describe_prompt_difference("same\ntext", "same\ntext"), std::string("identical"));

    // A skill added to the Tier 1 index changes a line partway down.
    const std::string before = "header\n- a: one\n";
    const std::string after = "header\n- a: one\n- b: two\n";
    const std::string described = describe_prompt_difference(before, after);
    TINYCA_CHECK(described.find("first difference at line 3") != std::string::npos);
    TINYCA_CHECK(described.find("bytes") != std::string::npos);
}

// An option changed inside a session (/toggle_show_thinking) is part of how
// that session was left, like session_rename and work_dir_update -- so it is
// recorded and read back rather than reverting to the config default on every
// restart.
void test_option_changes_are_recovered_and_the_latest_one_wins()
{
    const std::string path = "build/tests/resume_options.jsonl";
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"ts":1,"kind":"session_start","payload":{"work_dir":"/w"}})" << "\n";
        f << R"({"ts":2,"kind":"option_change","payload":{"key":"agent.show_thinking","value":"true"}})" << "\n";
        f << R"({"ts":3,"kind":"user_task","payload":{"text":"hi"}})" << "\n";
        // Toggled off and on again: the transcript is append-only, so all three
        // are there and only the last one is the state.
        f << R"({"ts":4,"kind":"option_change","payload":{"key":"agent.show_thinking","value":"false"}})" << "\n";
        f << R"({"ts":5,"kind":"option_change","payload":{"key":"agent.show_thinking","value":"true"}})" << "\n";
    }
    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));
    TINYCA_CHECK_EQ(session.options.size(), static_cast<size_t>(1));

    // The option entries are state, not conversation: they must not appear as
    // messages to the model.
    TINYCA_CHECK_EQ(session.history.size(), static_cast<size_t>(1));

    bool show_thinking = false; // the config default
    std::string option_err;
    TINYCA_CHECK(resumed_bool_option(session, "agent.show_thinking", show_thinking, option_err));
    TINYCA_CHECK(show_thinking);
    TINYCA_CHECK_EQ(option_err, std::string(""));
    std::remove(path.c_str());
}

void test_untouched_and_malformed_options_leave_the_default_alone()
{
    const std::string path = "build/tests/resume_options_bad.jsonl";
    {
        std::ofstream f(path, std::ios::trunc);
        f << R"({"ts":1,"kind":"option_change","payload":{"key":"agent.show_thinking","value":"yes please"}})" << "\n";
        f << R"({"ts":2,"kind":"option_change","payload":{"value":"true"}})" << "\n"; // no key
    }
    ResumedSession session;
    std::string err;
    TINYCA_CHECK(load_session(path, session, err));

    // A session that never changed the option: the caller's default stands and
    // the call says so, rather than returning a value the session never set.
    ResumedSession empty;
    bool value = true;
    std::string option_err;
    TINYCA_CHECK(!resumed_bool_option(empty, "agent.show_thinking", value, option_err));
    TINYCA_CHECK(value); // untouched

    // A value that is neither true nor false is reported and treated as
    // absent: a corrupt transcript must not flip a setting silently.
    value = false;
    TINYCA_CHECK(!resumed_bool_option(session, "agent.show_thinking", value, option_err));
    TINYCA_CHECK(!value); // untouched
    TINYCA_CHECK(option_err.find("yes please") != std::string::npos);
    std::remove(path.c_str());
}

// What a resumed session shows the user. The rendering is a pure function so
// that what is shown -- and what is deliberately NOT shown -- can be asserted
// without a terminal.
void test_restored_conversation_shows_the_turns_in_their_own_colours()
{
    tinyca::Appearance style;
    style.enabled = true;
    style.user_input = "<U>";
    style.assistant = "<A>";

    std::vector<tinyca::Message> history;
    tinyca::Message system;
    system.role = "system";
    system.content = "You are TinyCA.";
    history.push_back(system);
    tinyca::Message task;
    task.role = "user";
    task.content = "count the files";
    history.push_back(task);
    tinyca::Message reply;
    reply.role = "assistant";
    reply.content = "```bash\nls | wc -l\n```";
    history.push_back(reply);
    tinyca::Message tool;
    tool.role = "tool";
    tool.content = "Output of `ls | wc -l`:\n4\n[exit code: 0]";
    history.push_back(tool);
    tinyca::Message answer;
    answer.role = "assistant";
    answer.content = "There are 4 files.";
    history.push_back(answer);

    const std::string shown = render_restored_conversation(history, style);

    // The user's task, with the live prompt's "> " so it reads as it did when
    // it was typed, in the user colour.
    TINYCA_CHECK(shown.find("<U>> count the files\x1b[0m\n") != std::string::npos);
    // The model's turns, in the assistant colour.
    TINYCA_CHECK(shown.find("<A>There are 4 files.\x1b[0m\n") != std::string::npos);
    // The command stays visible: it is part of the model's own text.
    TINYCA_CHECK(shown.find("ls | wc -l") != std::string::npos);

    // Not shown: the system prompt, and tool output (which can dwarf the turns
    // that caused it).
    TINYCA_CHECK(shown.find("You are TinyCA.") == std::string::npos);
    TINYCA_CHECK(shown.find("[exit code: 0]") == std::string::npos);

    // Every line of a multi-line turn carries the style, not just the first:
    // the fence, the command and the closing fence are three lines.
    TINYCA_CHECK(shown.find("<A>```bash\x1b[0m\n<A>ls | wc -l\x1b[0m\n<A>```\x1b[0m\n") != std::string::npos);

    // A stored reply ends with its own newline; splitting on it as-is appended
    // an empty line, which with a style applied was a blank coloured row after
    // every turn.
    tinyca::Message trailing;
    trailing.role = "assistant";
    trailing.content = "done.\n";
    std::vector<tinyca::Message> one;
    one.push_back(trailing);
    TINYCA_CHECK_EQ(render_restored_conversation(one, style), std::string("<A>done.\x1b[0m\n"));

    // Blank lines INSIDE a turn are the author's and are kept.
    tinyca::Message spaced;
    spaced.role = "assistant";
    spaced.content = "first\n\nsecond\n";
    one.clear();
    one.push_back(spaced);
    TINYCA_CHECK_EQ(render_restored_conversation(one, tinyca::Appearance()),
                    std::string("first\n\nsecond\n"));

    // Unstyled output is plain text, with no stray resets.
    const std::string plain = render_restored_conversation(history, tinyca::Appearance());
    TINYCA_CHECK(plain.find("\x1b") == std::string::npos);
    TINYCA_CHECK(plain.find("> count the files\n") != std::string::npos);
}

void test_restored_conversation_of_a_fresh_session_is_empty()
{
    tinyca::Appearance style;
    std::vector<tinyca::Message> history;
    tinyca::Message system;
    system.role = "system";
    system.content = "You are TinyCA.";
    history.push_back(system);
    // Nothing has been said yet: a banner with nothing under it would read as
    // a session that lost its history.
    TINYCA_CHECK_EQ(render_restored_conversation(history, style), std::string(""));

    // An empty assistant turn (the model returned nothing) contributes no
    // blank line either.
    tinyca::Message empty_reply;
    empty_reply.role = "assistant";
    history.push_back(empty_reply);
    TINYCA_CHECK_EQ(render_restored_conversation(history, style), std::string(""));
}

void run_all_tests()
{
    TINYCA_RUN(test_missing_file_is_reported_not_crashed);
    TINYCA_RUN(test_work_dir_is_recovered_and_the_latest_one_wins);
    TINYCA_RUN(test_transcript_without_a_work_dir);
    TINYCA_RUN(test_replays_turns_and_skips_derived_entries);
    TINYCA_RUN(test_stored_system_prompt_is_recovered_from_the_first_request);
    TINYCA_RUN(test_transcript_without_a_system_prompt);
    TINYCA_RUN(test_describe_prompt_difference);
    TINYCA_RUN(test_option_changes_are_recovered_and_the_latest_one_wins);
    TINYCA_RUN(test_untouched_and_malformed_options_leave_the_default_alone);
    TINYCA_RUN(test_restored_conversation_shows_the_turns_in_their_own_colours);
    TINYCA_RUN(test_restored_conversation_of_a_fresh_session_is_empty);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
