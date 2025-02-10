#pragma once

#include "yakka_blueprint.hpp"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <filesystem>

namespace fs = std::filesystem;

namespace yakka {
struct blueprint_match {
  std::vector<std::string> dependencies; // Template processed dependencies
  std::shared_ptr<yakka::blueprint> blueprint;
  std::vector<std::string> regex_matches; // Regex capture groups for a particular regex match
};

class blueprint_database {
public:
  std::vector<std::shared_ptr<blueprint_match>> find_match(const std::string target, const nlohmann::json &project_summary);

  void load(const fs::path filename);
  void save(const fs::path filename);

  // void generate_task_database(std::vector<std::string> command_list);
  // void process_blueprint_target( const std::string target );

  std::multimap<std::string, std::shared_ptr<blueprint>> blueprints;
};

} // namespace yakka
