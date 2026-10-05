#pragma once
// Tiny dependency-free test harness for Brazen's core (no GUI, no OpenGL).
//
//   BrazenSelfTest                run every test
//   BrazenSelfTest --list         print test names
//   BrazenSelfTest <name>...      run only the named tests
//
// Exit status is non-zero if any check failed. Running a single named test
// in its own process is handy when a test is expected to crash on broken
// code (that is how the regression tests prove they catch a bug).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace selftest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline bool& SkipFlag() {
    static bool skipped = false;
    return skipped;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline int Main(int argc, char** argv) {
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            for (auto& c : Registry()) std::printf("%s\n", c.name);
            return 0;
        }
        wanted.push_back(argv[i]);
    }
    int ran = 0, failedTests = 0, skipped = 0;
    for (auto& c : Registry()) {
        if (!wanted.empty()) {
            bool match = false;
            for (auto& w : wanted) match = match || w == c.name;
            if (!match) continue;
        }
        int before = FailureCount();
        SkipFlag() = false;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        ++ran;
        if (SkipFlag() && FailureCount() == before) {
            ++skipped;
            std::printf("[ SKIP ] %s\n", c.name);
        } else if (FailureCount() == before) {
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            std::printf("[ FAIL ] %s\n", c.name);
            ++failedTests;
        }
        std::fflush(stdout);
    }
    if (!wanted.empty() && ran == 0) {
        std::printf("No test matched the given name(s).\n");
        return 2;
    }
    std::printf("\n%d test(s) run, %d failed, %d skipped.\n", ran, failedTests, skipped);
    return failedTests == 0 ? 0 : 1;
}

} // namespace selftest

#define SELFTEST_CAT2(a, b) a##b
#define SELFTEST_CAT(a, b) SELFTEST_CAT2(a, b)

#define TEST_CASE(name)                                                                  \
    static void name();                                                                  \
    static ::selftest::Registrar SELFTEST_CAT(registrar_, name)(#name, name);            \
    static void name()

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__);   \
            ++::selftest::FailureCount();                                                \
        }                                                                                \
    } while (0)

// Like CHECK but stops the current test (use when continuing would crash).
#define REQUIRE(cond)                                                                    \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    REQUIRE failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++::selftest::FailureCount();                                                \
            return;                                                                      \
        }                                                                                \
    } while (0)

// For environmental preconditions (free disk space, ...): the test is
// reported as skipped, not failed, when the machine can't support it.
#define SKIP_UNLESS(cond, reason)                                                        \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("    skipped: %s\n", reason);                                    \
            ::selftest::SkipFlag() = true;                                               \
            return;                                                                      \
        }                                                                                \
    } while (0)
