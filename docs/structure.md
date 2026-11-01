# Yakka code and object structure

Yakka has a number of key concepts; workspace, project, component, blueprints, and actions.

The workspace represents a directory path that acts as the root for Yakka actions. It can hold a config file (config.yaml), the .yakka folder, and acts as the base for finding other components.

A project represents a particular set of components, and features that operate from within a workspace.
An instantiation of Yakka may involve multiple projects.
Projects generally require an evaluation process that identifies dependencies and attempts to resolve them. The evaluation process can be interactive whereby the project owner (typically the Yakka CLI) can perform actions such as downloading missing components or prompting the user for addtional information.


# Project evaluation process

- CLI creates project. Provides initial set of components and features
- CLI gets project to evaluate itself.
- Project sets internal state, lists any missing components, features or choices.
- CLI inspects project and reacts.
  - If missing components and user has indicated to download, request workspace to fetch components
  - If missing choices, prompt user to make a choice
- If any actions have occurred in response to project state, get project to evaluate

## Component features

A component can declare build features in a top-level, name-keyed `features` map:

```yaml
name: platform
config:
  logging: true
features:
  platform_available: {}
  logging:
    condition: "data.config.logging == true"
```

These declarations are separate from `supports.features`, which describes how a
component reacts to a feature already selected for the build. They also differ
from `provides.features`, which advertises capabilities without selecting them.
An enabled component feature is both provided and selected, and activates matching
`supports.features` rules throughout the project.

Features are not enabled merely by being declared. A feature is enabled when it
is selected with `+feature` in the build string, required by a component, selected
by a choice default or recommendation, or its condition evaluates to `true`.
An explicit selection enables the feature independently of its condition; the
condition is evaluated only for features that have not already been selected.
In the example above, `platform_available` remains inactive unless selected,
while `logging` is enabled automatically when `data.config.logging` is true.
Each enabled declaration contributes its requirements, sources, includes, flags,
and other build data to its component once, as well as activating matching
`supports.features` rules.

A feature declaration must be an object. Its optional `condition` must be a
nonempty string; quote boolean or numeric-looking expressions in YAML.

`provides.features` accepts a feature name or a list of names, not a map of
feature settings. To advertise capabilities and apply flags only when a feature
is selected, use:

```yaml
provides:
  features: [cpp23, cpp20]
supports:
  features:
    cpp23:
      flags:
        cpp:
          global: [-std=c++23]
    cpp20:
      flags:
        cpp:
          global: [-std=c++20]
```

Alternatively, put those name-keyed declarations under top-level `features`;
enabled declarations both provide the feature and apply their settings.
Invalid `provides.features` values are rejected before dependency evaluation
reads them. The diagnostic includes the component file, YAML path, and guidance
on where to put feature-specific settings.

A condition is an Inja
expression without delimiters: Yakka wraps it in `{{` and `}}`, parses it once
into a cached Inja template, and renders it against a separate evaluation context.
It must produce `true` or `false`; invalid expressions,
missing values at the end of resolution, and non-boolean results fail evaluation
with a component/feature diagnostic.

Conditions are evaluated after ordinary component and feature requirements,
supported relationships, choice defaults, replacements, recommendations, and SLC
dependency resolution have settled. The parsed condition also retains its
statically known input variable names. Before every condition check, Yakka
rebuilds its data from all currently resolved components using both `requires.data`
and inferred `data.*` paths, applies data merge strategies, and overlays
command-line `--data`. For example, `data.device.clock` collects `/device/clock`
without an explicit `requires.data` entry. Conditions can access this as
`data`, together with `components`, selected `features`, choices, and project metadata.
This template context is separate from the project summary: feature evaluation
updates internal feature state, while summary generation and the final data update
publish the resolved values.

Function arguments are inspected too: `at(data.devices, selected)` requests the
`/devices` subtree, since the selected child is only known at runtime. Data reads
hidden inside callbacks still need explicit `requires.data`. Inferred paths are
used for condition evaluation only; data needed in the final summary or by other
templates still needs `requires.data`.

