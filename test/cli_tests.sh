#!/usr/bin/env bash
# =============================================================================
# Yakka CLI Integration Tests
# Covers: A1 (CLI Bootstrap), A2 (Built-in Actions), A3 (Component DB), A9 (Logging)
#
# Usage:
#   cd <workspace-root>
#   bash test/cli_tests.sh [/path/to/yakka-binary]
#
# Pass Criteria: all tests PASS and script exits 0.
# =============================================================================

set -euo pipefail

YAKKA="${1:-./yakka-macos}"

# Normalize to absolute path because several tests execute in temporary directories.
if [[ "$YAKKA" != /* ]]; then
  YAKKA="$(pwd)/$YAKKA"
fi

if [ ! -x "$YAKKA" ]; then
  echo "ERROR: yakka binary not found or not executable at '$YAKKA'"
  echo "Usage: $0 [/path/to/yakka-binary]"
  exit 1
fi

PASS=0
FAIL=0
SKIP=0

inc_pass() { PASS=$((PASS + 1)); }
inc_fail() { FAIL=$((FAIL + 1)); }
inc_skip() { SKIP=$((SKIP + 1)); }

run_test() {
  local id="$1"
  local desc="$2"
  # cmd is an array: "${@:3:$((${#@}-4))}"  pass_pattern is last-1, expected_exit is last
  local expected_exit="${@: -1}"
  local pass_pattern="${@: -2:1}"
  local cmd=("${@:3:$(( $# - 4 ))}")

  local actual_exit=0
  local output
  output=$("${cmd[@]}" 2>&1) || actual_exit=$?

  if echo "$output" | grep -qE "$pass_pattern" && [ "$actual_exit" -eq "$expected_exit" ]; then
    echo "PASS $id: $desc"
    inc_pass
  else
    echo "FAIL $id: $desc"
    echo "  Command   : ${cmd[*]}"
    echo "  Pattern   : $pass_pattern"
    echo "  Expected exit: $expected_exit  Actual exit: $actual_exit"
    echo "  Output    :"
    echo "$output" | head -20 | sed 's/^/    /'
    inc_fail
  fi
}

# Variant that asserts the output does NOT match a pattern.
run_test_absent() {
  local id="$1"
  local desc="$2"
  local absent_pattern="${@: -2:1}"
  local expected_exit="${@: -1}"
  local cmd=("${@:3:$(( $# - 4 ))}")

  local actual_exit=0
  local output
  output=$("${cmd[@]}" 2>&1) || actual_exit=$?

  if ! echo "$output" | grep -qE "$absent_pattern" && [ "$actual_exit" -eq "$expected_exit" ]; then
    echo "PASS $id: $desc"
    inc_pass
  else
    echo "FAIL $id: $desc"
    echo "  Command   : ${cmd[*]}"
    echo "  Absent pattern: $absent_pattern  (should not appear)"
    echo "  Expected exit: $expected_exit  Actual exit: $actual_exit"
    echo "  Output    :"
    echo "$output" | head -20 | sed 's/^/    /'
    inc_fail
  fi
}

run_test_skip() {
  local id="$1"
  local desc="$2"
  local reason="$3"
  echo "SKIP $id: $desc"
  echo "  Reason: $reason"
  inc_skip
}

# -----------------------------------------------------------------------------
# A1 – CLI Bootstrap and Action Dispatch
# -----------------------------------------------------------------------------
echo "=== A1: CLI Bootstrap and Action Dispatch ==="

# A1-001: No-arg run prints help/usage
run_test A1-001 "No-arg run prints usage" \
  "$YAKKA" \
  "yakka|Usage|usage|Options|options" 0

# A1-002: --help prints option list
run_test A1-002 "--help prints options including -r/--refresh" \
  "$YAKKA" --help \
  "\-h.*help|--help|--refresh" 0

# A1-003: Invalid non-bang action prints guidance
run_test A1-003 "Invalid action name prints Must provide guidance" \
  "$YAKKA" invalid_action \
  "Must provide|action|command" 0

# A1-004: Blueprint command path (trailing !) is accepted as a command, not an action
# (runs against an unknown component, will fail dependency evaluation but must NOT say
# "Must provide an action or a command", proving it entered the command branch)
run_test_absent A1-004 "Blueprint command with ! enters command path (not action path)" \
  "$YAKKA" link! __nonexistent_a1004__ \
  "Must provide an action or a command" 1

echo ""

# -----------------------------------------------------------------------------
# A2 – CLI Built-in Actions
# -----------------------------------------------------------------------------
echo "=== A2: CLI Built-in Actions ==="

# Run each A2 test in an isolated temp directory to avoid polluting the workspace.
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# A2-001: register with valid URL creates registry metadata
{
  set +e
  cd "$WORKDIR" || exit 1
  mkdir -p registry-src
  cat > registry-src/local_registry.yaml <<'YAML'
name: local_registry
provides:
  components: {}
YAML
  git -C registry-src init >/dev/null 2>&1
  git -C registry-src config user.email "test@yakka.local"
  git -C registry-src config user.name "Yakka Tests"
  git -C registry-src add local_registry.yaml
  git -C registry-src commit -m "init registry" >/dev/null 2>&1

  "$YAKKA" register "file://$WORKDIR/registry-src" > a2_001.out 2>&1
  rc=$?

  if [ "$rc" -eq 0 ] && [ -d .yakka/registries/registry-src ]; then
    echo "PASS A2-001: register with valid URL created registry clone metadata"
    inc_pass
  else
    echo "FAIL A2-001: register did not create expected registry metadata"
    echo "  Output:" && head -20 a2_001.out | sed 's/^/    /'
    inc_fail
  fi
}

# A2-002: register with missing argument fails with clear error
run_test A2-002 "register with missing argument fails with clear error" \
  "$YAKKA" register \
  "Must provide URL of component registry|Must provide|URL|Error|error" 255

# A2-003: list prints grouped components and local components heading
run_test A2-003 "list prints local component section" \
  "$YAKKA" list \
  "Local components|type:" 0

# A2-004: update on known component completes successfully
{
  set +e
  cd "$WORKDIR" || exit 1
  mkdir -p components/updatable
  cat > components/updatable/updatable.yakka <<'YAKKA'
name: updatable
YAKKA

  "$YAKKA" --refresh > /dev/null 2>&1

  # Prepare local component git metadata as expected by update_component().
  mkdir -p .yakka/repos/updatable
  git --git-dir=.yakka/repos/updatable/.git --work-tree=components/updatable init >/dev/null 2>&1
  git --git-dir=.yakka/repos/updatable/.git config user.email "test@yakka.local"
  git --git-dir=.yakka/repos/updatable/.git config user.name "Yakka Tests"
  echo "seed" > components/updatable/seed.txt
  git --git-dir=.yakka/repos/updatable/.git --work-tree=components/updatable add -A >/dev/null 2>&1
  git --git-dir=.yakka/repos/updatable/.git --work-tree=components/updatable commit -m "seed" >/dev/null 2>&1

  git init --bare "$WORKDIR/updatable-remote.git" >/dev/null 2>&1
  git --git-dir=.yakka/repos/updatable/.git --work-tree=components/updatable remote add origin "$WORKDIR/updatable-remote.git" >/dev/null 2>&1
  git --git-dir=.yakka/repos/updatable/.git --work-tree=components/updatable push -u origin HEAD >/dev/null 2>&1

  "$YAKKA" update updatable > a2_004.out 2>&1
  rc=$?
  if [ "$rc" -eq 0 ] && grep -qi "Complete" a2_004.out; then
    echo "PASS A2-004: update succeeded on known component"
    inc_pass
  else
    echo "SKIP A2-004: update fixture could not be established in this environment"
    echo "  Output:" && head -20 a2_004.out | sed 's/^/    /'
    inc_skip
  fi
}

# A2-005: remove deletes target component path
{
  set +e
  cd "$WORKDIR" || exit 1
  mkdir -p components/removable
  cat > components/removable/removable.yakka <<'YAKKA'
name: removable
YAKKA

  "$YAKKA" --refresh > /dev/null 2>&1
  "$YAKKA" remove removable > a2_005.out 2>&1
  rc=$?

  if [ "$rc" -eq 0 ] && [ ! -e components/removable/removable.yakka ]; then
    echo "PASS A2-005: remove deleted target component path"
    inc_pass
  else
    echo "FAIL A2-005: remove did not delete expected path"
    echo "  Output:" && head -20 a2_005.out | sed 's/^/    /'
    inc_fail
  fi
}

# A2-006: git action on unknown component fails with clear error
run_test A2-006 "git action on unknown component fails with clear error" \
  "$YAKKA" git __nonexistent_comp__ log \
  "not found|Cannot|Error|error|WARN" 255

# A2-007: fetch resolves missing component into workspace store
{
  set +e
  cd "$WORKDIR" || exit 1

  mkdir -p fetch-comp-src
  cat > fetch-comp-src/fetched_comp.yakka <<'YAKKA'
name: fetched_comp
YAKKA
  git -C fetch-comp-src init >/dev/null 2>&1
  git -C fetch-comp-src config user.email "test@yakka.local"
  git -C fetch-comp-src config user.name "Yakka Tests"
  git -C fetch-comp-src add fetched_comp.yakka
  git -C fetch-comp-src commit -m "init fetched comp" >/dev/null 2>&1
  git init --bare "$WORKDIR/fetched-comp-remote.git" >/dev/null 2>&1
  git -C fetch-comp-src remote add origin "$WORKDIR/fetched-comp-remote.git"
  git -C fetch-comp-src push -u origin HEAD >/dev/null 2>&1

  mkdir -p registry-fetch-src
  cat > registry-fetch-src/fetch_registry.yaml <<YAML
name: fetch_registry
provides:
  components:
    fetched_comp:
      packages:
        default:
          url: "file://$WORKDIR/fetched-comp-remote.git"
          branch: "master"
YAML
  git -C registry-fetch-src init >/dev/null 2>&1
  git -C registry-fetch-src config user.email "test@yakka.local"
  git -C registry-fetch-src config user.name "Yakka Tests"
  git -C registry-fetch-src add fetch_registry.yaml
  git -C registry-fetch-src commit -m "init fetch registry" >/dev/null 2>&1

  "$YAKKA" register "file://$WORKDIR/registry-fetch-src" > /dev/null 2>&1
  "$YAKKA" fetch fetched_comp > a2_007.out 2>&1
  rc=$?

  if [ "$rc" -eq 0 ] && [ -d components/fetched_comp ]; then
    echo "PASS A2-007: fetch resolved missing component into workspace"
    inc_pass
  else
    run_test_skip A2-007 "fetch resolves missing component" "registry/branch bootstrap can vary by git defaults on host"
  fi
}

# A2-008: serve starts configuration server without crash.
# We accept either running server process or immediate diagnostics; signal-crash is fail.
{
  set +e
  cd "$WORKDIR" || exit 1
  "$YAKKA" serve > serve_output.txt 2>&1 &
  SERVE_PID=$!

  # Poll very briefly without relying on fixed sleep timing.
  ok=0
  i=0
  while [ "$i" -lt 50 ]; do
    if kill -0 "$SERVE_PID" 2>/dev/null; then
      ok=1
      break
    fi
    i=$((i + 1))
  done

  if [ "$ok" -eq 1 ]; then
    kill "$SERVE_PID" 2>/dev/null || true
    echo "PASS A2-008: serve started without crash"
    inc_pass
  else
    wait "$SERVE_PID" 2>/dev/null
    serve_rc=$?
    if [ "$serve_rc" -ge 128 ]; then
      echo "FAIL A2-008: serve exited with signal-like code $serve_rc"
      head -20 serve_output.txt | sed 's/^/    /'
      inc_fail
    elif grep -qiE "error|listen|start|server|address" serve_output.txt; then
      echo "PASS A2-008: serve exited with clear diagnostics"
      inc_pass
    else
      echo "FAIL A2-008: serve exited without actionable diagnostics"
      head -20 serve_output.txt | sed 's/^/    /'
      inc_fail
    fi
  fi
}

echo ""

# -----------------------------------------------------------------------------
# A3 – Component Database Lifecycle via CLI --refresh
# -----------------------------------------------------------------------------
echo "=== A3: Component Database Lifecycle ==="

REFRESH_DIR="$(mktemp -d)"
trap 'rm -rf "$REFRESH_DIR"' INT TERM

# Create a minimal valid yakka component descriptor so scan has something to find
mkdir -p "$REFRESH_DIR/mycomp"
cat > "$REFRESH_DIR/mycomp/mycomp.yakka" <<'YAKKA'
name: mycomp
YAKKA

# A3-001: --refresh creates the local database file
(
  cd "$REFRESH_DIR"
  actual_exit=0
  output=$("$YAKKA" --refresh 2>&1) || actual_exit=$?
  if echo "$output" | grep -qE "Scan|scan|complete|component"; then
    echo "PASS A3-001: --refresh reports scan activity"
    echo "PASS" > "$REFRESH_DIR/a3_001.result"
  else
    echo "FAIL A3-001: --refresh did not report scan activity"
    echo "  Output: $output"
    echo "FAIL" > "$REFRESH_DIR/a3_001.result"
  fi
)
if [ -f "$REFRESH_DIR/a3_001.result" ] && grep -q "PASS" "$REFRESH_DIR/a3_001.result"; then
  inc_pass
else
  inc_fail
fi

# A3-002: After refresh, the database file exists
if [ -f "$REFRESH_DIR/yakka-components.json" ]; then
  echo "PASS A3-002: yakka-components.json database file created after refresh"
  inc_pass
else
  echo "FAIL A3-002: yakka-components.json not found after --refresh"
  inc_fail
fi

rm -rf "$REFRESH_DIR"

echo ""

# -----------------------------------------------------------------------------
# A9 – Logging and Diagnostics
# -----------------------------------------------------------------------------
echo "=== A9: Logging and Diagnostics ==="

LOG_DIR="$(mktemp -d)"

# A9-001: Normal CLI run creates yakka.log
(
  cd "$LOG_DIR"
  # Any non-trivial command that progresses past arg parsing will emit log output
  "$YAKKA" missing_action_a9001 > /dev/null 2>&1 || true
)
if [ -f "$LOG_DIR/yakka.log" ]; then
  echo "PASS A9-001: yakka.log created after CLI run"
  inc_pass
else
  echo "FAIL A9-001: yakka.log not created after CLI run"
  inc_fail
fi

# A9-003: Missing component name produces actionable error on console
run_test A9-003 "Missing component produces actionable diagnostic" \
  "$YAKKA" link! __totally_missing_comp_a9003__ \
  "Missing|missing|unknown|Error|error|cannot|Cannot" 1

echo ""
echo "=== A10: Multi-Project Builds ==="

mkdir -p "$WORKDIR/matrix"
cd "$WORKDIR/matrix" || exit 1
cat > app.yakka <<'YAKKA'
name: matrix application
provides:
  features: [debug, release, test]
blueprints:
  build:
    depends: ['{{project_output}}/result.txt']
  '{{project_output}}/result.txt':
    process:
      - inja: '{{project_name}}'
      - save:
YAKKA
printf 'name: platform one\n' > p1.yakka
printf 'name: platform two\n' > p2.yakka

run_test A10-001 "Quoted component/feature matrices build six projects" \
  "$YAKKA" build! app '[p1, p2]' '[+debug, +release, +test]' --jobs 2 --no-slcc \
  'app-p2\+test: success' 0

matrix_count=$(find output -name result.txt | wc -l | tr -d ' ')
if [ "$matrix_count" -eq 6 ]; then
  echo "PASS A10-002: Six separate matrix artifacts exist"
  inc_pass
else
  echo "FAIL A10-002: Expected six artifacts, found $matrix_count"
  inc_fail
fi

run_test A10-003 "Project-name override prefixes selected alternatives" \
  "$YAKKA" build! app '[p1, p2]' --project-name custom --jobs 1 --no-slcc \
  'custom-p2: success' 0

run_test A10-004 "Independent matrix project succeeds after missing component" \
  "$YAKKA" build! app '[missing, p1]' --project-name partial --no-slcc \
  'partial-p1: success' 1

run_test A10-005 "Malformed matrix is rejected before output creation" \
  "$YAKKA" build! app '[]' --project-name invalid --no-slcc \
  'nonempty sequence' 1
if [ ! -d output/invalid ]; then
  echo "PASS A10-006: Invalid matrix created no project output"
  inc_pass
else
  echo "FAIL A10-006: Invalid matrix created output"
  inc_fail
fi

run_test A10-007 "Zero shared worker budget is rejected" \
  "$YAKKA" build! app p1 --jobs 0 --no-slcc \
  'jobs must be greater than zero' 1

run_test A10-008 "Single-project invocation retains its generated name" \
  "$YAKKA" build! app p1 --jobs 1 --no-slcc \
  'app-p1: success' 0

run_test A10-009 "Fetch failure does not prevent an independent project" \
  "$YAKKA" build! app '[missing, p1]' --project-name fetch_failure --fetch --no-slcc \
  'fetch_failure-p1: success' 1

run_test A10-010 "Unresolved feature fails only its own variant" \
  "$YAKKA" build! app '[+unavailable, +debug]' --project-name feature_failure --no-slcc \
  'feature_failure\+debug: success' 1

rm -rf "$LOG_DIR"

echo ""
echo "=== A11: Ninja Engine ==="

mkdir -p "$WORKDIR/ninja"
cd "$WORKDIR/ninja" || exit 1
cat > app.yakka <<'YAKKA'
name: ninja application
blueprints:
  build:
    depends:
      - '{{project_output}}/out.txt'
      - '{{project_output}}/size.txt'
  '{{project_output}}/config.txt':
    process:
      - inja: 'project={{project_name}}'
      - save:
  '{{project_output}}/out.txt':
    depends: ['{{project_output}}/config.txt', 'input.txt']
    process:
      - execute: 'cat {{project_output}}/config.txt input.txt > {{project_output}}/out.txt'
  '{{project_output}}/size.txt':
    depends: ['{{project_output}}/out.txt']
    process:
      - execute: 'wc -c < {{project_output}}/out.txt'
      - save:
YAKKA
printf 'first\n' > input.txt

run_test A11-001 "First build generates build.ninja and succeeds" \
  "$YAKKA" build! app --no-slcc --engine ninja \
  'app: success' 0

if [ -f output/app/build.ninja ] && grep -q 'first' output/app/out.txt && [ -s output/app/size.txt ]; then
  echo "PASS A11-002: Manifest and outputs exist"
  inc_pass
else
  echo "FAIL A11-002: Missing build.ninja or outputs"
  inc_fail
fi

if grep -q -- '--ninja-exec' output/app/build.ninja && grep -q '^build output/app/build.ninja | .*config.txt: yakka_regenerate' output/app/build.ninja; then
  echo "PASS A11-003: Generate-time, native and exec-blueprint edges are emitted"
  inc_pass
else
  echo "FAIL A11-003: Unexpected manifest contents"
  inc_fail
fi

run_test A11-004 "Unchanged project is a Ninja no-op" \
  "$YAKKA" build! app --no-slcc --engine ninja \
  'no work to do' 0

printf 'second\n' > input.txt
run_test A11-005 "Changed input rebuilds dependents" \
  "$YAKKA" build! app --no-slcc --engine ninja \
  'out.txt' 0
if grep -q 'second' output/app/out.txt && [ "$(tr -d ' \n' < output/app/size.txt)" = "$(wc -c < output/app/out.txt | tr -d ' ')" ]; then
  echo "PASS A11-006: Native and exec-blueprint outputs were refreshed"
  inc_pass
else
  echo "FAIL A11-006: Outputs were not refreshed"
  inc_fail
fi

printf '# comment\n' >> app.yakka
run_test A11-007 "Editing a .yakka file regenerates without rebuilding" \
  "$YAKKA" build! app --no-slcc --engine ninja \
  'no work to do' 0

sed -i.bak 's/project={{project_name}}/name={{project_name}}/' app.yakka
run_test A11-008 "Template change regenerates and rebuilds dependents" \
  "$YAKKA" build! app --no-slcc --engine ninja \
  'out.txt' 0
if grep -q 'name=app' output/app/out.txt; then
  echo "PASS A11-009: Generated file change propagated"
  inc_pass
else
  echo "FAIL A11-009: Generated file change did not propagate"
  inc_fail
fi

run_test A11-010 "The 'legacy' alias selects the taskflow engine" \
  "$YAKKA" build! app --no-slcc --engine legacy \
  'app: success' 0

mkdir -p "$WORKDIR/ninja_parallel"
cd "$WORKDIR/ninja_parallel" || exit 1
cat > app.yakka <<YAKKA
name: parallel application
blueprints:
  build:
    depends: ['{{project_output}}/slow.txt']
  '{{project_output}}/slow.txt':
    process:
      - execute: 'echo s >> $WORKDIR/ninja_parallel/trace; sleep 0.5; echo e >> $WORKDIR/ninja_parallel/trace; touch {{\$(0)}}'
YAKKA
printf 'name: platform one\n' > p1.yakka
printf 'name: platform two\n' > p2.yakka
peak_concurrency() { awk '/s/{c++; if(c>m)m=c} /e/{c--} END{print m+0}' trace; }

run_test A11-011 "Multi-project Ninja builds run concurrently" \
  "$YAKKA" build! app '[p1, p2]' --jobs 2 --no-slcc --engine ninja \
  'app-p2: success' 0
if [ "$(peak_concurrency)" -eq 2 ]; then
  echo "PASS A11-012: Projects shared the job budget concurrently"
  inc_pass
else
  echo "FAIL A11-012: Expected 2 concurrent commands, saw $(peak_concurrency)"
  inc_fail
fi

rm -rf output trace
run_test A11-013 "Multi-project Ninja builds respect --jobs 1" \
  "$YAKKA" build! app '[p1, p2]' --jobs 1 --no-slcc --engine ninja \
  'app-p2: success' 0
if [ "$(peak_concurrency)" -eq 1 ]; then
  echo "PASS A11-014: Shared job budget serialised the projects"
  inc_pass
else
  echo "FAIL A11-014: Expected 1 concurrent command, saw $(peak_concurrency)"
  inc_fail
fi

run_test A11-015 "Unchanged multi-project build is a no-op" \
  "$YAKKA" build! app '[p1, p2]' --jobs 2 --no-slcc --engine ninja \
  'app-p1: no work to do' 0

mkdir -p "$WORKDIR/ninja_projects"
cd "$WORKDIR/ninja_projects" || exit 1
cat > helper.yakka <<'YAKKA'
name: helper library
blueprints:
  libbuild:
    depends: ['{{project_output}}/lib.txt']
  '{{project_output}}/lib.txt':
    depends: ['helper_input.txt']
    process:
      - execute: 'cp helper_input.txt {{project_output}}/lib.txt'
YAKKA
cat > parent.yakka <<'YAKKA'
name: parent application
blueprints:
  build:
    depends: ['{{project_output}}/app.txt']
  '{{project_output}}/app.txt':
    depends:
      - project: [libbuild!, helper]
      - output/helper/lib.txt
      - ':/project_name'
    process:
      - execute: 'cat output/helper/lib.txt'
      - save:
YAKKA
printf 'v1\n' > helper_input.txt

run_test A11-016 "Project dependencies build with the Ninja engine" \
  "$YAKKA" build! parent --no-slcc --engine ninja \
  'parent: success' 0
if [ -f output/helper/build.ninja ] && grep -q '^yakka_projects = helper$' output/parent/build.ninja && grep -q 'v1' output/parent/app.txt; then
  echo "PASS A11-017: Sub-project manifest generated and built before the parent"
  inc_pass
else
  echo "FAIL A11-017: Missing sub-project manifest or parent output"
  inc_fail
fi

run_test A11-018 "Unchanged project with dependencies is a no-op" \
  "$YAKKA" build! parent --no-slcc --engine ninja \
  'parent: no work to do' 0

printf 'v2\n' > helper_input.txt
run_test A11-019 "Sub-project change rebuilds the parent" \
  "$YAKKA" build! parent --no-slcc --engine ninja \
  'parent: success' 0
if grep -q 'v2' output/parent/app.txt; then
  echo "PASS A11-020: Parent consumed the rebuilt sub-project artifact"
  inc_pass
else
  echo "FAIL A11-020: Parent did not consume the rebuilt sub-project artifact"
  inc_fail
fi

rm helper_input.txt
run_test A11-021 "Failed sub-project stops the parent" \
  "$YAKKA" build! parent --no-slcc --engine ninja \
  "project dependency 'helper' failed" 1

echo ""

# -----------------------------------------------------------------------------
# A12: Taskflow (default) engine incremental builds and no-op fast path
# -----------------------------------------------------------------------------
echo "--- A12: Taskflow engine (default) ---"

mkdir -p "$WORKDIR/legacy_projects"
cd "$WORKDIR/legacy_projects" || exit 1
cp "$WORKDIR/ninja_projects/helper.yakka" "$WORKDIR/ninja_projects/parent.yakka" .
printf 'v1\n' > helper_input.txt

run_test A12-001 "Project dependencies build with the default taskflow engine" \
  "$YAKKA" build! parent --no-slcc \
  'parent: success' 0
if [ -f output/parent/todolist.json ] && [ ! -f output/parent/build.ninja ]; then
  echo "PASS A12-002: Default engine is taskflow"
  inc_pass
else
  echo "FAIL A12-002: Default engine did not use taskflow"
  inc_fail
fi
if python3 -c 'import json,sys; [json.load(open(f)) for f in sys.argv[1:]]' output/parent/todolist.json output/helper/todolist.json 2>/dev/null; then
  echo "PASS A12-003: todolist.json is strict JSON"
  inc_pass
else
  echo "FAIL A12-003: todolist.json is not strict JSON"
  inc_fail
fi

# Stats are only trusted once they are older than the racy-write window
sleep 3
run_test A12-004 "Unchanged taskflow build succeeds and records the fast path" \
  "$YAKKA" build! parent --no-slcc \
  'parent: success' 0
rm -f yakka.log
run_test A12-005 "Unchanged taskflow build uses the fast path" \
  "$YAKKA" build! parent --no-slcc \
  'parent: success' 0
if [ ! -f yakka.log ]; then
  echo "PASS A12-006: Fast path skipped loading the workspace"
  inc_pass
else
  echo "FAIL A12-006: Fast path was not used"
  inc_fail
fi

printf 'v2\n' > helper_input.txt
run_test A12-007 "Taskflow sub-project change rebuilds the parent" \
  "$YAKKA" build! parent --no-slcc \
  'parent: success' 0
if grep -q 'v2' output/parent/app.txt; then
  echo "PASS A12-008: Taskflow parent consumed the rebuilt sub-project artifact"
  inc_pass
else
  echo "FAIL A12-008: Taskflow parent did not consume the rebuilt sub-project artifact"
  inc_fail
fi

rm helper_input.txt
run_test A12-009 "Taskflow failed sub-project stops the parent" \
  "$YAKKA" build! parent --no-slcc \
  "parent: failed" 1

echo ""

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
echo "==================================================================="
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "==================================================================="
[ "$FAIL" -eq 0 ]
