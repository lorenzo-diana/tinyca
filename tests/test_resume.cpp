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

void run_all_tests()
{
    TINYCA_RUN(test_missing_file_is_reported_not_crashed);
    TINYCA_RUN(test_work_dir_is_recovered_and_the_latest_one_wins);
    TINYCA_RUN(test_transcript_without_a_work_dir);
    TINYCA_RUN(test_replays_turns_and_skips_derived_entries);
    TINYCA_RUN(test_stored_system_prompt_is_recovered_from_the_first_request);
    TINYCA_RUN(test_transcript_without_a_system_prompt);
    TINYCA_RUN(test_describe_prompt_difference);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
