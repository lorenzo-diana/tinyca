// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/control.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "test_util.h"
#include "tinyca/executor.h"

namespace
{

using tinyca::ControlThread;
using tinyca::Executor;

void test_sigint_interrupts_running_executor()
{
    Executor executor("/tmp", 60, 40, 40, 4096);
    ControlThread control(nullptr, &executor);
    control.start();

    std::thread raiser(
        [&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            kill(getpid(), SIGINT); // simulates Ctrl+C
        });

    auto start = std::chrono::steady_clock::now();
    auto result = executor.run("sleep 60");
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    raiser.join();

    TINYCA_CHECK(result.killed_by_control);
    TINYCA_CHECK(elapsed_ms < 5000); // nowhere near the 60s command/timeout
    TINYCA_CHECK(control.stop_requested());

    control.stop();
}

void test_reset_clears_stop_requested_for_next_task()
{
    Executor executor("/tmp", 60, 40, 40, 4096);
    ControlThread control(nullptr, &executor);
    control.start();

    kill(getpid(), SIGINT);
    // Give the control thread a moment to actually process the signal.
    for (int i = 0; i < 50 && !control.stop_requested(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    TINYCA_CHECK(control.stop_requested());

    control.reset();
    TINYCA_CHECK(!control.stop_requested());

    control.stop();
}

void test_stop_joins_cleanly_without_a_prior_signal()
{
    Executor executor("/tmp", 60, 40, 40, 4096);
    ControlThread control(nullptr, &executor);
    control.start();
    control.stop(); // must not hang -- this is the real assertion
    TINYCA_CHECK(!control.stop_requested());
}

void run_all_tests()
{
    TINYCA_RUN(test_sigint_interrupts_running_executor);
    TINYCA_RUN(test_reset_clears_stop_requested_for_next_task);
    TINYCA_RUN(test_stop_joins_cleanly_without_a_prior_signal);
}

} // namespace

TINYCA_TEST_MAIN(run_all_tests)
