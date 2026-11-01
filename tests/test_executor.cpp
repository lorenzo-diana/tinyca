// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/executor.h"

#include <chrono>
#include <thread>

#include <unistd.h>

#include "test_util.h"

namespace
{

using tinyca::Executor;
using tinyca::truncate_output;

void test_basic_command_and_exit_code()
{
    Executor ex("/tmp", 10, 40, 40, 4096);
    auto r = ex.run("echo hello; exit 3");
    TINYCA_CHECK_EQ(r.exit_code, 3);
    TINYCA_CHECK(r.output.find("hello") != std::string::npos);
    TINYCA_CHECK(!r.timed_out);
}

void test_work_dir_is_respected()
{
    Executor ex("/tmp", 10, 40, 40, 4096);
    auto r = ex.run("pwd");
    TINYCA_CHECK(r.output.find("/tmp") != std::string::npos);
}

void test_merged_stdout_and_stderr()
{
    Executor ex("/tmp", 10, 40, 40, 4096);
    auto r = ex.run("echo out_line; echo err_line >&2");
    TINYCA_CHECK(r.output.find("out_line") != std::string::npos);
    TINYCA_CHECK(r.output.find("err_line") != std::string::npos);
}

// wc -l's output has trailing whitespace/newline; trim before comparing.
std::string trimmed(const std::string& s)
{
    std::string out = s;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
    {
        out.pop_back();
    }
    return out;
}

void test_timeout_kills_process_group_including_grandchild()
{
    // The direct child backgrounds a long sleep (a grandchild of Executor,
    // same process group) and then itself sleeps past the timeout. A naive
    // kill(child_pid) would leave the backgrounded sleep running.
    Executor ex("/tmp", 1, 40, 40, 4096);
    auto start = std::chrono::steady_clock::now();
    auto r = ex.run("sleep 30 & sleep 30");
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

    TINYCA_CHECK(r.timed_out);
    TINYCA_CHECK(elapsed < 5); // well under the 30s the command asked for

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // The bracket trick (`[s]leep`) keeps pgrep's own argv from matching its
    // own search pattern -- otherwise it always finds "itself" via the
    // literal "sleep 30" text in its own command line. The trailing `$`
    // anchors the end of the match so an unrelated "sleep 300" elsewhere on
    // the machine (observed for real on the Jetson -- a pre-existing system
    // process) doesn't false-positive as a substring match.
    auto check = ex.run("pgrep -f '[s]leep 30$' | wc -l");
    TINYCA_CHECK_EQ(trimmed(check.output), std::string("0"));
}

void test_interrupt_stops_a_running_command_promptly()
{
    Executor ex("/tmp", 60, 40, 40, 4096);
    std::thread interrupter(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ex.interrupt();
        });

    auto start = std::chrono::steady_clock::now();
    auto r = ex.run("sleep 60");
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    interrupter.join();

    TINYCA_CHECK(r.killed_by_control);
    TINYCA_CHECK(!r.timed_out);
    TINYCA_CHECK(elapsed_ms < 5000); // nowhere near the 60s timeout
}

void test_truncate_output_short_text_untouched()
{
    std::string in = "line1\nline2\nline3\n";
    std::string out = truncate_output(in, 40, 40, 4096);
    TINYCA_CHECK_EQ(out, in);
}

void test_truncate_output_omits_middle_lines()
{
    std::string in;
    for (int i = 0; i < 100; ++i)
    {
        in += "line" + std::to_string(i) + "\n";
    }
    std::string out = truncate_output(in, 5, 5, 100000);
    TINYCA_CHECK(out.find("line0\n") != std::string::npos);
    TINYCA_CHECK(out.find("line99\n") != std::string::npos);
    TINYCA_CHECK(out.find("90 lines omitted") != std::string::npos);
    TINYCA_CHECK(out.find("line50") == std::string::npos); // a middle line must be gone
}

void test_truncate_output_hard_caps_bytes()
{
    std::string in(10000, 'x');
    std::string out = truncate_output(in, 40, 40, 100);
    TINYCA_CHECK(out.size() < in.size());
    TINYCA_CHECK(out.find("truncated at output_max_bytes") != std::string::npos);
    // SPEC.md §8 calls this a hard cap, so the marker must fit inside it --
    // guards a defect where appending the marker after resize(max_bytes)
    // returned 4137 bytes for max_bytes = 4096.
    TINYCA_CHECK(out.size() <= 100);

    std::string capped_4096 = truncate_output(in, 40, 40, 4096);
    TINYCA_CHECK(capped_4096.size() <= 4096);

    // Degenerate case: a cap shorter than the marker still reports that
    // something was cut, and still respects the cap.
    std::string tiny = truncate_output(in, 40, 40, 8);
    TINYCA_CHECK(tiny.size() <= 8);
    TINYCA_CHECK(!tiny.empty());
}

void test_five_megabyte_output_is_truncated_not_hung()
{
    // Required fixture scenario (dev/PROMPT.md): a single pip-install-sized log
    // must not destroy the context window or the Executor itself.
    Executor ex("/tmp", 10, 40, 40, 4096);
    auto r = ex.run("head -c 5000000 /dev/zero | tr '\\0' 'a'");
    TINYCA_CHECK(!r.timed_out);
    TINYCA_CHECK(r.output.size() < static_cast<size_t>(10000)); // well under 5MB after truncation
}

// 13(a): a skill must be runnable by bare filename from inside work_dir,
// which is a different directory than the one skills live in. Guards the
// defect where the advertised usage "skills/fetch_url.sh ..." returned exit
// 127 ("No such file or directory") for every skill invocation.
void test_path_prepend_makes_a_skill_runnable_by_bare_name()
{
    char cwd[4096];
    TINYCA_CHECK(getcwd(cwd, sizeof(cwd)) != nullptr);
    const std::string skills_abs = std::string(cwd) + "/skills";

    // work_dir deliberately is NOT the repo root, mirroring the real default.
    tinyca::Executor without_path("/tmp", 10, 40, 40, 4096);
    tinyca::ExecResult missing = without_path.run("fetch_url.sh");
    TINYCA_CHECK_EQ(missing.exit_code, 127);

    tinyca::Executor with_path("/tmp", 10, 40, 40, 4096);
    with_path.set_path_prepend(skills_abs);
    tinyca::ExecResult found = with_path.run("fetch_url.sh");
    // Found and executed: the script rejects a wrong argument count with 2,
    // which is proof it ran rather than failing to resolve.
    TINYCA_CHECK_EQ(found.exit_code, 2);
    TINYCA_CHECK(found.output.find("usage:") != std::string::npos);
}

void run_all_tests()
{
    TINYCA_RUN(test_basic_command_and_exit_code);
    TINYCA_RUN(test_work_dir_is_respected);
    TINYCA_RUN(test_merged_stdout_and_stderr);
    TINYCA_RUN(test_timeout_kills_process_group_including_grandchild);
    TINYCA_RUN(test_interrupt_stops_a_running_command_promptly);
    TINYCA_RUN(test_truncate_output_short_text_untouched);
    TINYCA_RUN(test_truncate_output_omits_middle_lines);
    TINYCA_RUN(test_truncate_output_hard_caps_bytes);
    TINYCA_RUN(test_five_megabyte_output_is_truncated_not_hung);
    TINYCA_RUN(test_path_prepend_makes_a_skill_runnable_by_bare_name);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
