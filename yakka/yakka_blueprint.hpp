#pragma once

#include "taskflow.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <future>
#include <optional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace yakka {

struct blueprint_regex;

struct blueprint {
  struct dependency {
    enum dependency_type { DEFAULT_DEPENDENCY, DATA_DEPENDENCY, DEPENDENCY_FILE_DEPENDENCY, PROJECT_DEPENDENCY } type;
    c4::csubstr name;
    std::vector<c4::csubstr> arguments;
  };
  c4::csubstr target;
  std::optional<c4::csubstr> regex;
  std::vector<c4::csubstr> requirements;
  std::vector<dependency> dependencies; // Unprocessed dependencies. Raw values as found in the YAML.
  ryml::ConstNodeRef data;
  ryml::ConstNodeRef process;
  c4::csubstr parent_path;
  c4::csubstr task_group;
  std::shared_ptr<const blueprint_regex> compiled_regex;
  size_t insertion_order = 0;

  blueprint(c4::csubstr target, ryml::ConstNodeRef blueprint_data, c4::csubstr parent_path);
  bool match_regex(const std::string &value, std::vector<std::string> &captures) const;
};
} // namespace yakka
