#pragma once

#include "taskflow.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <future>
#include <optional>
#include <filesystem>

namespace yakka {

struct blueprint {
  struct dependency {
    enum dependency_type { DEFAULT_DEPENDENCY, DATA_DEPENDENCY, DEPENDENCY_FILE_DEPENDENCY } type;
    c4::csubstr name;
  };
  c4::csubstr target;
  std::optional<c4::csubstr> regex;
  std::vector<c4::csubstr> requirements;
  std::vector<dependency> dependencies; // Unprocessed dependencies. Raw values as found in the YAML.
  ryml::ConstNodeRef data;
  ryml::ConstNodeRef process;
  c4::csubstr parent_path;
  c4::csubstr task_group;

  blueprint(const c4::csubstr &target, ryml::ConstNodeRef blueprint_data, const c4::csubstr &parent_path);
};
} // namespace yakka
