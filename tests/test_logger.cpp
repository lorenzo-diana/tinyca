// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/logger.h"

#include <cstdio>
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
    Logger logger(path, 0, LogLevel::Info);
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
    // A tiny cap so a handful of entries exceeds it quickly.
    Logger logger(path, 80, LogLevel::Info);
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
    Logger logger(path, 0, LogLevel::Info);
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

void run_all_tests()
{
    TINYCA_RUN(test_transcript_writes_jsonl_lines);
    TINYCA_RUN(test_transcript_max_bytes_cutoff);
    TINYCA_RUN(test_transcript_unlimited_when_max_bytes_zero);
    TINYCA_RUN(test_level_from_string_and_name);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
