// BSD 3-Clause License — see LICENSE at the repository root.
// Named sessions (SPEC.md §12.3): names, default naming, uniqueness, listing.
#include "tinyca/session.h"

#include <sys/stat.h>
#include <time.h>

#include <cstdlib>
#include <fstream>

#include "test_util.h"

namespace
{

using namespace tinyca;

const char* kDir = "build/tests/sessions";

void fresh_dir()
{
    TINYCA_CHECK_EQ(system((std::string("rm -rf ") + kDir + " && mkdir -p " + kDir).c_str()), 0);
}

void write_session(const std::string& name, int tasks)
{
    std::ofstream f(session_path(kDir, name), std::ios::trunc);
    f << R"({"ts":1,"kind":"session_start","payload":{"work_dir":"/tmp"}})" << "\n";
    for (int i = 0; i < tasks; ++i)
    {
        f << R"({"ts":2,"kind":"user_task","payload":{"text":"t"}})" << "\n";
        f << R"({"ts":3,"kind":"response","payload":{"content":"ok"}})" << "\n";
    }
}

// Names double as filenames, so anything that could traverse a path or need
// quoting is refused -- the same stance add_skill takes on skill names.
void test_name_validation()
{
    TINYCA_CHECK(valid_session_name("2026-11-01_17-36-04_workspace"));
    TINYCA_CHECK(valid_session_name("2026-11-01_workspace"));
    TINYCA_CHECK(valid_session_name("sort-bug.2"));
    TINYCA_CHECK(valid_session_name("a"));

    TINYCA_CHECK(!valid_session_name(""));
    TINYCA_CHECK(!valid_session_name("."));
    TINYCA_CHECK(!valid_session_name(".."));
    TINYCA_CHECK(!valid_session_name(".hidden"));
    TINYCA_CHECK(!valid_session_name("../escape"));
    TINYCA_CHECK(!valid_session_name("with/slash"));
    TINYCA_CHECK(!valid_session_name("with space"));
    TINYCA_CHECK(!valid_session_name("quote\"d"));
    TINYCA_CHECK(!valid_session_name("semi;colon"));
    // No ':' either: a name has to survive being copied onto a FAT volume.
    TINYCA_CHECK(!valid_session_name("2026-11-01_17:36:04_workspace"));
}

void test_session_path_has_exactly_one_separator()
{
    TINYCA_CHECK_EQ(session_path("logs/sessions", "a"), std::string("logs/sessions/a.jsonl"));
    TINYCA_CHECK_EQ(session_path("logs/sessions/", "a"), std::string("logs/sessions/a.jsonl"));
}

// The template: YYYY-MM-DD_hh-mm-ss_<work dir basename>.
void test_default_session_name_template()
{
    // 2026-11-01 12:00:00 UTC. The name uses LOCAL time, so assert the shape
    // and the basename rather than a fixed timestamp -- a test pinned to a
    // clock reading is a test that fails in another timezone.
    const long when = 1793534400L;
    const size_t stamp_len = std::string("YYYY-MM-DD_hh-mm-ss").size(); // 19
    const std::string name = default_session_name("/home/me/projects/TinyCA/workspace", when);

    TINYCA_CHECK_EQ(name.size(), stamp_len + std::string("_workspace").size());
    TINYCA_CHECK_EQ(name.substr(stamp_len), std::string("_workspace"));
    TINYCA_CHECK_EQ(name[4], '-');
    TINYCA_CHECK_EQ(name[7], '-');
    TINYCA_CHECK_EQ(name[10], '_');
    TINYCA_CHECK_EQ(name[13], '-');
    TINYCA_CHECK_EQ(name[16], '-');
    // Every character of the stamp other than the separators is a digit.
    for (size_t i = 0; i < stamp_len; ++i)
    {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16)
        {
            continue;
        }
        TINYCA_CHECK(name[i] >= '0' && name[i] <= '9');
    }
    // A name has to survive being used as a filename.
    TINYCA_CHECK(valid_session_name(name));

    // A trailing slash is not a basename.
    TINYCA_CHECK_EQ(default_session_name("/a/b/work/", when).substr(stamp_len), std::string("_work"));
    // Characters that cannot appear in a name are replaced, not dropped.
    TINYCA_CHECK_EQ(default_session_name("/a/my work!", when).substr(stamp_len), std::string("_my_work_"));
    // Degenerate work dirs still produce a usable name.
    TINYCA_CHECK_EQ(default_session_name("/", when).substr(stamp_len), std::string("_workdir"));
    TINYCA_CHECK_EQ(default_session_name(".", when).substr(stamp_len), std::string("_workdir"));
    TINYCA_CHECK(valid_session_name(default_session_name("/", when)));

    // Two sessions a second apart get different names by construction -- the
    // "-2" suffix is now a fallback for the same-second case only.
    TINYCA_CHECK(default_session_name("/a/work", when) != default_session_name("/a/work", when + 1));
}

// Two sessions started the same day in the same directory must not share a
// transcript: the second one gets a suffix.
void test_unique_session_name_disambiguates()
{
    fresh_dir();
    const std::string base = "2026-11-01_17-36-04_workspace";

    TINYCA_CHECK_EQ(unique_session_name(kDir, base), base);
    write_session(base, 1);
    TINYCA_CHECK_EQ(unique_session_name(kDir, base), base + "-2");
    write_session(base + "-2", 1);
    TINYCA_CHECK_EQ(unique_session_name(kDir, base), base + "-3");
}

