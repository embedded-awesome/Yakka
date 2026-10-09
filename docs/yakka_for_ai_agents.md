# Yakka Build System — Reference for AI Agents

This document describes the Yakka build system for use by AI agents that need to understand, create, or modify Yakka projects and components.

---

## What is Yakka?

Yakka is a component-based build system and development tool designed primarily for embedded software. It uses YAML (or TOML) descriptor files, Inja/Jinja-like templating, and regular expressions to describe software components, their dependencies, and how to build them.

The name comes from Australian slang for "work".

---

## Core Concepts

### Component
A component is the fundamental unit in Yakka. It can represent:
- A software library or source module
- A toolchain (compiler, linker, etc.)
- A meta-component that groups functionality
- A tool or generator

A component is defined by a `.yakka` file (YAML format) or a `.yakka.toml` file (TOML format). The file is typically located in the component's source directory.

### Feature
A feature is a named concept that modifies a component's behaviour. When a feature is applied to a component, its YAML data is merged into the parent component. Features allow a single component file to cover multiple configurations without splitting into sub-components.

### Blueprint
A blueprint is analogous to a Makefile recipe. It maps a build **target** to its **dependencies** and a **process** (list of commands) to produce the target. Targets can be:
- Plain strings (e.g. `compile`, `link`)
- Inja template strings (e.g. `{{project_output}}/{{project_name}}`)
- Regular expressions with capture groups

### Workspace
A workspace is a directory that acts as the root for Yakka operations. It may contain a `config.yaml` and a `.yakka/` folder for caching component databases.

### Project
A project is an instantiation of a set of components and features to be built. Multiple projects can exist in a single workspace. The project evaluation process resolves all dependencies and collects all required data before running blueprints.

---

## .yakka File Structure

`.yakka` files are YAML documents. A `.yakka.toml` file contains the same structure in TOML format.

### Reserved Top-Level Keys

| Key | Description |
|-----|-------------|
| `name` | **Required.** Human-readable component name. Used for build output naming. |
| `type` | Optional. Component type (e.g. `application`). |
| `description` | Optional. Human-readable description. |
| `version` | Optional. Semver version string. |
| `requires` | Dependencies that **must** be present. |
| `supports` | Optional dependencies that activate only when present. |
| `provides` | Features or capabilities this component fulfills. |
| `features` | List of features defined within this component. |
| `blueprints` | Build recipes defined by this component. |
| `tools` | Named tool binaries provided by this component (e.g. compilers). |
| `choices` | Interactive configuration choices exposed to the user. |
| `replaces` | Declares that this component replaces another. |

Any additional top-level keys are component-specific data (e.g. `sources`, `includes`, `defines`, `flags`). These are consumed by blueprints via templating.

### `name`

```yaml
name: My Component
```

Forbidden characters in names: `/ < > : " \ | ? *`, ASCII control characters 0–31. Names cannot end in a space or dot.

### `requires`

Declares hard dependencies. The build fails if these cannot be resolved.

```yaml
requires:
  components:
    - yaml-cpp
    - spdlog
  features:
    - cpp23
```

### `supports`

Declares optional dependencies. If the listed component or feature is present in the project, the nested YAML data is merged into this component.

```yaml
supports:
  components:
    gcc:
      flags:
        cpp:
          global:
            - -Wno-error=sign-compare
    xcode:
      flags:
        cpp:
          global:
            - -Wno-deprecated-declarations
```

### `provides`

Indicates that this component fulfills a named feature.

```yaml
provides:
  - feature_name
```

### `features`

Defines named sub-configurations within the component. When a feature is activated, its data is merged into the component.

```yaml
features:
  - name: test
    description: "Enable test mode"
    app:
      min_stack: 200
  - name: optimized
    flags:
      cpp:
        global:
          - -O3
```

### `blueprints`

Defines build recipes. Each blueprint has a target key (string, template, or regex), optional `depends`, and optional `process`.

```yaml
blueprints:
  # Simple string target
  link:
    depends:
      - '{{project_output}}/{{project_name}}'

  # Template string target
  '{{project_output}}/{{project_name}}':
    depends:
      - '[{% for name, component in components %}{% for source in component.sources %}{{project_output}}/components/{{name}}/{{source}}.o, {% endfor %}{% endfor %}]'
    process:
      - execute: 'clang++ -o {{$(0)}} @options_file'

  # Regex target — capture groups accessed as $(1), $(2), etc.
  # $(0) is the full matched target string
  object_files:
    regex: '.+/components/([^/]*)/(.*)\.(cpp|c)\.o'
    depends:
      - '{{at(components, $(1)).directory}}/{{$(2)}}.{{$(3)}}'
    process:
      - create_directory: '{{$(0)}}'
      - execute: 'clang++ -c -o {{$(0)}} {{at(components, $(1)).directory}}/{{$(2)}}.{{$(3)}}'
```

