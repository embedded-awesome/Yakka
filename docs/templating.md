# Template Engine

The template engine used by Yakka is [inja](https://github.com/pantor/inja).

# Template Engine Documentation

The Yakka project uses the Inja template engine (a modern C++ template engine) with some custom additions to facilitate code generation and template rendering.

## Base Inja Functionality
- Variable substitution using `{{ variable }}`
- Loops using `{% for item in items %}` 
- Conditionals using `{% if condition %}`
- Includes using `{% include "template.txt" %}`

## Yakka-Specific Extensions

### Added Functions

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
- `join(array, delimiter)`: Joins array elements with specified delimiter
- `concat(str1, str2)`: Concatenates two strings

#### Path Operations
- `base_name(path)`: Extracts base name from file path
- `dir_name(path)`: Extracts directory name from file path
- `file_ext(path)`: Extracts file extension from path

### File System Operations

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
- `starts_with(input, start)`: Returns true if the input string starts with the given prefix
- `substring(input, index)`: Returns a substring starting from the given index
- `trim(input)`: Removes whitespace from both ends of a string

### File System Pattern Matching

- `glob(patterns...)`: Takes one or more glob patterns and returns an array of matching file paths

### Numeric Operations

- `hex2dec(hex_string)`: Converts a hexadecimal string to its decimal value

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