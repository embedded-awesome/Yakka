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
requires:
  data:
    - /config
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

Features without a condition are enabled automatically. A condition is an Inja
expression without delimiters: Yakka wraps it in `{{` and `}}` and renders it
against the project summary. It must produce `true` or `false`; invalid expressions,
missing values at the end of resolution, and non-boolean results fail evaluation
with a component/feature diagnostic.

Conditions are evaluated after ordinary component and feature requirements,
supported relationships, choice defaults, replacements, recommendations, and SLC
dependency resolution have settled. Before each condition pass, Yakka partially
collects `requires.data` from the components resolved so far, applies data merge
strategies, and overlays command-line `--data`. Conditions can access this as
`data`, together with `components`, selected `features`, choices, and project metadata.
This template context is separate from the project summary: feature evaluation
updates internal feature state, while summary generation and the final data update
publish the resolved values.

Enabling features may introduce more components, features, or required data.
Yakka repeats dependency resolution and rechecks inactive conditions until no more
features activate. Resolution is additive: once enabled, a feature stays enabled
for that evaluation even if later data would make its condition false. Each build
reevaluates component features from fresh data rather than inheriting cached
selections. Partial data collection does not duplicate merged arrays on later passes.
