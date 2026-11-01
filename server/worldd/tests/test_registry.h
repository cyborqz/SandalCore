// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/test_registry.h: checks that register themselves
//
//  A feature's checks live in a file of their own and register their test functions with AOC_REGISTER_TEST. The test program calls
//  aoc_tests::run(fixture) ONCE in main() and that runs every registered test. Adding a feature then changes no line of main() and no line
//  of another feature's file.
//
//      // my_feature_tests.inc   (included inside the test program's anonymous namespace, after the fixture and the CHECK macros)
//      void test_my_feature(Fixture& f) { std::printf("my feature\n"); CHECK(f.ok(), "..."); }
//      AOC_REGISTER_TEST(test_my_feature);
//
//      // main(), once for the whole program:
//      g_fail += aoc_tests::run(f);
//
//  - A test function takes the program's fixture by reference, or nothing. run(f) hands f to the first kind (its type has to be the type of f)
//    and calls the second as it is.
//  - The tests run in the order of the registrations, which is the order of the source text: a test program is one translation unit, so the
//    order is the same on every machine and every build. What a test prints is its own; the runner prints nothing unless something is wrong.
//  - Every check still counts through the program's own CHECK macros; the runner does not count checks.
//  - A program that registers tests and never calls run() prints a "[FAIL]" line when it ends (the exit code stays as the program leaves it).
//  - A test that throws is reported as a "[FAIL]" line and the next test runs. So are a test made for another fixture type and a name that is
//    registered twice. run() returns how many of these it found: add it to the program's failure count (as above), so that the
//    "N passed, M failed" line and the exit code stay true.
//  - Nothing here needs a build-file change: the checks are plain .inc files included by the program's .cpp (see tests/README.md).
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <type_traits>
#include <vector>

namespace aoc_tests {

namespace detail {

/// An address that is the same for the same type and differs between types (no RTTI needed). The object is not const: a linker may fold identical read-only data of
/// different types into one address (MSVC /OPT:ICF), but never a writable object.
template <class T>
const void* type_tag() {
    static char tag = 0;
    return &tag;
}

struct Entry {
    const char*                name;
    const char*                file;
    int                        line;
    const void*                context;   // type_tag of the fixture the test takes; null when it takes none
    std::function<void(void*)> call;
};

/// The last part of a path (a failure line names the file, never its folder).
inline const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

/// The list lives in a function-local static, built on first use: the registrations run before main(), in no fixed order between files. When the program ends it says so
/// if tests were registered and run() was never called: such a program would run none of them and still report no failure.
struct Registry {
    std::vector<Entry> list;
    bool               ran = false;   // run() was called
    ~Registry() {
        if (!ran && !list.empty()) {
            std::printf("  [FAIL] test registry: %zu test(s) are registered (the first is %s, %s:%d) but this program never calls aoc_tests::run(): none of them ran\n", list.size(),
                        list.front().name, base_name(list.front().file), list.front().line);
        }
    }
};

inline Registry& registry() {
    static Registry r;
    return r;
}

inline std::vector<Entry>& entries() { return registry().list; }

}  // namespace detail

/// One registration. AOC_REGISTER_TEST makes one at namespace scope, so it runs before main().
class Registrar {
public:
    template <class Ctx>
    Registrar(const char* name, const char* file, int line, void (*fn)(Ctx&)) {
        detail::entries().push_back({name, file, line, detail::type_tag<std::remove_const_t<Ctx>>(), [fn](void* ctx) { fn(*static_cast<Ctx*>(ctx)); }});
    }
    Registrar(const char* name, const char* file, int line, void (*fn)()) {
        detail::entries().push_back({name, file, line, nullptr, [fn](void*) { fn(); }});
    }
};

/// Runs the tests of `list` in their order, with `ctx` for the tests that take a fixture. Returns the number of problems it found itself (a test that threw, a test made
/// for another fixture type, a name registered twice); each one is printed to `out` as a "[FAIL]" line. run() calls it with the registered tests and stdout; the registry's
/// own checks call it with a list of their own and a file.
template <class Ctx>
int run_list(const std::vector<detail::Entry>& list, Ctx& ctx, std::FILE* out) {
    const void* const want = detail::type_tag<std::remove_const_t<Ctx>>();
    int problems = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const detail::Entry& e = list[i];
        bool twice = false;
        for (std::size_t j = 0; j < i; ++j) twice = twice || std::strcmp(list[j].name, e.name) == 0;
        if (twice) {
            std::fprintf(out, "  [FAIL] test registry: the name %s is registered twice (%s:%d); the second one did not run\n", e.name, detail::base_name(e.file), e.line);
            ++problems;
            continue;
        }
        if (e.context && e.context != want) {
            std::fprintf(out, "  [FAIL] test registry: %s (%s:%d) takes another fixture type than the one this program runs with; it did not run\n", e.name, detail::base_name(e.file), e.line);
            ++problems;
            continue;
        }
        try {
            e.call(e.context ? static_cast<void*>(&ctx) : nullptr);
        } catch (const std::exception& ex) {
            std::fprintf(out, "  [FAIL] %s threw: %s\n", e.name, ex.what());
            ++problems;
        } catch (...) {
            std::fprintf(out, "  [FAIL] %s threw something that is not an exception\n", e.name);
            ++problems;
        }
    }
    return problems;
}

/// Runs every registered test in the order of the registrations, with `ctx` for the tests that take a fixture. Returns the number of problems it
/// found itself (a test that threw, a test made for another fixture type, a name registered twice); each one is printed as a "[FAIL]" line.
template <class Ctx>
int run(Ctx& ctx) {
    detail::registry().ran = true;
    return run_list(detail::entries(), ctx, stdout);
}

/// The same for a program that has no fixture: only the tests that take nothing can run.
inline int run() {
    struct None {};
    None none;
    return run(none);
}

}  // namespace aoc_tests

/// Registers the test function `fn` (at namespace scope, after the function's definition): `void fn(Fixture&)` or `void fn()`. Use it once per function.
#define AOC_REGISTER_TEST(fn) [[maybe_unused]] static const ::aoc_tests::Registrar aoc_registered_##fn(#fn, __FILE__, __LINE__, &fn)
