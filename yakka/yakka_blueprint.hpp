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
    std::string name;
  };
  std::string target;
  std::optional<std::string> regex;
  std::vector<std::string> requirements;
  std::vector<dependency> dependencies; // Unprocessed dependencies. Raw values as found in the YAML.
  ryml::Tree data;
  ryml::ConstNodeRef process;
  std::string parent_path;
  std::string task_group;

  blueprint(const std::string &target, ryml::Tree blueprint_data, const std::string &parent_path);

  const ryml::Tree &as_ryml() const;
};
} // namespace yakka
