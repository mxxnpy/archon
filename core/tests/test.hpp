#pragma once
// Minimal dependency-free test harness.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace t

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static t::Register TEST_CAT(reg_, name)(#name, name);                       \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++t::failures();                                                    \
        }                                                                       \
    } while (0)

#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL %s:%d: %s (fatal)\n", __FILE__, __LINE__, #cond); \
            ++t::failures();                                                    \
            return;                                                             \
        }                                                                       \
    } while (0)