Enabling features may introduce more components, features, or required data.
Yakka repeats dependency resolution and rechecks inactive conditions until no more
features activate. Resolution is additive: once enabled, a feature stays enabled
for that evaluation even if later data would make its condition false. Each build
reevaluates component features from fresh data rather than inheriting cached
selections. Partial data collection does not duplicate merged arrays on later passes.

## Evaluation diagnostics

Failed builds report unresolved features and invalid choice selections rather than
a numeric project state. Diagnostics include the component file and YAML path
that selected a feature or component, including active `supports` rules, enabled
`features` declarations, choice defaults, and command-line selections.

An unresolved feature needs a selected provider: `provides.features` advertises a
capability, while an enabled top-level `features` declaration both provides and
selects it. `supports.features` only reacts to a selection and does not itself
provide the feature. Check spelling and case before adding a provider or correcting
the requirement.

Choices use `options` entries containing `feature` or `component`. They are
exclusive unless `exclusive: false` is specified. A conflict reports the selected
options and their sources; correct the requirements, declarations, or defaults so
only one remains selected. Use `exclusive: false` only for compatible options,
not mutually exclusive hardware architectures. An incomplete choice lists the
available options; select one with `+feature` or a component on the command line,
in `requires`, or via the choice's `default`.

Data choices specify a `data` path (relative to the project data root) and an
`options` array of allowed values, rather than feature/component entries:

```yaml
choices:
  log_level:
    description: Logging level
    data: /logging/level
    options: [debug, info, warning, error]
    default: info
```

Yakka automatically collects the choice's data path from component data; no
`requires.data` entry is needed. Command-line `--data` takes precedence. If the
path is missing, `default` supplies its value and is preserved in the project
summary and condition evaluation data. Without a default, a missing path is an
incomplete choice, even with only one option. An existing value outside `options`
is an error, not a reason to apply the default. Defaults must also be allowed
values. Values retain their YAML types (for example, `true` and `"true"` differ).

With `exclusive: false`, the path can contain an array of allowed selections, and
`default` can be an array too. By default, multiple selections are an error.
Objects and arrays may themselves be allowed values; an exact match against an
option is treated as one selection before considering an array as multiple
selections.

## Data merge strategies

Use `merge_strategy` on a `data_schema` property to resolve values contributed by
multiple components. The older `merge` spelling is also supported;
`merge_strategy` takes precedence when both are present.

Without an `enum`, `max` and `min` compare numbers. With an `enum`, its entries
define a highest-to-lowest ranking: `max` selects the earliest entry and `min`
selects the latest, independently of component order. This also applies to numeric
enums; declaration order, not numeric magnitude, determines their ranking.
Both values must belong to the enum; invalid comparisons log an error and leave
the existing value unchanged.

```yaml
data_schema:
  toolchain:
    properties:
      cpp_standard:
        type: string
        merge_strategy: max
        enum: [c++26, c++23, c++20, c++17, c++14, c++11, c++98]
```

For example, contributions of `c++17` and `c++23` resolve to `c++23`. A data
choice can separately provide the default when no component supplies a value;
its default does not compete with contributed values.

## Conditional choices

Any choice can have an optional `condition`, using the same Inja expression syntax
and evaluation context as component features:

```yaml
choices:
  cpp_standard:
    description: C++ language standard
    condition: '"cpp" in features'
    options:
      - feature: cpp20
      - feature: cpp23
    default:
      feature: cpp23
```

Without a condition, a choice participates as before. With a condition, it joins
the project evaluation only when the expression evaluates to `true`. Until then,
it is absent from the project summary's `choices`, does not select defaults, and
cannot produce incomplete or conflicting selection errors. Inactive data choices
also do not request their data paths or supply default data.

Conditions work on feature/component choices and data choices, including choices
introduced by enabled `features` declarations and matching `supports` rules.
They must be nonempty strings producing `true` or `false`. Invalid expressions,
missing inputs at the end of resolution, and non-boolean results fail evaluation
with a diagnostic identifying the choice and its source.

Inactive choices are rechecked as features, components, and data become available.
Once activated, a choice remains active for the rest of that evaluation, even if
its own default later makes its condition false. Builds reevaluate conditions
against fresh project data, including command-line `--data`.
