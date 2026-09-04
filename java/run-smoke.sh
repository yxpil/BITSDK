#!/usr/bin/env bash
# Compiles and runs the BitClient smoke test against the fake BIT server.
# Usage: ./run-smoke.sh   (env BIT_URL / BIT_KEY / BIT_PWD optional)
set -euo pipefail
cd "$(dirname "$0")"

rm -rf build
mkdir -p build

# -encoding UTF-8: sources contain UTF-8 literals (中文)
javac -encoding UTF-8 -d build $(find src test -name '*.java')

# UTF-8 stdout/stderr so printed Chinese compares and displays correctly
java -Dfile.encoding=UTF-8 -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 \
    -cp build BitClientSmokeTest
