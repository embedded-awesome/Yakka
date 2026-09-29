# Yakka Comprehensive Test Plan

## 1. Purpose and Scope
This plan defines comprehensive feature testing for Yakka with independently runnable sections.

Two test types are covered:
1. Type 1: tests for the Yakka executable flow.
2. Type 2: tests for subsection executables and standalone suites.

Included feature families:
1. CLI parsing, flags, and action dispatch.
2. Component discovery and database persistence.
3. Workspace and registry operations.
4. Project lifecycle and dependency evaluation.
5. Blueprint command execution.
6. Task graph execution and incremental behavior.
7. Template rendering and callback functions.
8. Data dependency behavior.
9. Logging and diagnostics.

Excluded:
1. Internal implementation correctness of third-party libraries under components unless exposed through Yakka integration behavior.

## 2. Test Environment Baseline
Primary platform: macOS.

Parity coverage target: Linux and Windows for compatibility sections.

Baseline assets:
1. test/test_data.json
2. test/test_data.yaml
3. test/test_data.toml
4. test/inja_suite/
5. test/circular_blueprints.yakka

Core executables and suites:
1. yakka-macos
2. unit tests from test/unit_tests.yakka
3. inja_test from inja_test.yakka
4. perf_test from perf_test/perf_test.yakka

Common evidence and diagnostics:
1. yakka.log
2. output/

## 3. Independent Section Template
Every section in this document follows this template:
1. Objective
2. Scope
3. Preconditions
4. Test Cases
5. Execution Method
6. Pass Criteria
7. Debug Notes

## 4. Type 1: Yakka Executable Tests

### A1. CLI Bootstrap and Action Dispatch
Objective:
1. Verify argument parsing and command/action routing behavior.

Scope:
1. Help output and no-arg behavior.
2. Invalid action handling.
3. Blueprint command syntax with trailing !.

Preconditions:
1. yakka-macos built and executable.

Test Cases:
1. A1-001: Run yakka with no args and verify usage/help is printed.
2. A1-002: Run yakka with --help and verify options are listed.
3. A1-003: Run yakka invalid_action and verify error guidance is printed.
4. A1-004: Run yakka link! <component> <toolchain> and verify command path is selected instead of action path.

Execution Method:
1. Manual CLI invocation.

Pass Criteria:
1. Output matches expected dispatch behavior.
2. Invalid action path exits cleanly with actionable message.

Debug Notes:
1. Inspect yakka.log for action routing diagnostics.

### A2. CLI Built-in Actions
Objective:
1. Validate register, list, update, remove, git, fetch, and serve action behavior.

Scope:
1. Happy path and failure path for each action.

Preconditions:
1. Test registry fixture or known reachable registry.
2. Local component fixture for remove and git actions.

Test Cases:
1. A2-001: register with valid URL creates registry metadata.
2. A2-002: register with missing argument fails with clear error.
3. A2-003: list prints grouped components and local components.
4. A2-004: update on known component completes successfully.
5. A2-005: remove deletes target component path.
6. A2-006: git action on unknown component fails with clear error.
7. A2-007: fetch resolves missing component into workspace/shared store.
8. A2-008: serve starts configuration server without crash.

Execution Method:
1. Manual or automated script-driven CLI tests.

Pass Criteria:
1. Correct exit behavior and expected filesystem/state changes per action.

Debug Notes:
1. Inspect yakka.log and .yakka directories.

### A3. Component Database Lifecycle via CLI
Objective:
1. Verify component scanning, persistence, and reload behaviors.

Scope:
1. Refresh scan.
2. Save/load round-trip.
3. Handling invalid descriptor files.

Preconditions:
1. Workspace includes both valid and invalid component descriptor fixtures.

Test Cases:
1. A3-001: yakka --refresh rebuilds local/package databases.
2. A3-002: Restart and verify persisted database state is reused.
3. A3-003: Corrupt descriptor does not crash refresh and yields diagnostic output.

