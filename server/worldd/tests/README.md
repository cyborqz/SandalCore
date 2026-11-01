# worldd tests

Every `test_worldd_*.cpp` is one program (the list is in `../CMakeLists.txt`). A check is `CHECK(condition, "what it proves")`, or `CHECKF(condition, "format", args...)`. A program prints a `  [FAIL] ...` line for every failed check, ends with the line `N passed, M failed`, and exits 1 when a check failed. The test runner reads exactly those lines.

## Adding the checks of a new feature

Put them in a file of their own and register them. No line of the program and no line of another feature's file changes, so branches that each add a feature never conflict over the tests.

1. Create `<name>_<feature>_tests.inc` next to the program (`<name>` is the program's name without `test_worldd_`), for example `combat_burn_tests.inc`. It is included inside the program's anonymous namespace, so it sees the program's fixture and macros. Write each test as a plain function that takes the program's fixture (or nothing), and register it under its definition:

   ```cpp
   void test_burn_ticks(Fixture& f) {
       std::printf("a burning creature loses Health every second\n");
       CHECK(f.ok(), "...");
   }
   AOC_REGISTER_TEST(test_burn_ticks);
   ```

2. Add ONE line at the end of the program's list of registered files, `<name>_registered.inc` (for `test_worldd_combat.cpp`: `combat_registered.inc`):

   ```cpp
   #include "combat_burn_tests.inc"
   ```

   The list is merged line by line (`merge=union` in `.gitattributes`): when two branches add a line at the same time, git keeps both. The order of the lines does not matter.

3. That is all. The program already includes the list and calls `aoc_tests::run(f)` once in `main()`, which runs every registered test in the order of the source text. No `main()` edit and no build-file edit: the new file is part of the program because the program's `.cpp` includes the list.

Things to keep in mind:

- Test names are unique within a program (the runner reports a repeated name as a failed check and skips the second one).
- A test does not depend on another test having run before it.
- After adding checks, the program's `N passed` count goes up by the number of checks you added. If it does not, the program has no `aoc_tests::run` call (see below).
- A test that throws is reported as a failed check and the rest still run.

## A program that has no registered checks yet

Three small edits, once:

1. `#include "test_registry.h"` next to the program's other includes.
2. Create `<name>_registered.inc` (copy `combat_registered.inc` and keep its comment; the name has to end in `_registered.inc` so that `.gitattributes` merges it by lines) and include it after the program's last `.inc`, inside the anonymous namespace: `#include "<name>_registered.inc"`.
3. Call `g_fail += aoc_tests::run(f);` in `main()`, once, where the registered checks should run (`f` is the program's fixture; a program without one calls `aoc_tests::run()`, and then only tests that take nothing can be registered). Use the program's own failure counter in place of `g_fail`.

Older tests do not have to move. A test function that is already written moves by cutting it into its own file and adding the `AOC_REGISTER_TEST` line under it; its output stays the same when `aoc_tests::run(f)` stands where the function used to be called.
