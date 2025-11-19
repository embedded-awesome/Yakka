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
  void load(const std::filesystem::path file_path);
  void save(const std::filesystem::path file_path);

  const std::vector<std::shared_ptr<blueprint_match>>& add_target(const std::string target, blueprint_database &blueprint_database, nlohmann::json project_summary);
  const std::vector<std::shared_ptr<blueprint_match>>& get_target(const std::string target) const {
    return targets.at(target);
  }
private:
  std::map<std::string, std::vector<std::shared_ptr<blueprint_match>>> targets;
};

} // namespace yakka
