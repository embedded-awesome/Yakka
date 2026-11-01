#pragma once

#include "file_check.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace yakka {

/// Everything a legacy (Taskflow) no-op build depended on, so an identical invocation can confirm nothing changed without loading the workspace.
struct legacy_fast_record {
  struct file {
    std::string path;
    std::optional<file_stat> stat; // nullopt when the file was missing
  };
  struct glob_expansion {
    std::vector<std::string> patterns;
    std::vector<std::string> matches;
  };

  std::vector<std::string> projects; // Requested projects, reported as successful
  std::vector<file> files;
  std::vector<glob_expansion> globs;
};

/// True when the file starts with the legacy record magic.
bool is_legacy_fast_record(const std::filesystem::path &path);

bool write_legacy_fast_record(const std::filesystem::path &path, const legacy_fast_record &record);

/// nullopt when the file is missing, truncated or another format.
std::optional<legacy_fast_record> read_legacy_fast_record(const std::filesystem::path &path);

/// True when every recorded file has the same stat (or is still missing) and every glob expands to the same files.
bool legacy_fast_record_current(const legacy_fast_record &record);

} // namespace yakka
