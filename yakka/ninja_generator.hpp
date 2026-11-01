#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace yakka {

class project;
class workspace;

struct ninja_generator_options {
  /// Absolute path of the yakka binary, written as the manifest's `yakka` variable.
  std::string yakka_binary;
  /// Shell-quoted arguments that reproduce this project request (used by regeneration and exec-blueprint edges).
  std::string yakka_arguments;
  /// Space-separated names of the projects this project depends on, written as the manifest's `yakka_projects` variable.
  std::string projects;
};

/// Name of the generated manifest inside a project's output folder.
inline constexpr const char *ninja_manifest_filename = "build.ninja";

/// Evaluate blueprints that do not depend on build outputs and write `<project output>/build.ninja`.
/// The manifest is relative to the workspace root, which must be the current directory when Ninja runs.
std::expected<std::filesystem::path, std::string> generate_ninja_manifest(project &project, const workspace &workspace, const ninja_generator_options &options);

/// Escape a path for use in a Ninja `build` line.
std::string ninja_escape_path(std::string_view path);
/// Escape a value for use on the right-hand side of a Ninja variable binding.
std::string ninja_escape_value(std::string_view value);
/// Quote an argument for POSIX /bin/sh.
std::string shell_quote(std::string_view argument);

} // namespace yakka
