#pragma once

#include "blueprint_database.hpp"
#include "yakka.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace fs = std::filesystem;

namespace yakka {
class target_database {
public:
  yakka_status load(const std::filesystem::path file_path, const blueprint_database &blueprint_database);
  yakka_status save(const std::filesystem::path file_path);

  const std::vector<std::shared_ptr<blueprint_match>>& add_target(ryml::csubstr target, blueprint_database &blueprint_database, ryml::ConstNodeRef project_summary);
  const std::vector<std::shared_ptr<blueprint_match>>& get_target(ryml::csubstr target) const {
    return targets.at(target);
  }
  std::map<ryml::csubstr, std::vector<std::shared_ptr<blueprint_match>>> targets;

private:
  ryml::Tree database;
};

} // namespace yakka
