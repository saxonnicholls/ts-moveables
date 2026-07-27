#!/usr/bin/env python3
#
#  check_msvc_capture.py
#  TSMoveables
#
#  Copyright 2026 Saxon Herschel Nicholls
#
#  Guards against MSVC C3493, which has now broken CI five times.
#
#  A `constexpr` local used inside a lambda whose capture list has NO default
#  capture mode is rejected by MSVC - "cannot be implicitly captured because no
#  default capture mode has been specified" - while GCC and Clang accept it,
#  since a constant needs no capture. The fix is `static constexpr`: static
#  storage duration means there is nothing to capture on any compiler.
#
#  The subtlety that cost a CI run: `[&lg]` is NOT a default capture mode. Only
#  a bare `&` or `=` as the FIRST element is - `[&]`, `[=]`, `[&, x]`, `[=, &y]`.
#  `[&lg]` is an explicit by-reference capture of one variable, and a constexpr
#  local used inside it is exactly the error. An earlier version of this check
#  tested `startswith('&')` and so skipped precisely the case that was broken.
#
#      python3 scripts/check_msvc_capture.py          # non-zero if any found
#
import glob
import re
import sys


def has_default_capture(capture_list):
    parts = [p.strip() for p in capture_list.split(',') if p.strip()]
    return bool(parts) and parts[0] in ('&', '=')


def scan(path):
    src = open(path, encoding='utf-8').read()
    consts = {}
    for i, line in enumerate(src.split('\n')):
        stripped = line.lstrip()
        if not line[:1].isspace():
            continue                      # namespace scope: static storage already
        if stripped.startswith('static'):
            continue                      # the fix itself
        m = re.match(r'constexpr\s+[\w:<>\s]+?\s(\w+)\s*=', stripped)
        if m:
            consts[m.group(1)] = i + 1
    if not consts:
        return []

    found = []
    for m in re.finditer(r'\[([^\]\[]*)\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?(?:->[^{]+)?\{', src):
        capture = m.group(1)
        if has_default_capture(capture):
            continue
        depth, j = 0, m.end() - 1
        while j < len(src):                # find the lambda's real extent
            if src[j] == '{':
                depth += 1
            elif src[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = src[m.end():j]
        named = {c.strip().lstrip('&') for c in capture.split(',') if c.strip()}
        at = src[:m.start()].count('\n') + 1
        for name, decl in consts.items():
            # Only a declaration that precedes the lambda can be the one it
            # refers to; a later same-named local in another scope is not.
            if decl < at and name not in named and re.search(r'\b' + name + r'\b', body):
                found.append((at, capture, name, decl))
    return found


def main():
    patterns = ['tests/**/*.cpp', 'tests/*.hpp', 'demos/*.cpp', 'benchmarks/*.cpp',
                'TSMoveables/**/*.hpp']
    total = 0
    for pat in patterns:
        for path in sorted(glob.glob(pat, recursive=True)):
            for line, capture, name, decl in scan(path):
                print("%s:%d: lambda [%s] uses constexpr local '%s' (declared line %d)"
                      % (path, line, capture, name, decl))
                print("    MSVC C3493. Fix: make it `static constexpr`.")
                total += 1
    if total:
        print("\n%d site(s) MSVC will reject." % total)
        return 1
    print("MSVC capture check: clean")
    return 0


if __name__ == '__main__':
    sys.exit(main())