Execution Method:
1. CLI invocation plus filesystem assertions.

Pass Criteria:
1. Databases persist expected components.
2. Invalid descriptors are reported, not fatal to unrelated valid components.

Debug Notes:
1. Inspect .yakka database files and yakka.log.

### A4. Workspace and Registry Operations via CLI Flow
Objective:
1. Validate registry lifecycle integration with workspace behavior.

Scope:
1. Registry load.
2. Fetch and update integration.
3. Missing/unavailable registry handling.

Preconditions:
1. At least one valid registry and one intentionally unavailable registry URL.

Test Cases:
1. A4-001: load registries succeeds with valid config.
2. A4-002: unavailable registry reports failure without crash.
3. A4-003: fetch uses registry data and populates missing components.
4. A4-004: update against unavailable source reports actionable failure.

Execution Method:
1. CLI actions plus state verification in workspace paths.

Pass Criteria:
1. Registry operations degrade gracefully under network or source failure.

Debug Notes:
1. Inspect registry metadata under .yakka and yakka.log.

### A5. Project Lifecycle Orchestration
Objective:
1. Verify end-to-end project lifecycle for CLI-initiated builds.

Scope:
1. init_project
2. evaluate_dependencies
3. generate_project_summary
4. parse_blueprints
5. generate_target_database
6. save_summary

Preconditions:
1. Valid component list and toolchain component in workspace.

Test Cases:
1. A5-001: valid input components produce summary and target graph artifacts.
2. A5-002: unknown component without fetch produces clear missing component diagnostics.
3. A5-003: unknown component with fetch resolves and proceeds.
4. A5-004: conflicting feature constraints are surfaced and build is blocked.

Execution Method:
1. CLI command execution with artifact checks under output/.

Pass Criteria:
1. Lifecycle stages complete in expected order and emit expected artifacts.

Debug Notes:
1. Inspect output/<project>/project_summary.json and yakka.log.

### A6. Blueprint Built-in Commands
Objective:
1. Validate every built-in blueprint command with independent checks.

Scope:
1. echo
2. execute
3. shell
4. regex
5. template
6. inja
7. save
8. create_directory
9. verify
10. rm
11. rmdir
12. pack
13. copy
14. cat
15. as_json
16. as_yaml
17. diff

Preconditions:
1. Controlled fixture workspace with read/write test area.

Test Cases:
1. A6-001 through A6-017: one success case per command with deterministic expected output.
2. A6-101 through A6-117: one failure case per command with invalid inputs.
3. A6-201 through A6-217: one edge case per command (missing file, permission issue, malformed content, or no-op behavior).

Execution Method:
1. Automated fixture-driven integration tests invoking representative blueprints.

Pass Criteria:
1. Command behavior and outputs match expected contract for success, failure, and edge-case variants.

Debug Notes:
1. Capture rendered command data and intermediate files for failed cases.

### A7. Task Engine Integration from Yakka CLI
Objective:
1. Validate task graph ordering, dependency satisfaction, and failure propagation.

Scope:
1. Dependency graph correctness.
2. Incremental behavior from timestamp and data dependency changes.
3. Parallel task execution safety.

Preconditions:
1. Multi-target fixture with explicit dependency chain and independent branches.

Test Cases:
1. A7-001: dependencies execute before dependents.
2. A7-002: unchanged inputs skip unnecessary work.
3. A7-003: data dependency change triggers required rebuild only.
4. A7-004: one task failure stops or marks downstream behavior as expected.
5. A7-005: parallel branches complete with stable artifact correctness.

Execution Method:
1. Automated integration tests with timestamp/artifact assertions.

Pass Criteria:
1. Graph invariants hold for all scenarios.

Debug Notes:
1. Save task ordering traces and execution logs.

### A8. Template Callback Integration in CLI Builds
Objective:
1. Validate callback functions used by templates during build execution.

