# Blueprints

Blueprints map targets to dependencies and an ordered process. A component declares
them under `blueprints`. Invoke a command target with a trailing `!`, for example:

```sh
./yakka-macos link! unit_tests xcode
```

## Definition and matching

```yaml
blueprints:
  build:
    group: Build
    requires: [compiler]
    depends: ['{{ project_output }}/main.o']
  object_files:
    regex: '{{ project_output }}/(.+)\.o'
    depends:
      - '{{ $(1) }}.cpp'
      - data: /components/*/flags
      - dependency_file: '{{ $(0) }}.d'
    process:
      - echo: 'Compiling {{ $(1) }}'
      - compiler: '-c {{ $(1) }}.cpp -o {{ $(0) }}'
```

All fields are optional, but a definition must contain at least one supported
field. `depends` and `process` are sequences; `requires` is a sequence of component
names supplying additional tools; `group` labels tasks. `regex` is a string.

Without `regex`, the blueprint key is rendered as an Inja template and matched
exactly. With `regex`, the expression is rendered and matched against the **whole**
target, not a substring. Multiple blueprints can provide the same target.
`$(0)` is the complete target; `$(1)` and later indices are regex captures.
Dependency-only blueprints are useful as command/grouping targets.

Data-producing targets must start with `:/data/`; targets under `:/components/`
are rejected. Empty rendered target names are also rejected.

## Dependencies

Dependencies may be:

- A filename or another blueprint target, rendered using the project summary.
- A rendered list string, such as `'[first.cpp, second.cpp,]'`. A trailing comma
  is accepted. Empty strings and empty lists contribute no file dependency.
- A colon-prefixed data pointer, such as `:/data/options`.
- A map with `data`, whose value is a pointer string or sequence of pointers.
  The colon prefix is added automatically.
- A map with `dependency_file`, naming a rendered GCC-style `.d` file.

Data pointers use slash-separated paths, not the old dot notation:

```yaml
depends:
  - data:
      - /components/*/flags/c/global
      - /components/{{ $(1) }}/includes/local
      - /data/options
```

The component wildcard tracks additions, removals, and changes across components.
Leading `./` is removed from file dependencies. GCC dependency parsing handles
same-line prerequisites, backslash-newline continuations, CRLF, and escaped spaces.
A missing dependency file contributes no prerequisites, as is needed before the
first compilation.

## Processes, tools, and callbacks

Each process entry is a map containing exactly one command. Commands run in order,
passing captured output to the next command. A failing command stops the process.
A malformed process container or entry is an error.

Components can provide external tools:

```yaml
tools:
  compiler: clang++
  checker:
    command: checker
    return_value_success: [0, 7]
```

A tool entry may be a scalar command or a map with `command`. The optional
`return_value_success` accepts an integer or sequence of integers. An accepted
nonzero exit code is normalized to success; other codes stop execution.

Blueprint-specific template callbacks:

| Callback | Meaning |
|---|---|
| `$(n)` | Target/regex capture; indices outside the available captures are invalid. |
| `curdir()` | Directory of the component providing the blueprint. |
| `render(text)` | Render another template with the project summary. |
| `render(text, directory)` | Execution-only variant with a temporary `curdir()`; the previous directory is restored. |
| `aggregate("/path")` | Combine matching component values followed by project data. |
| `load_component(name)` | Execution-only callback loading component data into template-owned storage. |

Aggregation merges maps recursively, with project data applied last. Sequences
are concatenated, scalars become sequence entries, and scalar entries are rendered
as templates. Nested maps/sequences are retained. Missing paths produce an empty
sequence. Mixing map and sequence/scalar contributions is an error. The same
aggregation implementation is used for dependency and process templates.

Process templates also support Inja includes; included files are parsed as
templates and can use blueprint callbacks.

## Built-in commands

