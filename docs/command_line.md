# Command line interface

The Yakka CLI operates by executing commands on a list of components and features.
Yakka has a minimal set of built-in commands that are reserved for it internal use
- `build`
- `register`
- `list`
- `update`
- `benchmark`


The first argument provided to Yakka is assumed to be a command unless it ends with a `!` to indicate that is references a [blueprint](blueprints).
The remaining arguments are interpreted as component names but can be features if prefixed with `+` such as `+feature` or can be blueprints if suffixed with `!` such as `compile!`.
There can be any number of features or blueprints provided via the command line.

## Build engines

By default Yakka builds with its taskflow engine, which keeps incremental state in
`output/<project>/todolist.json`. Repeating an identical command after an
unchanged build skips workspace initialisation entirely. See
[the task engine](task_engine.md) for details.

With `--engine ninja`, Yakka instead generates `output/<project>/build.ninja` and
runs an embedded Ninja to build it; an unchanged build reports
`ninja: no work to do`. See [the Ninja engine](task_engine.md#ninja-engine).

- `--engine taskflow|ninja`: choose the build engine (default `taskflow`;
  `legacy` is accepted as an alias for `taskflow`).
- `--ninja-regenerate` and `--ninja-exec <target>`: internal options used by
  generated manifests. They are not intended to be run by hand.

The generated manifest can be inspected with a standalone Ninja, for example
`ninja -f output/<project>/build.ninja -n -d explain`.

## Dependency comparison benchmark

After building a project, run this built-in action from the same workspace root:

```sh
./yakka-macos benchmark yakka_cli-xcode
```

Use the saved output directory's project name (including any `--project-name`
override), not component names. The command reads
`output/<project-name>/target_database.json` and GCC/Clang `.d` files under that
project's output directory. Reading the `.d` files includes headers discovered
during the first build, even if they are not yet in the saved target database.
Only the named project's saved graph is inspected; separate child projects must
be benchmarked individually. Rebuild first if the build configuration has changed.

Each unique, lexically normalized absolute file path is checked once per method.
Existing file targets, generated inputs, source files, headers, and `.d` files
are included. Data dependencies are excluded; directories and nonexistent
blueprint targets (such as phony commands) are skipped and counted.
Missing leaf dependencies, unreadable files, missing/invalid saved state, and
projects with no files produce a diagnostic and a nonzero exit code.

The command captures timestamps and content hashes in memory, then separately
times one timestamp-read-and-compare pass and one full-file-read-and-hash-compare
pass using the same files. It reuses Yakka's `rapidhash` content hashing.
Each result reports the number of comparisons, total milliseconds, microseconds
per file, and changes since the in-memory baseline. Normally both change counts
are zero; this is a performance benchmark, not a comparison against the last build.
Discovery and baseline capture are excluded from the timings.

These are **warm-cache** measurements: baseline capture has already read every
file. Results include file I/O and comparison costs, not just hash arithmetic;
OS caching and concurrent filesystem activity affect timings. No build commands
are executed and no project state is updated (normal CLI logging still occurs).

## Multiple Projects

Bracket arguments select alternatives rather than adding every entry to the same project:

```sh
yakka link! my_app '[platform1, platform2]' gcc_arm
yakka link! my_app '[platform1, platform2]' '[+debug, +release]' gcc_arm --jobs 4
```

The first invocation builds two projects. The second builds all four combinations.
Quote bracket arguments so the shell does not interpret them as filename patterns.
Each nonempty, flat sequence may contain component names or `+feature` names.
Commands ending in `!` cannot be alternatives. Identical alternatives are deduplicated;
malformed sequences and conflicting output names fail before project initialization.
Matrices are limited to 4096 expanded projects.

Each project evaluates its components and features independently and keeps its own
summary, target database, task graph, and incremental state in `output/<project-name>/`.
Independent projects execute on one worker pool. `-j` or `--jobs` sets the shared worker
count; the default is the number of hardware threads, capped at 32 and never below one.
This limit does not constrain threads started by external tools.

Automatic names retain the existing component/feature convention. With multiple
projects, `--project-name custom` becomes a prefix followed by each selected alternative:
`custom-platform1+debug`, for example. With one project, the override remains exact.
Different projects cannot claim the same generated file target.

All independent projects are attempted even if one fails. The CLI reports a result for
each project and exits nonzero if any preparation or task execution fails. Failed tasks
remain eligible for retry on the next invocation.

Declarative blueprint dependencies that build isolated child projects are not yet
implemented. CLI matrices currently support independent projects only.