Scope:
1. File/path callbacks.
2. String/regex callbacks.
3. Data callbacks including merge/filter/find_json.
4. Hash/load callbacks.

Preconditions:
1. Fixture templates referencing each callback category.

Test Cases:
1. A8-001: basic path operations produce expected normalized paths.
2. A8-002: read_file/load_yaml/load_json behavior with valid files.
3. A8-003: malformed data file yields clear template/runtime diagnostic.
4. A8-004: merge/filter/find_json produce expected transformed data.
5. A8-005: hash callbacks produce deterministic results for fixture input.

Execution Method:
1. Integration blueprint runs with output verification.

Pass Criteria:
1. Callback outputs are correct and failures are diagnosable.

Debug Notes:
1. Preserve rendered outputs and template error logs.

### A9. Logging and Diagnostics
Objective:
1. Verify diagnostic quality and log generation behavior.

Scope:
1. Log file creation.
2. Fallback log naming.
3. Actionable error output in common failure paths.

Preconditions:
1. Workspace with writable and non-writable scenarios.

Test Cases:
1. A9-001: standard run creates yakka.log.
2. A9-002: log file creation fallback path works when yakka.log is unavailable.
3. A9-003: representative failures (missing component, invalid command input) emit actionable diagnostics.

Execution Method:
1. Manual or scripted CLI runs with output and log assertions.

Pass Criteria:
1. Logs are always available and diagnostics are specific enough to troubleshoot.

Debug Notes:
1. Archive console output and log file pair for failed tests.

## 5. Type 2: Subsection Executable and Standalone Suite Tests

### B1. inja_test Executable
Objective:
1. Validate standalone template behavior independent of full Yakka flow.

Scope:
1. Basics.
2. Includes.
3. Macros.
4. Custom syntax and loop metadata cases.

Preconditions:
1. inja_test built.
2. test/inja_suite fixtures available.

Test Cases:
1. B1-001: all baseline inja_suite cases pass exactly.
2. B1-002: custom syntax fixture passes.
3. B1-003: include/macro fixtures pass.
4. B1-004: malformed template fixture fails with expected message.

Execution Method:
1. Standalone executable test run with expected output comparisons.

Pass Criteria:
1. Zero mismatches for valid fixtures.
2. Expected error reporting for invalid fixtures.

Debug Notes:
1. Save per-case diff output for failure triage.

### B2. Unit Test Suites
Objective:
1. Validate subsystem unit coverage in independent executable runs.

Scope:
1. workspace_unit_tests
2. data_dependency_unit_tests
3. component_database_test
4. yakka/tests suite

Preconditions:
1. Unit test target built.

Test Cases:
1. B2-001: run workspace unit tests and verify all pass.
2. B2-002: run data dependency unit tests and verify all pass.
3. B2-003: run component database unit tests and verify all pass.
4. B2-004: run yakka project lifecycle tests and verify all pass.

Execution Method:
1. Execute test binary with per-suite filters where supported.

Pass Criteria:
1. No failing tests.
2. No flaky retries allowed in PR gate.

Debug Notes:
1. Store XML or console reports for CI artifacts.

### B3. perf_test Executable
Objective:
1. Track performance regressions for key benchmark scenarios.

Scope:
1. JSON insertion benchmark.
2. YAML insertion benchmark.

Preconditions:
1. perf_test built in release mode.
2. Baseline performance record available.

Test Cases:
1. B3-001: run benchmark and collect current metrics.
2. B3-002: compare against baseline trend band.

Execution Method:
1. Standalone benchmark run.

Pass Criteria:
1. Advisory warning if regression exceeds threshold.
2. Initially non-gating until baseline stabilizes.

Debug Notes:
1. Store raw benchmark output and environment metadata.

### B4. yinja Executable Smoke Tests
Objective:
1. Validate CLI template rendering behavior for yinja binary if shipped in release profile.

Scope:
1. Argument handling.
2. Template file rendering.
3. Missing file/error behavior.

