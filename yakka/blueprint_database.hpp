#pragma once

#include "yakka_blueprint.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <filesystem>

namespace yakka {
struct blueprint_match {
  std::vector<ryml::csubstr> dependencies; // Template processed dependencies
  std::shared_ptr<yakka::blueprint> blueprint;
  std::vector<ryml::csubstr> regex_matches; // Regex capture groups for a particular regex match
};

class blueprint_database {
public:
  blueprint_database();

  std::vector<std::shared_ptr<blueprint_match>> find_match(ryml::csubstr target, ryml::ConstNodeRef project_summary);

  void load(const std::filesystem::path filename);
  void save(const std::filesystem::path filename);

  // void generate_task_database(std::vector<std::string> command_list);
  // void process_blueprint_target( const std::string target );
  std::vector<ryml::csubstr> parse_gcc_dependency_file(const std::string &filename);

  void create_blueprint(const std::string &target, ryml::ConstNodeRef blueprint_data, c4::csubstr parent_path);

  ryml::Tree database;
  std::multimap<c4::csubstr, std::shared_ptr<blueprint>> blueprints;
};

} // namespace yakka
