#!/usr/bin/env bash
set -euo pipefail

# Usage: bash test/benchmark_cli_tests.sh /path/to/yakka-binary
YAKKA="${1:-./output/yakka_cli-xcode/yakka_cli-xcode}"
if [[ "$YAKKA" != /* ]]; then
  YAKKA="$(pwd)/$YAKKA"
fi
if [ ! -x "$YAKKA" ]; then
  echo "ERROR: yakka binary not executable: $YAKKA"
  exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
cd "$WORKDIR"
mkdir -p output/sample directory
printf 'source\n' > source.txt
: > empty.txt
printf 'header\n' > 'header space.hpp'
printf 'first-build header\n' > fresh.hpp
printf '\000\001\377' > output/sample/generated.bin
cat > output/sample/compiler.d <<'DEP'
output/sample/generated.bin: source.txt header\ space.hpp \
 fresh.hpp
DEP
cat > output/sample/target_database.json <<'JSON'
{"targets":[
  {"target":"build","matches":[{"dependencies":[
    "source.txt","./source.txt","empty.txt","header space.hpp",
    "output/sample/generated.bin",":/data/value","directory","phony"
  ]}]},
  {"target":"phony","matches":[{"dependencies":[]}]},
  {"target":"source.txt","matches":[]},
  {"target":"empty.txt","matches":[]},
  {"target":"header space.hpp","matches":[]},
  {"target":"output/sample/generated.bin","matches":[]}
]}
JSON

before="$(cksum output/sample/* source.txt empty.txt 'header space.hpp' fresh.hpp)"
output="$("$YAKKA" benchmark sample)"
printf '%s\n' "$output"
grep -q 'Files: 6 (unique paths; skipped non-file targets: 3)' <<< "$output"
for label in 'Timestamp' 'Hash \(rapidhash, full file read\)'; do
  grep -qE "^${label}: 6 comparisons, [0-9]+\.[0-9]+ ms total, [0-9]+\.[0-9]+ us/file, 0 changed$" <<< "$output"
done
test "$before" = "$(cksum output/sample/* source.txt empty.txt 'header space.hpp' fresh.hpp)"
echo "PASS: both methods compare all six unique files without changing project state"

expect_failure() {
  local pattern="$1"
  shift
  local output
  local status=0
  output=$("$YAKKA" benchmark "$@" 2>&1) || status=$?
  if [ "$status" -ne 1 ] || ! grep -qE "$pattern" <<< "$output"; then
    printf 'FAIL: expected exit 1 and %s; got exit %s:\n%s\n' "$pattern" "$status" "$output"
    exit 1
  fi
  if grep -q 'ms total' <<< "$output"; then
    echo "FAIL: incomplete benchmark printed successful timings"
    exit 1
  fi
}

expect_failure 'Usage: yakka benchmark'
expect_failure 'Usage: yakka benchmark' sample extra
expect_failure 'Expected a project name, not a path' ../sample
expect_failure 'Cannot load saved target database' nonexistent
mv fresh.hpp moved.hpp
expect_failure 'Missing file dependency: .*fresh.hpp' sample
mv moved.hpp fresh.hpp
rm source.txt
expect_failure 'Missing file dependency: .*source.txt' sample
printf '{"targets":[]}\n' > output/sample/target_database.json
rm output/sample/compiler.d
expect_failure 'No file dependencies found' sample
printf '{"not_targets":[]}\n' > output/sample/target_database.json
expect_failure 'Cannot load saved target database' sample
printf '{"targets":[{"target":"source.txt","matches":[{"dependencies":[{}]}]}]}\n' > output/sample/target_database.json
expect_failure 'invalid target/dependency structure' sample
printf '{"targets": [\n' > output/sample/target_database.json
expect_failure 'Dependency benchmark failed' sample
echo "PASS: invalid arguments, missing dependencies, empty graphs, and invalid saved state fail explicitly"
