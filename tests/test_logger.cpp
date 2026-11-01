// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/logger.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "test_util.h"

namespace
{

using tinyca::LogLevel;
using tinyca::Logger;

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void test_transcript_writes_jsonl_lines()
{
    const std::string path = "build/tests/transcript_basic.jsonl";
    std::remove(path.c_str());
    Logger logger(path, 0, 0, LogLevel::Info);
    logger.transcript("request", R"({"model":"m"})");
    logger.transcript("response", R"({"content":"hi"})");

    std::string content = read_file(path);
    TINYCA_CHECK(content.find("\"kind\":\"request\"") != std::string::npos);
    TINYCA_CHECK(content.find("\"kind\":\"response\"") != std::string::npos);
    TINYCA_CHECK(content.find(R"("payload":{"model":"m"})") != std::string::npos);

    int line_count = 0;
    for (char c : content)
    {
        if (c == '\n')
        {
            ++line_count;
        }
    }
    TINYCA_CHECK_EQ(line_count, 2);
}

void test_transcript_max_bytes_cutoff()
{
    const std::string path = "build/tests/transcript_capped.jsonl";
    std::remove(path.c_str());
    // A small cap so a handful of entries exceeds it quickly, but still big
    // enough to hold the cutoff marker (131 bytes) -- see
    // test_transcript_cap_too_small_for_the_marker for the other case.
    Logger logger(path, 400, 0, LogLevel::Info);
    for (int i = 0; i < 20; ++i)
    {
        logger.transcript("x", R"({"i":)" + std::to_string(i) + "}");
    }

    std::string content = read_file(path);
    TINYCA_CHECK(content.find("transcript_cutoff") != std::string::npos);
    // Not all 20 entries made it through -- the cap actually did something.
    size_t x_count = 0;
    size_t pos = 0;
    while ((pos = content.find("\"kind\":\"x\"", pos)) != std::string::npos)
    {
        ++x_count;
        pos += 1;
    }
    TINYCA_CHECK(x_count < static_cast<size_t>(20));
}

void test_transcript_unlimited_when_max_bytes_zero()
{
    const std::string path = "build/tests/transcript_unlimited.jsonl";
    std::remove(path.c_str());
    Logger logger(path, 0, 0, LogLevel::Info);
    for (int i = 0; i < 50; ++i)
    {
        logger.transcript("x", R"({"i":)" + std::to_string(i) + "}");
    }
    std::string content = read_file(path);
    TINYCA_CHECK(content.find("transcript_cutoff") == std::string::npos);
    size_t x_count = 0;
    size_t pos = 0;
    while ((pos = content.find("\"kind\":\"x\"", pos)) != std::string::npos)
    {
        ++x_count;
        pos += 1;
    }
    TINYCA_CHECK_EQ(x_count, static_cast<size_t>(50));
}

void test_level_from_string_and_name()
{
    TINYCA_CHECK(Logger::level_from_string("error") == LogLevel::Error);
    TINYCA_CHECK(Logger::level_from_string("warn") == LogLevel::Warn);
    TINYCA_CHECK(Logger::level_from_string("debug") == LogLevel::Debug);
    TINYCA_CHECK(Logger::level_from_string("info") == LogLevel::Info);
    TINYCA_CHECK(Logger::level_from_string("garbage") == LogLevel::Info); // safe default
    TINYCA_CHECK_EQ(std::string(Logger::level_name(LogLevel::Warn)), std::string("warn"));
}

// Guards SPEC.md §9.2's "cap on the file": the counter starts at the size the
// transcript already had, so a second run against an over-cap file appends
// nothing but the cutoff line. (Before this, each run restarted the count at
// zero and the file grew without bound across runs.)
void test_transcript_max_bytes_counts_pre_existing_file()
{
    const std::string path = "build/tests/transcript_inherited.jsonl";
    std::remove(path.c_str());
    {
        Logger first(path, 400, 0, LogLevel::Info);
        for (int i = 0; i < 20; ++i)
            first.transcript("x", R"({"i":)" + std::to_string(i) + "}");
    }
    const size_t after_first = read_file(path).size();
    TINYCA_CHECK(after_first <= 400);

    {
        Logger second(path, 400, 0, LogLevel::Info);
        for (int i = 0; i < 20; ++i)
            second.transcript("y", R"({"i":)" + std::to_string(i) + "}");
    }
    const std::string content = read_file(path);
    // The second run may only add its single cutoff line, not 20 entries.
    TINYCA_CHECK(content.find("\"kind\":\"y\"") == std::string::npos);
    TINYCA_CHECK(content.find("transcript_max_bytes exceeded") != std::string::npos);
}

// A cap below one entry plus the cutoff marker cannot be honoured AND
// explained: TinyCA keeps the cap (never writes past it) and the transcript
// stays empty. load_config warns about such a value at startup, which is
// where a user can actually act on it.
void test_transcript_cap_too_small_for_the_marker()
{
    const std::string path = "build/tests/transcript_tiny_cap.jsonl";
    std::remove(path.c_str());
    {
        Logger logger(path, 80, 0, LogLevel::Info);
        for (int i = 0; i < 5; ++i)
            logger.transcript("x", R"({"i":)" + std::to_string(i) + "}");
    }
    TINYCA_CHECK(read_file(path).size() <= 80);
}

// The per-run cap bounds one run's growth with no total-file cap set, and the
// cutoff line names which cap tripped so a reader of the transcript can tell
// the two apart.
void test_transcript_max_bytes_per_run_cap()
{
    const std::string path = "build/tests/transcript_per_run.jsonl";
    std::remove(path.c_str());
    {
        Logger first(path, 0, 200, LogLevel::Info);
        for (int i = 0; i < 20; ++i)
            first.transcript("a", R"({"i":)" + std::to_string(i) + "}");
    }
    const std::string first_content = read_file(path);
    TINYCA_CHECK(first_content.find("transcript_max_bytes_per_run exceeded") != std::string::npos);
    const size_t after_first = first_content.size();
    TINYCA_CHECK(after_first <= 300); // 200 of entries plus the one cutoff line

    // A fresh run gets a fresh per-run budget: the file keeps growing, which
    // is exactly the difference from the total-file cap above.
    {
        Logger second(path, 0, 200, LogLevel::Info);
        second.transcript("b", R"({"i":0})");
    }
    const std::string second_content = read_file(path);
    TINYCA_CHECK(second_content.size() > after_first);
    TINYCA_CHECK(second_content.find("\"kind\":\"b\"") != std::string::npos);
}

// A session renamed mid-run keeps logging, into the new file (SPEC.md §12.3).
void test_set_path_follows_a_renamed_file()
{
    const std::string first = "build/tests/logger_rename_a.jsonl";
    const std::string second = "build/tests/logger_rename_b.jsonl";
    std::remove(first.c_str());
    std::remove(second.c_str());

    Logger logger(first, 0, 0, LogLevel::Info);
    logger.transcript("before", R"({"n":1})");

    // Mirrors what /rename does: move the file, then point the logger at it.
    TINYCA_CHECK_EQ(std::rename(first.c_str(), second.c_str()), 0);
    logger.set_path(second);
    logger.transcript("after", R"({"n":2})");

    const std::string content = read_file(second);
    TINYCA_CHECK(content.find("\"kind\":\"before\"") != std::string::npos);
    TINYCA_CHECK(content.find("\"kind\":\"after\"") != std::string::npos);
    // Nothing was written back to the old name.
    TINYCA_CHECK(read_file(first).empty());
}

// A transcript that cannot be written used to be lost in silence: TinyCA ran
// the task, reported nothing, and exited 0. On the target board the transcript
// is the only post-mortem available, so the failure is now flagged (and warned
// about once on stderr). Guards both directions.
void test_unwritable_transcript_is_reported()
{
    const std::string dir = "build/tests/logger_readonly";
    TINYCA_CHECK_EQ(system(("rm -rf " + dir + " && mkdir -p " + dir).c_str()), 0);
    TINYCA_CHECK_EQ(system(("chmod 500 " + dir).c_str()), 0);

    Logger blocked(dir + "/transcript.jsonl", 0, 0, LogLevel::Info);
    TINYCA_CHECK(!blocked.write_failed()); // nothing attempted yet
    blocked.transcript("request", R"({"n":1})");
    TINYCA_CHECK(blocked.write_failed());

    TINYCA_CHECK_EQ(system(("chmod 700 " + dir).c_str()), 0);

    // The same logger pointed at a writable path reports no failure, so the
    // flag tracks the write rather than being stuck on.
    const std::string ok_path = "build/tests/logger_writable.jsonl";
    std::remove(ok_path.c_str());
    Logger healthy(ok_path, 0, 0, LogLevel::Info);
    healthy.transcript("request", R"({"n":1})");
    TINYCA_CHECK(!healthy.write_failed());
    TINYCA_CHECK(read_file(ok_path).find("\"kind\":\"request\"") != std::string::npos);
}

void run_all_tests()
{
    TINYCA_RUN(test_transcript_writes_jsonl_lines);
    TINYCA_RUN(test_transcript_max_bytes_cutoff);
    TINYCA_RUN(test_transcript_max_bytes_counts_pre_existing_file);
    TINYCA_RUN(test_transcript_cap_too_small_for_the_marker);
    TINYCA_RUN(test_transcript_max_bytes_per_run_cap);
    TINYCA_RUN(test_transcript_unlimited_when_max_bytes_zero);
    TINYCA_RUN(test_set_path_follows_a_renamed_file);
    TINYCA_RUN(test_unwritable_transcript_is_reported);
    TINYCA_RUN(test_level_from_string_and_name);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