#### Dependency list expansion

Wrapping a dependency entry in `[ ]` causes it to be expanded as a YAML sequence. A trailing comma inside the expansion is tolerated.

```yaml
depends:
  - '[{% for name, component in components %}{% for source in component.sources %}{{project_output}}/components/{{name}}/{{source}}.o, {% endfor %}{% endfor %}]'
```

#### Data dependencies

Blueprints can depend on specific data paths within component files. Yakka tracks whether those data entries changed since the last build.

```yaml
depends:
  - data:
      - '*.flags.c.global'      # wildcard: any component
      - '/my_component/defines' # specific component
```

### `tools`

Named tool binaries exposed to blueprints via the `tools` map.

```yaml
tools:
  clang++: clang++
  clang_c: clang
```

In blueprints, tools are referenced as `{{tools.clang++}}` or `{{at(tools, "clang++")}}`.

A tool entry can also be a map with `command` and `return_value_success` to restrict which process exit codes count as success:

```yaml
tools:
  clang_c:
    command: clang
    return_value_success: [0]
```

If `return_value_success` is present and the tool's exit code isn't in the list, the blueprint step is treated as a failure and the taskflow engine aborts further processing for that build.

---

## Common Component Data Keys (C/C++ Extension)

These are not reserved by Yakka itself but are used by convention in C/C++ toolchain blueprints:

| Key | Description |
|-----|-------------|
| `sources` | List of source file paths relative to the component directory |
| `includes.global` | Include paths exposed to all dependent components |
| `includes.local` | Include paths used only within this component |
| `defines.global` | Preprocessor defines exposed globally |
| `defines.local` | Preprocessor defines local to this component |
| `flags.cpp.global` | C++ compiler flags exposed globally |
| `flags.cpp.local` | C++ compiler flags local to this component |
| `flags.c.global` / `flags.c.local` | C compiler flags |
| `flags.ld.global` | Linker flags |
| `flags.S.global` / `flags.S.local` | Assembler flags |

---

## Templating in .yakka Files

Blueprint targets, `depends`, and `process` entries are evaluated using the Inja template engine (Jinja-like syntax).

### Key Template Variables

| Variable | Description |
|----------|-------------|
| `project_name` | The name of the current project |
| `project_output` | Output directory path for the current project |
| `components` | Map of all resolved components (key = component ID) |
| `features` | List of all active features |
| `tools` | Map of all tool binaries available to the project |
| `configuration` | Workspace/project configuration data |

### Regex Capture Groups

In regex blueprint targets, capture groups are accessed as `$(0)` (full match), `$(1)`, `$(2)`, etc.

### Yakka-Specific Template Functions

In addition to standard Inja functions:

**String manipulation:** `snake_case()`, `camel_case()`, `pascal_case()`, `kebab_case()`, `upper_case()`, `lower_case()`

**Path operations:** `dir()`, `not_dir()`, `parent_path()`, `absolute_path()`, `relative_path()`, `extension()`, `base_name()`, `dir_name()`

**File operations:** `read_file()`, `load_yaml()`, `load_json()`, `file_exists()`, `filesize()`

**Collection operations:** `join()`, `concat()`, `split()`, `glob()`

**Other:** `at(map, key)`, `quote()`, `replace()`, `hex2dec()`

---

## Blueprint Process Commands

The `process` sequence in a blueprint is a list of commands to execute:

| Command | Description |
|---------|-------------|
| `execute: <shell command>` | Run a shell command |
| `inja: <template string>` | Render an Inja template to a string buffer |
| `inja: { template_file: path }` | Render an Inja template file to buffer |
| `template: { template_file: path }` | Alias for inja with a file |
| `save: [optional path]` | Save the current buffer to a file (defaults to target path) |
| `echo: <message>` | Print a message |
| `create_directory: <path>` | Create a directory (and parents) |
| `rm: <path>` | Remove a file |
| `rmdir: <path>` | Remove a directory |
| `copy: { source: ..., destination: ... }` | Copy files or directories |
| `cat: <path>` | Concatenate file content into buffer |
| `pack: ...` | Pack files into an archive |
| `verify: ...` | Verify a condition |
| `regex: ...` | Apply a regex substitution |
| `<tool_name>: <args>` | Invoke a named tool from the `tools` map |

