#include "target_database.hpp"
#include "blueprint_database.hpp"
#include "inja.hpp"
#include "yakka.hpp"
#include <regex>

namespace yakka {

const std::vector<std::shared_ptr<blueprint_match>>& target_database::add_target(const std::string target, blueprint_database &blueprint_database, nlohmann::json project_summary)
{
  // Check if target is not in the database. Note task_database is a multimap
  if (targets.find(target) == targets.end()) {
    const auto match = blueprint_database.find_match(target, project_summary);
    targets.insert({ target, match });
  }
  return targets[target];
}

} // namespace yakka