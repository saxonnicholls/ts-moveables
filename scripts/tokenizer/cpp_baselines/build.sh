#!/bin/sh
#
#  build.sh
#  TSMoveables
#
#  Copyright 2010-2026 Saxon Herschel Nicholls
#
#  Fetches quicktok v0.4.0 and TokenDagger 0.1.1 (commit 1eed815, the PyPI 0.1.1 source) into <deps dir>,
#  builds quicktok with its own Makefile (-O3 -march=native) and TokenDagger's core with the same -O3
#  -march=native (its Makefile says -O2; the higher level is the generous choice), then links
#  cpp_baselines against both. TokenDagger needs PCRE2 with JIT (brew install pcre2).
#
#      scripts/tokenizer/cpp_baselines/build.sh <deps dir> <output binary>
#
set -eu
deps=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
pcre2=$(pcre2-config --prefix 2>/dev/null || brew --prefix pcre2)
mkdir -p "$deps"
[ -d "$deps/quicktok" ] || git clone -q https://github.com/dmatth1/quicktok.git "$deps/quicktok"
git -C "$deps/quicktok" checkout -q v0.4.0
[ -d "$deps/tokendagger" ] || git clone -q https://github.com/M4THYOU/TokenDagger.git "$deps/tokendagger"
git -C "$deps/tokendagger" checkout -q 1eed815d1ef3a23a73b0d6554f99fc0be6ff4164
make -C "$deps/quicktok" lib >/dev/null
CXX=${CXX:-c++}
$CXX -std=c++20 -O3 -DNDEBUG -march=native -pthread -w \
    -I"$deps/quicktok/include" -I"$deps/tokendagger/src/tiktoken" -I"$pcre2/include" \
    "$here/cpp_baselines.cpp" "$deps/tokendagger/src/tiktoken/tiktoken.cpp" \
    "$deps/quicktok/build/libquicktok.a" "$pcre2/lib/libpcre2-8.a" -o "$out"
echo "built $out"
