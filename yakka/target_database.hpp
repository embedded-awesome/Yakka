#pragma once

#include "blueprint_database.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fs = std::filesystem;

namespace yakka {
class target_database {
public:
  void load(const fs::path file_path);
  void save(const fs::path file_path);

  void add_target(const std::string target, blueprint_database &blueprint_database, nlohmann::json project_summary);

  std::multimap<std::string, std::shared_ptr<blueprint_match>> targets;
};

} // namespace yakka
