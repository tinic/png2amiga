#!/usr/bin/env bash
# clang-tidy + cppcheck over src/. Runs the curated .clang-tidy check set
# and a focused cppcheck pass. Returns non-zero if either reports issues.
#
# Uses build-lint/ (configured with homebrew clang) for the compile DB so
# clang-tidy sees a consistent flag set; the main GCC build is untouched.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LLVM_PREFIX="${LLVM_PREFIX:-/opt/homebrew/opt/llvm}"
CLANG_TIDY="$LLVM_PREFIX/bin/clang-tidy"
RUN_CLANG_TIDY="$LLVM_PREFIX/bin/run-clang-tidy"
CLANG="$LLVM_PREFIX/bin/clang"
CLANGXX="$LLVM_PREFIX/bin/clang++"
CPPCHECK="${CPPCHECK:-cppcheck}"

for tool in "$CLANG_TIDY" "$RUN_CLANG_TIDY" "$CLANG" "$CLANGXX"; do
    if [[ ! -x "$tool" ]]; then
        echo "lint: missing $tool — install via 'brew install llvm'" >&2
        exit 2
    fi
done
if ! command -v "$CPPCHECK" >/dev/null 2>&1; then
    echo "lint: missing cppcheck — install via 'brew install cppcheck'" >&2
    exit 2
fi

BUILD_LINT="$REPO_ROOT/build-lint"
if [[ ! -f "$BUILD_LINT/compile_commands.json" ]]; then
    echo "lint: configuring $BUILD_LINT with homebrew clang"
    CC="$CLANG" CXX="$CLANGXX" cmake -S "$REPO_ROOT" -B "$BUILD_LINT" \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        > /dev/null
fi

# Generate version.hpp + ocs_cand_table.cpp etc. so headers parse cleanly.
cmake --build "$BUILD_LINT" --target ocs_cand_table > /dev/null 2>&1 || true
cmake --build "$BUILD_LINT" --target png2amiga -- -j1 -t version.hpp > /dev/null 2>&1 || true

SRC_FILES=()
while IFS= read -r f; do
    SRC_FILES+=("$f")
done < <(find "$REPO_ROOT/src" -maxdepth 1 -name '*.cpp' | sort)

echo "=== clang-tidy ($(${CLANG_TIDY} --version | head -1 | tr -s ' ')) ==="
TIDY_LOG="$(mktemp -t png2amiga-tidy.XXXXXX)"
trap 'rm -f "$TIDY_LOG"' EXIT
if ! PATH="$LLVM_PREFIX/bin:$PATH" "$RUN_CLANG_TIDY" \
        -p "$BUILD_LINT" -quiet -j "$(sysctl -n hw.ncpu)" \
        "${SRC_FILES[@]}" > "$TIDY_LOG" 2>&1; then
    grep -E "(warning|error):" "$TIDY_LOG" | sort -u | head -200 || true
    echo "lint: clang-tidy reported issues (full log: $TIDY_LOG)"
    TIDY_FAIL=1
else
    if grep -qE "(warning|error):" "$TIDY_LOG"; then
        grep -E "(warning|error):" "$TIDY_LOG" | sort -u | head -200
        TIDY_FAIL=1
    else
        echo "clang-tidy: clean"
        TIDY_FAIL=0
    fi
fi

echo
echo "=== cppcheck ($("$CPPCHECK" --version)) ==="
CPPCHECK_LOG="$(mktemp -t png2amiga-cppcheck.XXXXXX)"
trap 'rm -f "$TIDY_LOG" "$CPPCHECK_LOG"' EXIT
if "$CPPCHECK" \
        --enable=warning,performance,portability \
        --inline-suppr \
        --quiet \
        --error-exitcode=1 \
        --std=c++23 \
        --suppress=missingIncludeSystem \
        --suppress=knownConditionTrueFalse \
        --suppress=unusedFunction \
        --suppress=uninitMemberVar:src/ham.hpp \
        --suppress=passedByValueCallback:src/wasm_bindings.cpp \
        --suppress=CastAddressToIntegerAtReturn \
        --suppress=containerOutOfBounds:src/ham.cpp \
        --suppress=*:third_party/* \
        -I "$REPO_ROOT/src" \
        -I "$BUILD_LINT/generated" \
        -j "$(sysctl -n hw.ncpu)" \
        "$REPO_ROOT/src" 2> "$CPPCHECK_LOG"; then
    if [[ -s "$CPPCHECK_LOG" ]]; then
        cat "$CPPCHECK_LOG"
    fi
    echo "cppcheck: clean"
    CPPCHECK_FAIL=0
else
    cat "$CPPCHECK_LOG"
    CPPCHECK_FAIL=1
fi

echo
if (( TIDY_FAIL || CPPCHECK_FAIL )); then
    echo "lint: FAIL  (clang-tidy=$TIDY_FAIL  cppcheck=$CPPCHECK_FAIL)"
    exit 1
fi
echo "lint: PASS"
