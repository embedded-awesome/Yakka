# Template Engine

The template engine used by Yakka is [inja](https://github.com/pantor/inja).

# Template Engine Documentation

The Yakka project uses the Inja template engine (a modern C++ template engine) with some custom additions to facilitate code generation and template rendering.

## Base Inja Functionality
- Variable substitution using `{{ variable }}`
- Loops using `{% for item in items %}` 
- Conditionals using `{% if condition %}`
- Includes using `{% include "template.txt" %}`
- `exists(path)` checks whether a value exists in the template data. Paths may use dots (for example, `"component.sources"`) or slash separators (for example, `"/component/sources"`); `existsIn(object, path)` supports slash-separated nested paths relative to the object.

## Yakka-Specific Extensions

### Added Functions

#### Project Naming
- `project_name(segments)`: Returns Yakka's canonical project name for an array
  of build argument strings. Components are joined with `-`, followed by
  `+features`; order within each group is preserved. Blueprint commands ending
  in `!` do not contribute to the name. For example,
  `{{ project_name(["link!", "app", "+debug", "board", "xcode"]) }}`
  returns `app-board-xcode+debug`. Filter syntax is also supported:
  `{{ segments | project_name }}`. This computes the default name, without
  applying a CLI project-name override, and does not change the `project_name`
  template variable. An empty array or command-only array returns an empty
  string. Non-array inputs, non-string elements, invalid build arguments or
  project names, and alternatives expanding to multiple projects raise an error.

#### String Manipulation
- `snake_case(str)`: Converts string to snake_case format
- `camel_case(str)`: Converts string to camelCase format
- `pascal_case(str)`: Converts string to PascalCase format
- `kebab_case(str)`: Converts string to kebab-case format
- `upper_case(str)`: Converts string to UPPERCASE
- `lower_case(str)`: Converts string to lowercase

#### Type Conversions
- `type_to_string(type)`: Converts internal type representation to string format
- `remove_ref(type)`: Removes reference from type name
- `remove_const(type)`: Removes const qualifier from type name
- `raw_type(type)`: Gets raw type name without qualifiers

#### Collection Operations
- `keys(map)`: Returns an array of key strings in the map's iteration order.
  An empty map returns `[]`; non-map arguments raise a render error. The map
  is not modified. Also supports `{{ components | keys }}`. To select component
  names present in the summary, use
  `{{ intersect(wanted_components, keys(components)) }}`.
- `join(array, delimiter)`: Joins array elements with specified delimiter
- `intersect(left, right)`: Returns distinct elements present in both arrays,
  preserving their order in `left` and using Inja's equality rules. Neither input
  is modified. A scalar first argument (including null) is treated as a
  single-element array; a map first argument or a non-array second argument
  raises a render error. For example,
  `{{ intersect([3, 1, 3, 2], [2, 3]) }}` returns `[3,2]`.
  `{{ intersect("uart", ["uart", "spi"]) }}` returns `["uart"]`; a scalar
  without a match returns `[]`.
  Filter syntax is also supported: `{{ left | intersect(right) }}`.
- `concat(str1, str2)`: Concatenates two strings

#### Path Operations
- `base_name(path)`: Extracts base name from file path
- `dir_name(path)`: Extracts directory name from file path
- `file_ext(path)`: Extracts file extension from path

### File System Operations

`dir`, `not_dir`, `parent_path`, and `extension` are lexical Inja built-ins:
they inspect only their arguments, not the filesystem. Absolute/relative path
resolution, filesystem queries, globbing, and file loading remain Yakka callbacks.

- `dir(path)`: Returns the parent directory of a path. If the path is a directory, returns the path itself
- `not_dir(path)`: Returns the filename portion of a path
- `parent_path(path)`: Returns the parent directory of a path
- `absolute_dir(path)`: Converts a directory path to its absolute form
- `absolute_path(path)`: Converts a file path to its absolute form
- `relative_path(path)`: Converts a path to be relative to the current working directory
- `relative_path(path1, path2)`: Converts path1 to be relative to path2
- `extension(path)`: Returns the file extension without the dot (e.g., "cpp" instead of ".cpp")
- `filesize(path)`: Returns the size of a file in bytes
- `file_exists(path)`: Returns true if the file exists, false otherwise

### File Content Operations

- `read_file(path)`: Reads the entire contents of a file and returns it as a string
- `load_yaml(path)`: Loads and parses a YAML file, returning its contents as a JSON object
- `load_json(path)`: Loads and parses a JSON file, returning its contents as a JSON object

### String Operations

- `quote(value)`: Wraps a string, integer, or float value in quotes
- `replace(input, target, match)`: Performs regex-based string replacement
- `regex_escape(input)`: Escapes regex metacharacters in a string
- `split(input, delimiter)`: Splits a string by a delimiter and returns an array
- `startswith(input, prefix)`: Built-in Inja function returning `true` or `false`
  for a case-sensitive prefix match. An empty prefix always matches. The legacy
  `starts_with` callback has been removed; use `startswith` in templates and conditions.
- `substring(input, index)`: Returns a substring starting from the given index
- `trim(input)`: Removes whitespace from both ends of a string

### File System Pattern Matching

- `glob(patterns...)`: Takes one or more glob patterns and returns an array of matching file paths

### Numeric Operations

- `hex2dec(hex_string)`: Converts a hexadecimal string to its decimal value

### Argument-only Inja Built-ins

These functions are available on a plain `inja::Environment`, without registering
Yakka callbacks, in both the modular and single-header versions:

- Paths: `dir`, `not_dir`, `parent_path`, `extension`
- Strings/conversion: `quote`, `regex_escape`, `hex2dec`, `concatenate(args...)`
- Collections: `join(array, delimiter)`, `intersect(left, right)`, `keys(map)`, `find_json(value, key)`
- Hashing: `sha256sum`, `xxh64sum`, `fnv1a_32`

Hash functions return lowercase hexadecimal strings of 64, 16, and 8 characters,
respectively. Scalars are hashed as their original text; maps and arrays are
hashed as their JSON serialization. `xxh64sum` uses seed zero.
`find_json` returns matching key paths in traversal order, using dots for map
keys and brackets for array indices (for example, `devices[0].name`).
An empty `join` returns an empty string; its first argument must be an array.
An extensionless path returns an empty string from `extension`.

The incomplete `merge` and `load_xml` callbacks remain in Yakka. The existing
Inja `merge` built-in is unchanged and is still shadowed by Yakka's callback
when common callbacks are registered.

### Usage Examples

```jinja
{# File path operations #}
{{ dir("/path/to/file.txt") }}                  {# Returns "/path/to" #}
{{ not_dir("/path/to/file.txt") }}             {# Returns "file.txt" #}
{{ extension("script.py") }}                    {# Returns "py" #}

{# String manipulation #}
{{ quote("hello") }}                           {# Returns "\"hello\"" #}
{{ split("a,b,c", ",") }}                     {# Returns ["a", "b", "c"] #}
{{ trim("  hello  ") }}                       {# Returns "hello" #}

{# File operations #}
{% set contents = read_file("config.txt") %}
{% set config = load_json("settings.json") %}
{% set yaml_data = load_yaml("data.yaml") %}

{# File system queries #}
{% set size = filesize("large_file.dat") %}
{% if file_exists("optional.cfg") %}
  {# Handle existing file #}
{% endif %}

{# Pattern matching #}
{% for file in glob("src/**/*.cpp") %}
  {# Process each cpp file #}
{% endfor %}
```

These template functions provide a comprehensive set of tools for file manipulation, string processing, and data loading, making it easy to generate complex output based on your project's needs.