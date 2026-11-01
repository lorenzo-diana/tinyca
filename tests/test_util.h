// BSD 3-Clause License — see LICENSE at the repository root.
// Minimal assert macro set -- no test framework dependency (dev/PROMPT.md W2).
#pragma once

#include <iostream>
#include <string>

namespace tinyca_test
{
inline int& failure_count()
{
    static int n = 0;
    return n;
}
} // namespace tinyca_test

#define TINYCA_CHECK(cond)                                                                       \
    do                                                                                            \
    {                                                                                             \
        if (!(cond))                                                                              \
        {                                                                                         \
            std::cerr << "  CHECK failed: " << #cond << " at " << __FILE__ << ":" << __LINE__     \
                       << std::endl;                                                              \
            ++tinyca_test::failure_count();                                                       \
        }                                                                                         \
    } while (0)

#define TINYCA_CHECK_EQ(a, b)                                                                      \
    do                                                                                             \
    {                                                                                              \
        if (!((a) == (b)))                                                                         \
        {                                                                                           \
            std::cerr << "  CHECK_EQ failed: " << #a << " == " << #b << " at " << __FILE__ << ":"  \
                       << __LINE__ << "  (got '" << (a) << "', expected '" << (b) << "')"           \
                       << std::endl;                                                                \
            ++tinyca_test::failure_count();                                                         \
        }                                                                                           \
    } while (0)

#define TINYCA_RUN(name)                                                                          \
    do                                                                                             \
    {                                                                                              \
        std::cout << "-- " << #name << std::endl;                                                 \
        name();                                                                                    \
    } while (0)

#define TINYCA_TEST_MAIN(run_all)                                                                  \
    int main()                                                                                     \
    {                                                                                               \
        run_all();                                                                                  \
        if (tinyca_test::failure_count() == 0)                                                      \
        {                                                                                           \
            std::cout << "ALL PASS" << std::endl;                                                   \
            return 0;                                                                               \
        }                                                                                           \
        std::cout << tinyca_test::failure_count() << " check(s) failed" << std::endl;               \
        return 1;                                                                                   \
    }
