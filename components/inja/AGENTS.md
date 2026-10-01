# Inja Component Agent Guide

This component is a vendored and locally modified Inja template engine. The API in `master/include/inja` is not stock Jinja and is not quite stock upstream Inja: it renders `ryml`/rapidyaml node references, supports local mutable render data, and logs many render errors through `spdlog` instead of throwing.

Use this guide when editing templates, adding template features, or debugging behavior in this component.

## Important Files

- `master/include/inja/environment.hpp`: public C++ facade for parsing, rendering, writing files, callbacks, includes, delimiter config, and autoescape.
- `master/include/inja/parser.hpp`: accepted template statements and expression grammar.
- `master/include/inja/function_storage.hpp`: list of built-in functions and their arities.
- `master/include/inja/renderer.hpp`: runtime semantics for expressions, loops, inheritance, macros, mutable scratch data, and missing values.
- `master/include/inja/lexer.hpp` and `master/include/inja/config.hpp`: delimiters, whitespace trimming, line statements, tokens, and identifier rules.
- `master/single_include/inja/inja.hpp`: generated amalgamated header; regenerate it after changing modular headers.
- `master/test/test-functions.cpp`: focused built-in function tests.
- `master/test/test.cpp`: standalone Inja test entry point.
- `test/inja_suite`: behavioral fixtures for this fork.

## Useful Local Commands

From the repository root:

- Build Yakka on macOS: `./yakka-macos link! yakka_cli xcode`
- Reuse the generated Yakka CLI for an incremental build: `./output/yakka_cli-xcode/yakka_cli-xcode -p temp link! yakka_cli xcode`
- Build the Inja suite through the project graph when `inja_test.yakka` is present: `./output/yakka_cli-xcode/yakka_cli-xcode -p temp link! inja_test xcode`
- Regenerate the amalgamated header after modular header changes: `components/inja/master/scripts/update_single_include.sh`
- List available standalone CMake targets: `cmake --build components/inja/master/build --target help | rg 'test|inja'`
- Build modular standalone tests: `cmake --build components/inja/master/build --target inja_test -j2`
- Build amalgamated-header tests: `cmake --build components/inja/master/build --target single_inja_test -j2`
- Run registered standalone tests: `ctest --test-dir components/inja/master/build --output-on-failure`
- Check only the component diff before committing: `git diff --check -- components/inja/master`

The standalone CMake project expects RapidYAML headers to be supplied by its parent integration. If it reports `ryml.hpp file not found`, use the active Yakka build above or configure the standalone target with the repository's RapidYAML include paths before retrying. The project-graph `inja_test` command likewise requires the top-level `inja_test.yakka` component; it is not available in every checkout.

For a focused runtime smoke test, compile a temporary harness with the active generated options from `output/temp/temp.global_cpp_options`, include `components/inja/master/include/inja`, and link the RapidYAML objects under `output/temp/components/rapidyaml/master`. Prefer non-reserved input names such as `vars`; `values` is reserved scratch storage used by callbacks.

- The focused behavioral fixtures live under `test/inja_suite`; use them as the first place to add examples when changing template semantics.

## Public C++ Surface

The main entry point is `inja::Environment`.

- Construction: `Environment()`, `Environment(global_path)`, or `Environment(input_path, output_path)`.
- Delimiter config: `set_statement(open, close)`, `set_line_statement(open)`, `set_expression(open, close)`, `set_comment(open, close)`.
- Whitespace config: `set_trim_blocks(bool)` and `set_lstrip_blocks(bool)`.
- Include behavior: `set_search_included_templates_in_files(bool)`, `set_throw_at_missing_includes(bool)`, `set_include_callback(callback)`, and `include_template(name, tmpl)`.
- Escaping: `set_html_autoescape(bool)` escapes output expressions with `&amp;`, `&quot;`, `&apos;`, `&lt;`, and `&gt;`.
- Parsing/rendering: `parse(input)`, `parse_template(filename)`, `parse_file(filename)`, `render(input, data)`, `render(tmpl, data)`, `render_file(filename, data)`, `render_to(os, tmpl, data)`, and `render_to(os, input, data)`.
- File helpers: `load_file(filename)`, `load_json(filename_or_inline_json)`, `render_file_with_json_file(...)`, `write(...)`, and `write_with_json_file(...)`.
- Custom functions: `add_callback(name, callback)`, `add_callback(name, arity, callback)`, `add_void_callback(name, callback)`, and `add_void_callback(name, arity, callback)`.

