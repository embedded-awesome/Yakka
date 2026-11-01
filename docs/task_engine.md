# Task Engine Design Document

## Overview
The taskflow engine is Yakka's default build engine; the Ninja engine described
[below](#ninja-engine) is selected with `--engine ninja`.
The Task Engine is a core component of Yakka that provides an abstraction layer over the TaskFlow C++ library. It manages the creation, scheduling, and execution of build tasks in a dependency-aware manner.

## Key Components

### `task_engine` Class
The main class responsible for managing the build process. It orchestrates task creation, dependency management, and execution flow.

### `construction_task` 
A structure that represents a single build task, containing:
- Task execution information
- Last modification timestamps
- Blueprint match data
- Task group information for progress tracking

### `task_engine_ui`
An interface for providing progress feedback during task execution, allowing for different UI implementations (like progress bars).

## Core Functions

### `init(task_complete_type task_complete_handler)`
Initializes the task engine with a completion handler callback.

### `create_tasks(const std::string target_name, tf::Task &parent, yakka::project &project)`
The heart of the task engine that:
- Creates task nodes for the dependency graph
- Handles both leaf nodes (files, data dependencies) and complex nodes (build targets)
- Manages task dependencies and relationships
- Sets up task execution callbacks

Key behaviors:
- Detects and handles data dependencies (targets starting with a special identifier)
- Manages file timestamps for change detection
- Creates task groups for progress tracking
- Sets up build command execution

### `run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project)`
Executes build commands for a target with:
- Template engine integration (using Inja)
- Support for both built-in and external tools
- Command output capture
- Error handling and reporting
- Performance timing

### `run_taskflow(yakka::project &project, task_engine_ui *ui)`
Orchestrates the entire build process:
1. Creates an executor with appropriate thread count
2. Sets up the task graph
3. Initializes progress tracking UI
4. Executes the task graph
5. Provides real-time progress updates

## Task Creation Process

1. **Dependency Check**
   - Checks if target was already processed
   - Establishes parent-child relationships for dependencies

2. **Task Type Determination**
   - Leaf nodes (files, data dependencies)
   - Complex nodes (build targets)

3. **Task Setup**
   - Associates data with tasks
   - Establishes execution callbacks
   - Sets up progress tracking

## Task Execution Model

The engine uses a pull-based model where:
- Tasks are executed when their dependencies are satisfied
- Timestamps are used for incremental builds
- Build abortion is possible through a shared flag

## Progress Tracking

- Tasks are grouped for logical organization
- Real-time progress updates
- Support for different UI implementations
- Counts of total and completed tasks per group

## Task Persistence

`save_todolist()` saves build targets and their successor relationships for
incremental builds. The internal no-op finish task is excluded by task identity,
not by its empty name. Other successors that cannot be resolved to a persisted
target still produce a warning, including unnamed tasks.

`save_taskflow()` saves the complete graph, including the internal finish task.

### Compiled file-check policy

Yakka defaults to **content hashes** for cached project inputs and todolist files.
Hash checks detect timestamp-preserving edits and ignore timestamp-only touches.
Hashes use the existing non-cryptographic project-diff hash; they are cache
fingerprints, not security checks.

Like git's index, a hash record also stores the file's modification time,
inode change time, size and inode once both timestamps are more than two seconds
old. A later check whose stat matches is accepted without reading the file; any
difference falls back to comparing the hash. The inode change time catches
same-size rewrites whose modification time is reset, and the two-second window
stops a write in the same timestamp tick from being trusted. Records written
inside the window, or whose content matched but stat differed, are recorded
again on the next save (the hash is reused, not recomputed), so a touched file
is hashed once rather than on every build.

To build Yakka with **timestamp equality** checks instead:

```sh
./yakka-macos link! yakka_cli xcode +timestamp_file_checks
```

Without `+timestamp_file_checks`, the build uses hashes. Other build systems can
set `YAKKA_FILE_CHECK_TIMESTAMP=1` for timestamps or `=0` for hashes. This is a
compile-time selection, not a runtime flag. Timestamp mode detects forward and
backward timestamp changes but cannot detect edits that preserve the timestamp.
Changing policy invalidates incompatible fingerprints.

### Warm-build fast path

The loaded summary remains a ryml tree. Before dependency evaluation, Yakka
checks the build request, workspace configuration and component databases, plus
fingerprints for component and additional-tool descriptors, the optional project
and workspace configuration files, and compiler dependency files. The saved
blueprint and target databases are Yakka's own outputs and are not inputs: a
missing one is regenerated. Files consulted by evaluation-time template helpers
(`file_exists`, `filesize`, `read_file`, and the data loaders) are tracked too.
`glob()` expansions are rescanned to detect additions and removals, not just edits
to previously matched files. Expected missing optional files are tracked too.
A matching cache skips component loading, dependency/choice evaluation, schema
validation, and summary regeneration.

If evaluation is needed, `component_source_cache` retains pristine parsed Yakka
component trees with their fingerprints. Unchanged descriptors are copied from
that cache rather than loaded and parsed again. Requirements and features are
applied to fresh copies, never to previously evaluated component data.
SLC projects conservatively reevaluate because their override discovery and
generated configuration files are not yet fully covered by this cache.

The todolist, rather than the diagnostic taskflow export, is authoritative for
incremental task reconstruction. Version 2 entries include file fingerprints.
Yakka scans the raw ryml entries before constructing task nodes and restores only
changed entries and their successor chains. An unchanged build with only
dependency-aggregating commands creates no task nodes or progress groups and does
not rewrite its summary, graph, or todolist. Commands with actions still run;
failed builds retry their tasks. Restored output tasks are explicitly marked
dirty so hash changes and backward timestamp changes cannot be suppressed by
later timestamp comparisons.
When the summary is regenerated, the full graph is created from the target
database, converted to a version 2 tree carrying the previous fingerprints by
target name, and then restored through the same scan, so only changed targets
and their successors become tasks. `taskflow.json` is written from the full graph.
Restored targets get one task per blueprint match, as when creating the graph.

The target database JSON writer explicitly quotes its string fields (target names,
paths, captures, requirements, and project arguments). This preserves strings such
as `"123"` and `"true"` and skips rapidyaml's numeric inference for those fields.
The format version and dependency types remain numbers; embedded blueprint
processes retain their existing serialization behavior.

Projects with project dependencies are restored too. Files inside a
sub-project's output folder are always restored, because the sub-project builds
after the parent is prepared: changed ones force their successors, and unchanged
ones leave their successors to the timestamp comparison when the graph runs.
Legacy version 1/2 todolists without fingerprints retain their timestamp-based
restore behavior and acquire fingerprints when saved. Missing caches fall back
to normal evaluation or graph construction; invalid entries produce diagnostics.

The version 2 todolist is indexed once as references to the raw ryml nodes.
Successor validation and reconstruction use constant-time vector bounds/index
lookups: ryml's `num_children()` and positional `child()` traverse siblings and
must not be called for each edge of a large graph. Template glob expansions are
checked once per summary, not once per fingerprinted input.

For repeatable library builds, put the archive process on its actual file target
and make the command a dependency-only entry point, for example:

```yaml
blueprints:
  librarify:
    depends: ['{{project_output}}/{{project_name}}.a']
  '{{project_output}}/{{project_name}}.a':
    depends: ['{{project_output}}/components/example/example.a']
    process:
      - archive: 'rcs {{$(0)}} {{project_output}}/components/example/example.a'
```

An action attached directly to `librarify` remains an explicit command and runs
on every invocation. Hash checks do not silently make such commands incremental.

### Taskflow no-op fast path

After a taskflow build in which every project reused its summary, ran no
commands, and succeeded, the CLI writes a binary record (`YKLF`) to
`output/.yakka/fast/<hash>`, keyed like the Ninja record and including
`--engine`. It lists the stat of every summary input, todolist fingerprint,
component database, and Yakka state file (summary, blueprints, target database,
todolist), plus the template glob expansions. An identical later invocation
stats those files and rescans the globs before logging or workspace setup; if
all match it reports success, otherwise the slow path runs. Inputs changed less
than two seconds earlier are not recorded, so the record usually appears on the
second no-op after a change. As with the Ninja record, `yakka.log` is not
rewritten by fast-path invocations.

## Ninja Engine

The Ninja engine (`--engine ninja`) translates the target database into a Ninja manifest and runs
an embedded copy of Ninja (`components/ninja`, v1.13.1) inside the yakka binary.
Ninja's dirty check (one `stat` per node, the binary `.ninja_log` and `.ninja_deps`
files, and command hashes) replaces the todolist scan for incremental builds.
The taskflow engine described above is the default; `--engine taskflow` selects it
explicitly, and `--engine legacy` is accepted as its former name. The Ninja engine
exists for comparison when optimising the taskflow engine, not as an interchangeable
mode: each engine keeps its own build state, so switching engines on the same output
folder is expected to rebuild the project. Ninja also consumes the compiler `.d` files,
so the first taskflow builds after a Ninja build may rebuild again.

