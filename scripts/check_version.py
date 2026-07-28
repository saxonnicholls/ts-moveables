#!/usr/bin/env python3
#
#  check_version.py
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  The library's version is written down three times: as macros in
#  TSMoveables/version.hpp, as project(... VERSION) in CMakeLists.txt, and as
#  the git tag on a release. Those are three copies of one fact, and the
#  failure mode is not that they disagree loudly - it is that a release goes
#  out where find_package() reports one number, the header reports another,
#  and nobody notices until someone writes a version check that silently does
#  the wrong thing.
#
#  So they are compared on every CI job, the same way the MSVC capture rule is:
#  it is a static property of the tree, and there is no reason to learn about
#  it from a release forty minutes later.
#
#      python3 scripts/check_version.py                 # header vs CMake
#      python3 scripts/check_version.py --tag v1.0.0    # ... and vs a tag
#
#  Exit status is 0 when they agree and 1 when they do not.
#

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "TSMoveables", "version.hpp")
CMAKE = os.path.join(ROOT, "CMakeLists.txt")


def header_version():
    src = open(HEADER, encoding="utf-8").read()
    parts = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"^#define\s+SNICHOLLS_VERSION_%s\s+(\d+)\s*$" % field, src, re.M)
        if not m:
            raise SystemExit("check_version: SNICHOLLS_VERSION_%s not found in %s"
                             % (field, os.path.relpath(HEADER, ROOT)))
        parts.append(m.group(1))
    return ".".join(parts)


def cmake_version():
    src = open(CMAKE, encoding="utf-8").read()
    # project(name \n VERSION x.y.z ...) - the VERSION belonging to project(),
    # not cmake_minimum_required(VERSION ...), which is a different thing
    m = re.search(r"project\s*\([^)]*?\bVERSION\s+(\d+\.\d+\.\d+)", src, re.S)
    if not m:
        raise SystemExit("check_version: project(... VERSION x.y.z) not found in CMakeLists.txt")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description="Check the version is the same everywhere")
    ap.add_argument("--tag", default=os.environ.get("RELEASE_TAG"),
                    help="a git tag to check too, with or without a leading 'v'")
    args = ap.parse_args()

    hdr = header_version()
    cml = cmake_version()

    ok = True
    print("  TSMoveables/version.hpp   %s" % hdr)
    print("  CMakeLists.txt            %s" % cml)
    if hdr != cml:
        print("check_version: header and CMakeLists disagree", file=sys.stderr)
        ok = False

    if args.tag:
        tag = args.tag.lstrip("v")
        print("  git tag                   %s" % args.tag)
        if tag != hdr:
            print("check_version: tag %s does not match version %s" % (args.tag, hdr),
                  file=sys.stderr)
            ok = False

    if not ok:
        print("check_version: FAILED - one release, one number", file=sys.stderr)
        return 1
    print("version check: consistent (%s)" % hdr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