Callback signatures use rapidyaml nodes:

- `CallbackFunction`: `ConstNodeRef(Arguments& args, NodeRef additional_data)`
- `VoidCallbackFunction`: `void(Arguments& args, NodeRef additional_data)`
- `Arguments`: `std::vector<ConstNodeRef>`

`additional_data` is this fork's render scratch/scope tree. Functions that mutate template state should write there rather than the input data tree.

## Template Statements

Supported statement commands are:

- `{% if expression %}...{% else %}...{% endif %}` with chained `{% else if expression %}`.
- `{% for value in sequence %}...{% endfor %}` for sequences.
- `{% for key, value in object %}...{% endfor %}` for maps.
- `{% include "name" %}` and `{% extends "name" %}`.
- `{% block name %}...{% endblock %}` plus `super()` or `super(level)` inside blocks.
- `{% set name = expression %}` and dotted assignment such as `{% set scratch.name = value %}`.
- `{% macro name(arg, other) %}...{% endmacro %}` with fixed arity. Macro calls are expressions: `{{ name(value, other) }}`.
- Side-effect function statements such as `{% setAt(scratch, "meta", value) %}` or `{% push_back(scratch, "items", item) %}`. This is a local extension and is useful for mutating scratch state without printing a return value.

Line statements are enabled by default with `##` at the start of a line. Comments use `{# ... #}`.

## Expressions And Operators

Supported literals include strings, numbers, `true`, `false`, `null`, inline arrays, and inline objects as parsed by rapidyaml.

Supported operators:

- Boolean and membership: `not`, `and`, `or`, `in`.
- Comparison: `==`, `!=`, `>`, `>=`, `<`, `<=`.
- Arithmetic: `+`, `-`, `*`, `/`, `^`, `%`.
- Attribute access: `value.child` maps to a rapidyaml pointer path.
- Index access: `value[index]`, `value["key"]`, `call()[0]`, and `(expr)[idx]`.
- Pipe calls: `value | function` and `value | function(extra, args)`.

Identifier tokens are permissive: they may contain letters, digits, `.`, `/`, `_`, and `-`, and may start with `_`, `@`, or `$`. Dot names are converted to rapidyaml pointer paths, so `a.b` means `/a/b`.

## Built-In Functions

Built-ins are arity-specific. Unknown function/arity pairs are parse errors unless a custom callback or same-arity macro exists.

- Lookup and existence: `at(path)`, `at(container, key_or_index)`, `exists(name)`, `existsIn(object, key)`, `fetch(path)`, `fetch(path, key)`.
- Mutation of render scratch data: `setAt(path_or_node, value)`, `setAt(path_or_node, key, value)`, `push_back(path_or_node, value)`, `push_back(path_or_node, key, value)`, `erase(path)`, `unique(sequence)`.
- Strings and conversion: `capitalize(value)`, `lower(value)`, `upper(value)`, `trim(value)`, `replace(input, regex, replacement)`, `split(input, delimiter)`, `substring(input, start)`, `join(sequence, separator)`, `hex(value)`, `int(value)`, `float(value)`.
- Sequence and scalar helpers: `first(sequence)`, `last(sequence)`, `length(value)`, `range(limit)`, `sort(sequence)`, `min(sequence)`, `max(sequence)`, `filter(sequence, regex)`, `round(value, precision)`.
- Sequence-selection filters: `map(sequence, attribute)`, `select(sequence, [attribute_or_test], [test, expected])`, `reject(...)`, `selectattr(sequence, attribute, [test, expected])`, `rejectattr(...)`, `groupby(sequence, attribute, [default])`, `dictsort(mapping, [key_or_value], [reverse])`, `items(mapping)`, `list(value)`, `reverse(value)`, `slice(sequence, slice_count, [fill])`, and `batch(sequence, batch_size, [fill])`.
- Aggregation and formatting: `sum(sequence, [attribute], [start])`, `abs(value)`, `string(value)`, variadic `format(format_string, ...)`, `title(value)`, `center(value, [width])`, `indent(value, [width], [first], [blank])`, `truncate(value, [length], [kill_words], [end], [leeway])`, `wordcount(value)`, `wordwrap(value, [width], [break_long_words], [break_on_hyphens], [wrap_string])`, and `pprint(value)`.
- Tests: `divisibleBy(value, divisor)`, `even(value)`, `odd(value)`, `isArray(value)`, `isBoolean(value)`, `isFloat(value)`, `isInteger(value)`, `isNumber(value)`, `isObject(value)`, `isString(value)`.
- Inheritance: `super()` and `super(level)`.
- User code: callbacks registered through `Environment::add_callback` or `add_void_callback`.