---

## CLI Usage

```
yakka-macos <command> <components...> [+feature...] [blueprint!...]
```

- The first argument is a **command** unless it ends with `!` (which makes it a blueprint invocation).
- Components are referenced by their ID (typically their `.yakka` filename without extension).
- Features are prefixed with `+` (e.g. `+rtos`).
- Blueprints are suffixed with `!` (e.g. `link!`, `compile!`).

### Built-in Commands

| Command | Description |
|---------|-------------|
| `build` | Build the project |
| `register` | Register components in the workspace database |
| `list` | List available components/features |
| `update` | Update component database |

### Examples

```sh
# Build the yakka_cli application using the xcode toolchain on macOS
./yakka-macos link! yakka_cli xcode

# Build with a feature enabled
./yakka-macos link! my_app gcc +rtos

# Run a named blueprint
./yakka-macos cmake! my_project gcc
```

---

## Component Database

Yakka maintains a JSON component database (stored in `.yakka/`) that caches discovered component locations, blueprints, features, and type information. The database is scanned by recursively searching workspace directories for `.yakka`, `.slcc`, `.slcp`, and `.slce` files.

Database structure:
```json
{
  "blueprints": {},
  "components": {},
  "features": {},
  "types": {}
}
```

---

## File Format Variants

| Extension | Format | Notes |
|-----------|--------|-------|
| `.yakka` | YAML | Primary format |
| `.yakka.toml` | TOML | Equivalent TOML representation |
| `.slcc` | YAML | Silicon Labs Component Catalog format (also supported) |

---

## Minimal Component Example

```yaml
name: My Library

sources:
  - my_lib.cpp

includes:
  global:
    - include/

defines:
  global:
    - MY_LIB_ENABLED

requires:
  components:
    - json
    - spdlog
```

## Application Component Example

```yaml
name: My Application
type: application
version: 1.0.0

sources:
  - main.cpp
  - app.cpp

requires:
  components:
    - my_lib
    - xcode        # toolchain

supports:
  components:
    gcc:
      flags:
        cpp:
          global:
            - -Wno-unused-parameter
```

## Toolchain Component Example

```yaml
name: My Toolchain

tools:
  compiler: g++
  c_compiler: gcc

flags:
  cpp:
    global:
      - -std=c++23
      - -O2

blueprints:
  link:
    depends:
      - '{{project_output}}/{{project_name}}'

  '{{project_output}}/{{project_name}}':
    depends:
      - '[{% for name, component in components %}{% if existsIn(component,"sources") %}{% for source in component.sources %}{{project_output}}/components/{{name}}/{{source}}.o, {% endfor %}{% endif %}{% endfor %}]'
    process:
      - execute: '{{tools.compiler}} -o {{$(0)}} @{{project_output}}/{{project_name}}.global_ld_options'

  cpp_object_files:
    regex: '.+/components/([^/]*)/(.*)\.(cpp|cc)\.o'
    depends:
      - '{{at(components, $(1)).directory}}/{{$(2)}}.{{$(3)}}'
    process:
      - create_directory: '{{$(0)}}'
      - execute: '{{tools.compiler}} -c -o {{$(0)}} {{at(components, $(1)).directory}}/{{$(2)}}.{{$(3)}}'
```

---

## Project Evaluation Flow

1. CLI creates a project with an initial list of components and features.
2. Project evaluates itself — resolves dependencies, identifies missing components or unresolved choices.
3. CLI inspects the project state:
   - Missing components → fetch from registry (if enabled) or error.
   - Missing choices → prompt user.
4. Repeat evaluation after any resolution actions.
5. Once fully resolved, the task engine creates a dependency graph and executes blueprints in parallel using TaskFlow.

---

## Key Directories

| Path | Contents |
|------|----------|
| `/yakka/` | Core Yakka C++ source files |
| `/components/` | External library components (each has a `.yakka` file) |
| `/tools/` | Toolchain `.yakka` files (gcc, clang, xcode, msvc, cmake) |
| `/test/` | Unit test components and test data |
| `output/<project>-<toolchain>/` | Build outputs for a given project+toolchain combination |
| `.yakka/` | Workspace cache (component database JSON) |