### Generated files

`output/<project>/build.ninja` is written by `ninja_generator` and is only rewritten
when its content changes. Ninja state lives next to it in `.ninja_log` and
`.ninja_deps`. Targets are classified as follows:

| Kind | Condition | Ninja representation |
|------|-----------|----------------------|
| Source | Leaf file that exists | Plain input |
| Generated | Every step is `inja`, `template`, `save`, `regex`, `as_json`, `as_yaml`, `cat` or `create_directory`, and all inputs are static | Run at generation time; listed as an implicit output of the regeneration edge |
| Shell | Steps are tools, `execute`/`shell` or `create_directory`, renderable without evaluating other built-ins | `shell` rule with the rendered command; `shell_deps` (`deps = gcc`) when the blueprint declares a `dependency_file` |
| Yakka | Anything else | `yakka` rule running `yakka --ninja-exec <target> <request>`, with `restat = 1` |
| Phony | Blueprint with no process | `phony` edge |

Data dependencies are hashed into a trailing shell comment on the command, so a
data change changes the command hash and Ninja reruns the edge.

The manifest regenerates itself through a `generator = 1`, `restat = 1` edge whose
inputs are the project's `.yakka` files, component databases, `config.yaml`,
template files, glob directories and the inputs of generation-time targets. When
any of them changes, Ninja calls `yakka --ninja-regenerate <request>`, which
re-evaluates the project, rewrites only the generated files that changed, and
reloads the manifest before building. Unchanged generated files keep their
timestamps, so editing a comment in a `.yakka` file rebuilds nothing.