## Behavior That Differs From Standard Jinja

- Data model: templates render rapidyaml `ConstNodeRef`/`NodeRef`, not Python objects. Inline literals are parsed into YAML/JSON-like rapidyaml nodes.
- Missing values: many missing lookups produce invalid/null-like nodes and render as empty text. Missing for-loop sources render as an empty loop rather than throwing.
- Render errors: many renderer errors are logged through `spdlog::error` and rendering may continue; do not assume exceptions for every runtime problem.
- Function names use camelCase or local names such as `divisibleBy`, `existsIn`, `isArray`, `setAt`, and `push_back`, not Jinja test/filter syntax.
- Tests are normal functions, not `is` expressions. Use `even(x)` rather than `x is even`.
- Filters are implemented as pipe calls to ordinary functions. Use `value | upper` or `value | replace("a", "b")`; the parser resolves these against built-in functions, callbacks, or macros by arity.
- Object loops are rendered in sorted key order. Loop metadata includes `loop.index` zero-based, `loop.index1` one-based, `loop.key` for object loops, `loop.is_first`, `loop.is_last`, and nested `loop.parent` metadata.
- `set` writes into render scratch scope, not the input data. Dotted `set` names create nested scratch data.
- `scope.global` and `scope.global.*` are special. Inside macros, they resolve against the shared template/global scope first and then the input data, allowing access to outer values shadowed by macro parameters or local variables.
- Mutating functions operate on scratch data. `setAt`, `push_back`, and `erase` do not mutate the original input tree.
- Includes are resolved from the environment input path and cached in template storage. `set_include_callback` can provide missing templates. Missing includes/extents may log instead of throw depending on `set_throw_at_missing_includes`.
- `replace` and `filter` use C++ `std::regex`; patterns are regexes, not literal strings.
- `+` adds numbers when both sides convert to numbers; otherwise it concatenates string conversions.
- Object and sequence values printed directly are emitted as JSON via rapidyaml serialization.
- Macro definitions produce no output. Macro calls return captured rendered text. Macro recursion is capped at depth 1000.
- There is no evidence in this fork for full Jinja features such as `{% elif %}`, `{% with %}`, `{% import %}`, `{% from %}`, call blocks, keyword arguments, default macro arguments, whitespace `+`/`-` behavior exactly matching Python Jinja, or Jinja's full filter/test catalog.

## Local Examples

Scratch mutation:

```jinja
{%- set scratch = {} -%}
{%- set scratch.name = payload_name -%}
{%- setAt(scratch, "meta", extra) -%}
{%- for n in numbers -%}{%- push_back(scratch, "numbers", n) -%}{%- endfor -%}
{{ fetch("/scratch", "name") }} {{ join(fetch("/scratch/numbers"), ",") }}
```

Macro scope:

```jinja
{%- set name = "outer" -%}
{%- macro greet(name) -%}{{ name }}|{{ scope.global.name }}{%- endmacro -%}
{{ greet("local") }}
```

Loop metadata:

```jinja
{%- for key, value in settings -%}
{{ loop.index }}:{{ loop.key }}={{ key }}={{ value }}:{{ loop.is_first }}:{{ loop.is_last }}
{%- endfor -%}
```

Postfix indexing and pipe calls:

```jinja
{{ users[0].name | upper }}
{{ at(settings, "mode") }}
```

## Change Guidance

- Add or update parser support in `parser.hpp`, built-in registration in `function_storage.hpp`, AST shape in `node.hpp`, and runtime behavior in `renderer.hpp` together.
- Add focused fixtures under `test/inja_suite` for new or changed behavior. Prefer examples that show the exact syntax and expected text output.
- Be careful with `not_found_stack` in renderer changes. If expression evaluation consumes or ignores invalid nodes, keep this stack balanced so later expressions do not report stale missing-variable names.
- For loop behavior, preserve the current rule that a missing loop source is an empty iteration.
- For postfix indexing, remember that top-level `]` is a delimiter while recursively parsing the index expression.