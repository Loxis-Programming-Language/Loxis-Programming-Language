// Tiny test harness shared by AARC tests.  Kept deliberately dependency-free.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace aarc_test {

inline int& failures() {
    static int f = 0;
    return f;
}
inline int& checks() {
    static int c = 0;
    return c;
}

// Records a failure and prints context.
inline void report_failure(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("  FAIL %s:%d  %s\n", file, line, msg.c_str());
}

inline void report_success(const char* name) {
    std::printf("  ok    %s\n", name);
}

// Exit handler: nonzero exit code when anything failed.
struct Runner {
    ~Runner() {
        if (failures() == 0)
            std::printf("ALL PASS (%d checks)\n", checks());
        else
            std::printf("%d/%d checks FAILED\n", failures(), checks());
        std::exit(failures() == 0 ? 0 : 1);
    }
};

} // namespace aarc_test

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++aarc_test::checks();                                                 \
        if (!(cond)) {                                                         \
            aarc_test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, msg)                                                   \
    do {                                                                       \
        ++aarc_test::checks();                                                 \
        if (!(cond)) {                                                         \
            aarc_test::report_failure(__FILE__, __LINE__,                      \
                                      std::string("CHECK(" #cond "): ") + (msg)); \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++aarc_test::checks();                                                 \
        auto va = (a);                                                         \
        auto vb = (b);                                                         \
        if (!(va == vb)) {                                                     \
            aarc_test::report_failure(__FILE__, __LINE__,                      \
                                      "CHECK_EQ(" #a ", " #b ")");             \
        }                                                                      \
    } while (0)

// Anonymous-namespace Runner so each test TU installs its own exit handler.
namespace { aarc_test::Runner aarc_runner_; }
