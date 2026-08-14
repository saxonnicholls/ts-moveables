#!/usr/bin/env python3
#
#  check_test_registry.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  Every tests/tests_*.cpp must appear in CMakeLists.txt.
#
#  The Makefile globs the test sources, so `make test` picks a new file up the
#  moment it exists. CMakeLists lists them explicitly - which is the right call
#  for a build system people consume, since a glob makes a fresh checkout's
#  build depend on what happens to be on disk. The cost of that choice is that
#  the list can fall behind, and the way it falls behind is nasty: `make test`
#  goes green locally and on most CI jobs, while the three CMake jobs fail at
#  *link* time on the missing run_x_tests() symbol. The error names a symbol,
#  not a file, so it reads like a code problem rather than a bookkeeping one.
#
#  That happened once. This makes it a one-line failure instead, on every job,
#  the same way the MSVC capture rule and the version numbers are checked.
#
#      python3 scripts/check_test_registry.py
#
#  Exit status 0 when the two agree, 1 when they do not.
#

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
CMAKE = os.path.join(ROOT, "CMakeLists.txt")


def main():
    on_disk = sorted(f for f in os.listdir(TESTS)
                     if f.startswith("tests_") and f.endswith(".cpp"))
    if not on_disk:
        print("check_test_registry: found no tests/tests_*.cpp at all", file=sys.stderr)
        return 1

    cmake = open(CMAKE, encoding="utf-8").read()
    missing = [f for f in on_disk if f not in cmake]

    # The reverse direction matters just as much: a file listed but deleted
    # breaks the CMake configure step for everyone who clones.
    listed = set(re.findall(r"tests/(tests_\w+\.cpp)", cmake))
    stale = sorted(listed - set(on_disk))

    print("  %d test sources on disk, %d listed in CMakeLists.txt"
          % (len(on_disk), len(listed)))

    if missing:
        print("check_test_registry: NOT in CMakeLists.txt - the CMake jobs will fail "
              "to link on the missing run_*_tests() symbol:", file=sys.stderr)
        for f in missing:
            print("    tests/%s" % f, file=sys.stderr)
    if stale:
        print("check_test_registry: listed in CMakeLists.txt but not on disk:",
              file=sys.stderr)
        for f in stale:
            print("    tests/%s" % f, file=sys.stderr)

    if missing or stale:
        return 1
    print("test registry: CMakeLists.txt matches tests/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