### No-op fast path

After a successful slow-path build, the CLI records the manifests for the request
in `output/.yakka/fast/<hash>`. The hash covers the yakka build, the working
directory and the arguments (excluding `--jobs`). An identical later invocation
reads the record and runs Ninja directly, before logging setup and workspace
initialisation. Ninja's regeneration edge keeps the record honest: if any project
input changed, the manifest is regenerated in-process before the build. If a
manifest cannot be loaded or regenerated, the full slow path runs instead.
`yakka.log` is not rewritten by fast-path invocations.

### Multi-project builds

Requests that select several projects (for example `'[p1, p2]'`) bring every
manifest up to date one at a time, because regeneration runs in Ninja's console
pool and can rewrite shared inputs. The builds then run concurrently, one Ninja
builder per project on its own thread, each with its own `.ninja_log` and
`.ninja_deps`. They share:

- a single `--jobs` budget. Each builder reserves job slots only for edges that are
  ready to run, and a builder with nothing running takes priority for the next free slot;
- one progress display, so `[finished/total]` counts edges across all projects and
  messages such as `no work to do` are prefixed with the project name.

A failure in one project does not stop the others. Ctrl-C interrupts every build.
The fast path also builds multi-project records concurrently; if any of their
manifests cannot be loaded or regenerated, nothing is built and the slow path runs.
Ninja's process-wide console pool and signal handlers are not thread-safe, so
concurrent builds run console-pool edges in the default pool and the original
signal handlers are restored when all builds finish.

### Project dependencies

Each sub-project named by a `project` dependency gets its own manifest, prepared
before its parent's, with the parent's sub-project names recorded in the
manifest's `yakka_projects` variable. Sub-project builds run in the same
concurrent session as other projects, and a parent's builder starts only after
all of its sub-projects build successfully; a failed sub-project fails the parent
with `project dependency '<name>' failed`. The fast-path record stores this
dependency graph. If regeneration changes a parent's `yakka_projects`, the
record is stale and the slow path runs instead. As with the taskflow engine, list
sub-project artifacts as file dependencies so their changes trigger rebuilding.

### Fallbacks and limitations

- Projects with SLC (`.slcc`) inputs use the taskflow engine.
- Library callers that do not set `build_options::yakka_binary` use the taskflow engine,
  because Ninja edges re-invoke the yakka executable.
- `--ninja-exec` edges re-evaluate the project for each invocation (about 100 ms),
  and a `save` to `:/data` inside one is not visible to other edges.
- Glob additions and removals are detected through directory timestamps.
- Only the first dependency file of a blueprint is tracked.
- When one project of a multi-project build runs out of ready work, the slots it
  frees are picked up by another project when one of that project's running
  commands finishes, so up to one command duration per project can be left idle.
- Windows is not supported yet; shell rendering assumes a POSIX shell.

## Error Handling

- Command execution status tracking
- Build abortion on critical errors
- Error logging and reporting
- Exception handling for template processing

## Performance Considerations

- Parallel execution through TaskFlow
- Thread count optimization
- Timestamp-based incremental builds
- Progress tracking with minimal overhead

## Integration Points

- Project configuration
- Blueprint system
- Template engine
- Command execution
- UI feedback system

## Usage Example

```cpp
yakka::task_engine engine;
progress_bar_task_ui ui;

// Initialize engine
engine.init(completion_handler);

// Configure project
yakka::project project(...);

// Execute build
engine.run_taskflow(project, &ui);
```

## Future Considerations

- Enhanced error reporting
- More granular progress tracking
- Extended template capabilities
- Additional built-in commands
- Performance optimizations for large builds
