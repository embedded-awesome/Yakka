#include "target_database.hpp"
#include "blueprint_database.hpp"
#include "inja.hpp"
#include "yakka.hpp"
#include <regex>

namespace yakka {

void target_database::add_target(const std::string target, blueprint_database &blueprint_database, nlohmann::json project_summary)
{
}

void target_database::generate_target_database(std::vector<std::string> commands, blueprint_database &blueprint_database, nlohmann::json project_summary)
{
  std::vector<std::string> new_targets;
  std::unordered_set<std::string> processed_targets;
  std::vector<std::string> unprocessed_targets;

  for (const auto &c: commands)
    unprocessed_targets.push_back(c);

  while (!unprocessed_targets.empty()) {
    for (const auto &t: unprocessed_targets) {
      // Add to processed targets and check if it's already been processed
      if (processed_targets.insert(t).second == false)
        continue;

      // Do not add to task database if it's a data dependency. There is special processing of these.
      if (t.front() == yakka::data_dependency_identifier)
        continue;

      // Check if target is not in the database. Note task_database is a multimap
      if (targets.find(t) == targets.end()) {
        const auto match = blueprint_database.find_match(t, project_summary);
        for (const auto &m: match)
          targets.insert({ t, m });
      }
      auto tasks = targets.equal_range(t);

      std::for_each(tasks.first, tasks.second, [&new_targets](auto &i) {
        if (i.second)
          new_targets.insert(new_targets.end(), i.second->dependencies.begin(), i.second->dependencies.end());
      });
    }

    unprocessed_targets.clear();
    unprocessed_targets.swap(new_targets);
  }
}
} // namespace yakka