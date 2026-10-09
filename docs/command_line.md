# Command line interface

The Yakka CLI operates by executing commands on a list of components and features.
Yakka has a minimal set of built-in commands that are reserved for it internal use
- `build`
- `register`
- `list`
- `update`


The first argument provided to Yakka is assumed to be a command unless it ends with a `!` to indicate that is references a [blueprint](blueprints).
The remaining arguments are interpreted as component names but can be features if prefixed with `+` such as `+feature` or can be blueprints if suffixed with `!` such as `compile!`.
There can be any number of features or blueprints provided via the command line.

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