Preconditions:
1. yinja built and available in current profile.

Test Cases:
1. B4-001: render valid template with valid data.
2. B4-002: missing template file reports clear error.
3. B4-003: invalid data format reports clear error.

Execution Method:
1. Standalone binary smoke tests.

Pass Criteria:
1. Correct output for valid case and clear diagnostics for invalid cases.

Debug Notes:
1. Capture stderr/stdout and rendered artifact.

## 6. Cross-Cutting Independent Test Sections

### C1. Negative and Error Injection Matrix
Objective:
1. Confirm robust behavior under invalid inputs and system faults.

Cases:
1. C1-001 malformed YAML/JSON descriptors.
2. C1-002 missing files for cat/save/template/load callbacks.
3. C1-003 invalid regex patterns.
4. C1-004 command non-zero return paths.
5. C1-005 permission denied for write/remove operations.
6. C1-006 unavailable registry/network source.

Pass Criteria:
1. No crashes.
2. Actionable diagnostics in logs and console.

### C2. Compatibility Matrix
Objective:
1. Validate platform-sensitive behavior.

Cases:
1. C2-001 path separator and relative path behavior.
2. C2-002 shell command wrapping differences across platforms.
3. C2-003 filesystem edge behavior for rm/rmdir/copy.

Pass Criteria:
1. Behavior is equivalent by contract across supported platforms.

### C3. Regression Buckets
Objective:
1. Define execution tiers for fast feedback and deep validation.

Buckets:
1. Smoke: A1 subset, A2 subset, B1 sanity.
2. PR Gate: A1-A5, A7 basic, B2 full unit suites.
3. Nightly: A1-A9, B1-B4, C1-C2 exhaustive.
4. Release: full matrix plus benchmark comparison and compatibility pass.

Pass Criteria:
1. Tier-specific required sections pass with no untriaged failures.

### C4. Test Data Management
Objective:
1. Ensure reproducible and maintainable fixtures.

Policy:
1. Store reusable fixtures under test/.
2. Keep fixtures deterministic and minimal.
3. Name fixtures by feature and scenario.
4. Version expected outputs with fixture updates.

Pass Criteria:
1. Tests are deterministic and isolated.

### C5. Coverage Intent Map
Objective:
1. Ensure each major feature family has at least one independent section.

Mapping:
1. CLI parsing and actions -> A1, A2.
2. Component/workspace/registry -> A3, A4.
3. Project lifecycle -> A5.
4. Blueprint built-ins -> A6.
5. Task engine and incremental behavior -> A7.
6. Template callbacks -> A8, B1.
7. Diagnostics/logging -> A9.
8. Standalone test executables -> B1, B3, B4.
9. Unit regression baseline -> B2.
10. Robustness and compatibility -> C1, C2.

Pass Criteria:
1. No feature family remains unmapped.

## 7. Traceability Anchors
Source anchors for planning and future test implementation:
1. yakka/yakka_cli.cpp
2. yakka/yakka_cli_actions.cpp
3. yakka/component_database.cpp
4. yakka/yakka_workspace.cpp
5. yakka/yakka_project.cpp
6. yakka/blueprint_commands.cpp
7. yakka/task_engine.cpp
8. yakka/utilities.cpp
9. test/workspace_unit_tests.cpp
10. test/data_dependency_unit_tests.cpp
11. yakka/test/component_database_test.cpp
12. test/inja_test.cpp
13. inja_test.cpp
14. perf_test/perf_test.cpp
15. yinja/yinja.cpp
16. docs/command_line.md
17. docs/task_engine.md
18. docs/templating.md

## 8. Completion Checklist
1. Two test types are explicit and complete.
2. Every section is independently runnable.
3. Every section includes objective, scope, preconditions, test cases, execution method, pass criteria, and debug notes.
4. Both functional and negative testing are included.
5. Gating vs non-gating behavior is defined.