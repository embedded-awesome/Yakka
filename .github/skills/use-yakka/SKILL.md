---
name: use-yakka
description: 'Use when working with Yakka build files, .yakka components, Yakka commands, link! builds, generated output folders, project summaries, blueprints, taskflow output, or yakka.log diagnostics.'
argument-hint: 'What do you want to build, inspect, or troubleshoot with Yakka?'
---

# Use Yakka

Use this skill to explain, run, or troubleshoot Yakka projects in this workspace.

## Core Model

- Yakka is a data-first build system for embedded software.
- A `.yakka` file defines a component: its name, sources, includes, dependencies, features, and optionally blueprints.
- A workspace is the directory Yakka scans for components and configuration.
- A project is the evaluated set of selected components, required features, choices, data, blueprints, and generated tasks for one command invocation.
- A blueprint is a named generation or transformation step. Command-line actions ending in `!` invoke blueprints rather than built-in CLI actions.

## Command Shape

Use the platform binary from the workspace root:

```bash
./yakka-macos <command-or-blueprint> [components-or-features]
```

On macOS, the usual application generation command is:

```bash
./yakka-macos link! <application-component> <toolchain-component>
```

Examples:

```bash
./yakka-macos link! yakka_cli xcode
./yakka-macos link! perf_test xcode
```

Treat `link!` as the build/generate command for producing an application. The trailing `!` matters: it tells Yakka to run the `link` blueprint instead of looking for a built-in action named `link`.

## Reading `.yakka` Files

When explaining or modifying a `.yakka` file:

1. Start with the component `name`.
2. Read `sources` to see what files become part of the component.
3. Read `includes` to understand exported or local include paths.
4. Read `requires` to identify required components and features.
5. Read `provides`, `supports`, and `choices` when dependency evaluation or feature selection is involved.
6. Read `blueprints` when the task is about generated files, build steps, commands, or templates.

Do not assume a `.yakka` file is only a build target. It may describe an application, library, toolchain, feature provider, data source, or blueprint provider.

## Build Workflow

For a normal generation/build request:

1. Identify the application component and toolchain component.
2. Prefer an existing workspace task if one matches the request.
3. Otherwise run the corresponding Yakka command from the workspace root.
4. After the run, inspect the output directory named from the selected project and components, commonly under `output/`.
5. Use generated summaries and task files to explain what Yakka resolved and planned.
6. Use `yakka.log` to explain what Yakka actually did.

Useful generated files include:

- `output/<project>/project_summary.json`: evaluated components, features, choices, and data.
- `output/<project>/blueprints.json`: generated blueprint database.
- `output/<project>/target_database.json`: generated target database.
- `output/<project>/taskflow.json`: generated task graph.
- `output/<project>/todolist.json`: task execution state for resumable builds.
- `output/<project>/build.ninja`: generated Ninja manifest used by `--engine ninja`; `.ninja_log` and `.ninja_deps` beside it hold Ninja's build state.
- `yakka.log`: detailed log for the most recent Yakka action.

## Diagnostics

When a Yakka command fails or behaves unexpectedly:

1. Read the terminal error first for the high-level failure.
2. Open `yakka.log` for the detailed trace of the most recent run.
3. Check whether the requested component names match `name` fields in `.yakka` files.
4. Check missing component errors against `requires.components` and the component database.
5. Check unresolved features against `requires.features`, `provides.features`, `supports`, and any CLI `+feature` arguments.
6. Check incomplete or multiple choices in `project_summary.json` and the relevant component choice definitions.
7. Check blueprint issues in `blueprints.json`, template files, and the component that provides the blueprint.
8. Check task execution problems in `taskflow.json`, `todolist.json`, and the command logged in `yakka.log`.

Common options:

- `-f` or `--fetch`: automatically fetch missing components.
- `-r` or `--refresh`: rescan component databases.
- `-n` or `--no-eval`: skip dependency and choice evaluation.
- `-i` or `--ignore-eval`: continue despite dependency or choice evaluation errors.
- `-p` or `--project-name`: override the generated project name.
- `-d` or `--data`: inject additional project data before blueprint processing.
- `--no-slcc`: ignore SLC files.
- `--no-yakka`: ignore Yakka files.
- `--engine ninja`: use the embedded Ninja engine instead of the default taskflow engine (`--engine taskflow`).

With the Ninja engine, use `ninja -f output/<project>/build.ninja -n -d explain` to see why a target would rebuild. With either engine, repeated identical no-op commands take a fast path that skips `yakka.log`; delete `output/.yakka/fast` to get a full trace.

## Completion Checks

A Yakka task is handled when:

- The command syntax is explicit, including whether a blueprint needs `!`.
- The relevant `.yakka` component files have been identified.
- The generated `output/` location is known or inspected.
- `yakka.log` has been used for post-run explanation or troubleshooting when behavior is unclear.
- Any explanation distinguishes requested components, features, blueprints, and built-in CLI actions.