| Command | Input and behavior |
|---|---|
| `echo` | Render/log a scalar; an empty argument logs and retains captured output. |
| `execute` | Render and execute a command, returning its output and exit code. |
| `shell` | Execute using the platform shell behavior. |
| `inja` | Render a scalar template using the project summary. |
| `template` | Scalar template, or map with `template`/`template_file` and optional scalar YAML `data`/`data_file`. Missing data files fail. |
| `regex` | Map with `search` and `replace`, `match`, or `to_yaml`. Supports `prefix`, `suffix`, and linewise `split`. Match templates use `reg(n)` for captures. Zero-length matches advance safely. |
| `save` | Write captured bytes to a rendered filename. An empty argument uses the target. `:/data/path` writes project data, creating missing nodes. Other data target prefixes fail. |
| `create_directory` | Create the rendered directory, including parents. Existing directories are accepted. |
| `verify` | Require the rendered filesystem path to exist. |
| `rm` | Remove a rendered path or rendered YAML list of paths; missing paths are tolerated. |
| `rmdir` | Recursively remove the rendered directory. |
| `pack` | Append packed integer bytes to captured output; see below. |
| `copy` | Copy scalar, sequence, or categorized sources; see below. |
| `cat` | Read a rendered file as binary captured output; missing files fail. |
| `as_json` | Parse captured YAML and emit JSON. |
| `as_yaml` | Parse captured JSON/YAML and emit YAML. |
| `diff` | Currently validates/loads inputs but does not generate a patch. |

### Packing

```yaml
- pack:
    format: CcSsLlx
    data: [1, -1, 258, -2, 0x01020304, -3, 0]
```

`C/c`, `S/s`, and `L/l` encode unsigned/signed 8-, 16-, and 32-bit integers.
`x` writes a zero padding byte and consumes one data entry. There must be exactly
one value per format character. Unknown codes, incomplete numeric conversions,
and values outside the selected integer range fail. Decimal and `0x` hexadecimal
values are accepted. Multi-byte values use **native host byte order**, not a
portable/network byte order.

### Copying

```yaml
- copy:
    source:
      files: [input.txt]
      file_paths: [nested/input.txt]
      folders: [assets]
      folder_paths: [nested/assets]
    destination: output
```

`source` may instead be a filename or sequence of filenames. `yaml_list` is an
alternative scalar template expanding to a YAML list. `files` and `folders` copy
to the destination; `file_paths` and `folder_paths` append the source path beneath
the destination, creating directories. Copying is recursive for directories and
updates existing files. Missing source/destination arguments and filesystem
failures return errors.

## Persistence and known limitations

Blueprint persistence retains targets, regexes, dependency types, parent
directories, requirements, task groups, and process definitions. Restored
definitions own their YAML storage and can be matched repeatedly. Target
persistence retains resolved dependencies, captures, executable definitions, and
cached misses; absent regexes remain absent.

- Dependency `select()` is a stub returning no value; it contributes no dependency.
- `diff` accepts `left`/`right` scalar YAML or `left_file`/`right_file`, but currently
  returns empty output for valid inputs. It must not be used as a change detector.
- General template rendering helpers log rendering errors and may return empty
  text; empty output alone is not evidence that rendering succeeded.

## Regression tests

| Surface | Tests |
|---|---|
| Definition fields, dependency forms, exact/regex matching, aggregation, GCC files, persistence/cache | `BlueprintTest` in [blueprint_tests.cpp](../test/blueprint_tests.cpp) |
| Pipelines, failure short-circuiting, tool codes, callbacks, includes, malformed processes | `BlueprintExecutionTest` in [blueprint_tests.cpp](../test/blueprint_tests.cpp) |
| Project target rendering, data restrictions, graph traversal, required tools | `BlueprintProjectTest` in [blueprint_tests.cpp](../test/blueprint_tests.cpp) |
| Supported schema fields and rejected types | `BlueprintSchemaTest` in [blueprint_tests.cpp](../test/blueprint_tests.cpp) |
| Every registered built-in command, options, binary output, and error cases | [blueprint_command_tests.cpp](../test/blueprint_command_tests.cpp) |
| Wildcard data changes and removed paths | [data_dependency_unit_tests.cpp](../test/data_dependency_unit_tests.cpp) |
| End-to-end builds, regex command targets, cycles, warm rebuilds and failures | [project_lifecycle_tests.cpp](../test/project_lifecycle_tests.cpp) |

Build the [unit_tests component](../test/unit_tests.yakka), then run:

```sh
./output/unit_tests-xcode/unit_tests-xcode --gtest_filter='Blueprint*'
./output/unit_tests-xcode/unit_tests-xcode
```

These are feature/regression tests, not a claim of 100% line or branch coverage.
