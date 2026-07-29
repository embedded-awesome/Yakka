/**
 * @file target_database.cpp
 * @brief Implements target lookup and blueprint match caching for project commands.
 */

#include "target_database.hpp"
#include "blueprint_database.hpp"
#include "inja.hpp"
#include "yakka.hpp"

#include <regex>

namespace yakka {

/// @brief Executes add_target.

const std::vector<std::shared_ptr<blueprint_match>>& target_database::add_target(ryml::csubstr target, blueprint_database &blueprint_database, ryml::ConstNodeRef project_summary)
{
  // Check if target is not in the database. Note task_database is a multimap

  if (targets.find(target) == targets.end()) {
    const auto match = blueprint_database.find_match(target, project_summary);
    targets.insert({ target, match });
  }
  return targets[target];
}

} // namespace yakka
