#pragma once
// ----------------------------------------------------------------------------
// Tiny zero-dependency assertion helper for the host unit tests.
//
// No external test framework is pulled in so CI stays a single `g++` invocation
// per test file (fast, deterministic, sanitizer-friendly).  Each test_*.cpp
// defines its own main() that RUN()s its cases and returns SUMMARY().
// ----------------------------------------------------------------------------
#include <cstdio>
#include <cstring>

namespace ht {
inline int& total()  { static int t = 0; return t; }
inline int& failed() { static int f = 0; return f; }
}

#define CHECK(cond) do {                                                       \
    ht::total()++;                                                             \
    if (!(cond)) { ht::failed()++;                                             \
        std::printf("  [FAIL] %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); }\
} while (0)

#define CHECK_STREQ(a, b) do {                                                 \
    ht::total()++;                                                             \
    if (std::strcmp((a), (b)) != 0) { ht::failed()++;                          \
        std::printf("  [FAIL] %s:%d\n        expected: \"%s\"\n        actual:   \"%s\"\n", \
                    __FILE__, __LINE__, (b), (a)); }                           \
} while (0)

#define CHECK_EQ(a, b) do {                                                    \
    ht::total()++;                                                             \
    if (!((a) == (b))) { ht::failed()++;                                       \
        std::printf("  [FAIL] %s:%d  %s == %s  (%lld != %lld)\n",              \
                    __FILE__, __LINE__, #a, #b,                                \
                    (long long)(a), (long long)(b)); }                         \
} while (0)

#define RUN(fn) do { std::printf("[RUN ] %s\n", #fn); fn(); } while (0)

#define SUMMARY() ( std::printf("\n%d checks run, %d failed\n",                 \
                                ht::total(), ht::failed()),                    \
                    ht::failed() == 0 ? 0 : 1 )