void test_session_exists_and_listing()
{
    fresh_dir();
    TINYCA_CHECK(list_sessions(kDir).empty());
    TINYCA_CHECK(!session_exists(kDir, "nope"));

    write_session("older", 1);
    // Distinct mtimes, so "newest first" is actually testable.
    TINYCA_CHECK_EQ(system((std::string("touch -d '2020-01-01 10:00' ") + kDir + "/older.jsonl").c_str()), 0);
    write_session("newer", 3);

    TINYCA_CHECK(session_exists(kDir, "older"));
    std::vector<SessionInfo> sessions = list_sessions(kDir);
    TINYCA_CHECK_EQ(sessions.size(), static_cast<size_t>(2));
    TINYCA_CHECK_EQ(sessions[0].name, std::string("newer"));
    TINYCA_CHECK_EQ(sessions[1].name, std::string("older"));
    TINYCA_CHECK_EQ(sessions[0].tasks, 3);
    TINYCA_CHECK_EQ(sessions[0].entries, 7); // session_start + 3 * (task + response)
    TINYCA_CHECK(sessions[0].bytes > 0);

    // Non-transcript files and dotfiles are not sessions.
    TINYCA_CHECK_EQ(system((std::string("touch ") + kDir + "/notes.txt " + kDir + "/.hidden.jsonl").c_str()), 0);
    TINYCA_CHECK_EQ(list_sessions(kDir).size(), static_cast<size_t>(2));

    // A missing directory has no sessions; it is not an error.
    TINYCA_CHECK(list_sessions("build/tests/no_such_sessions_dir").empty());
}

// Names are unique across stored sessions, so rename refuses a taken name --
// the one mistake this operation can make is clobbering another session.
void test_rename_session()
{
    fresh_dir();
    write_session("alpha", 2);
    write_session("beta", 1);
    std::string err;

    TINYCA_CHECK(rename_session(kDir, "alpha", "gamma", err));
    TINYCA_CHECK(!session_exists(kDir, "alpha"));
    TINYCA_CHECK(session_exists(kDir, "gamma"));
    // Content moved with the name, not just the name.
    std::vector<SessionInfo> sessions = list_sessions(kDir);
    for (const auto& info : sessions)
    {
        if (info.name == "gamma")
        {
            TINYCA_CHECK_EQ(info.tasks, 2);
        }
    }

    TINYCA_CHECK(!rename_session(kDir, "gamma", "beta", err));
    TINYCA_CHECK(err.find("already exists") != std::string::npos);
    TINYCA_CHECK(session_exists(kDir, "gamma")); // untouched by the refusal

    TINYCA_CHECK(!rename_session(kDir, "gamma", "../escape", err));
    TINYCA_CHECK(err.find("not a usable session name") != std::string::npos);

    TINYCA_CHECK(!rename_session(kDir, "gamma", "gamma", err));
    TINYCA_CHECK(err.find("already called") != std::string::npos);

    TINYCA_CHECK(!rename_session(kDir, "no_such_session", "whatever", err));
    TINYCA_CHECK(err.find("no stored session") != std::string::npos);
}

// Finding 4: the task count must not depend on the exact bytes Logger happens
// to write. A transcript serialised with a space after the colon used to be
// reported as "0 task(s)" -- a wrong number that reads like a real one.
void test_task_count_tolerates_json_spacing()
{
    fresh_dir();
    {
        std::ofstream f(session_path(kDir, "spaced"), std::ios::trunc);
        f << R"({"ts": 1, "kind": "user_task", "payload": {"text": "a"}})" << "\n";
        f << R"({"ts":2,"kind":"user_task","payload":{"text":"b"}})" << "\n";
        f << R"({"ts": 3, "kind" : "response", "payload": {"content": "ok"}})" << "\n";
        // A field whose VALUE mentions the kind must not be counted.
        f << R"({"ts":4,"kind":"response","payload":{"content":"the \"kind\":\"user_task\" marker"}})" << "\n";
    }

    std::vector<SessionInfo> sessions = list_sessions(kDir);
    TINYCA_CHECK_EQ(sessions.size(), static_cast<size_t>(1));
    TINYCA_CHECK_EQ(sessions[0].entries, 4);
    TINYCA_CHECK_EQ(sessions[0].tasks, 2);
}

// Finding 7: the name is claimed atomically, so the returned name is free AND
// now exists -- two processes racing cannot both be handed the same one.
void test_unique_session_name_claims_the_file()
{
    fresh_dir();
    const std::string base = "2026-11-01_17-36-04_workspace";

    const std::string first = unique_session_name(kDir, base);
    TINYCA_CHECK_EQ(first, base);
    TINYCA_CHECK(session_exists(kDir, first)); // claimed, not just checked

    const std::string second = unique_session_name(kDir, base);
    TINYCA_CHECK_EQ(second, base + "-2");
    TINYCA_CHECK(session_exists(kDir, second));

    const std::string third = unique_session_name(kDir, base);
    TINYCA_CHECK_EQ(third, base + "-3");

    // Claiming leaves the file empty, so a resumed run of it has no history.
    TINYCA_CHECK(list_sessions(kDir).size() == 3);
    for (const auto& info : list_sessions(kDir))
    {
        TINYCA_CHECK_EQ(info.entries, 0);
    }
}

void run_all_tests()
{
    TINYCA_RUN(test_name_validation);
    TINYCA_RUN(test_session_path_has_exactly_one_separator);
    TINYCA_RUN(test_default_session_name_template);
    TINYCA_RUN(test_unique_session_name_disambiguates);
    TINYCA_RUN(test_session_exists_and_listing);
    TINYCA_RUN(test_rename_session);
    TINYCA_RUN(test_task_count_tolerates_json_spacing);
    TINYCA_RUN(test_unique_session_name_claims_the_file);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
