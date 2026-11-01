#pragma once

#include "yakka_blueprint.hpp"
#include "file_check.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <filesystem>
#include <set>

namespace yakka {
struct blueprint_match {
  std::vector<ryml::csubstr> dependencies; // Template processed dependencies
  std::vector<std::vector<c4::csubstr>> project_dependencies;
  std::shared_ptr<yakka::blueprint> blueprint;
  std::vector<ryml::csubstr> regex_matches; // Regex capture groups for a particular regex match
  std::vector<ryml::csubstr> dependency_files; // Rendered compiler dependency files (e.g. gcc .d)
};

class blueprint_database {
public:
  blueprint_database();
  ~blueprint_database();

  std::vector<std::shared_ptr<blueprint_match>> find_match(ryml::csubstr target, ryml::ConstNodeRef project_summary);

  void load(const std::filesystem::path filename);
  void save(const std::filesystem::path filename);

  // void generate_task_database(std::vector<std::string> command_list);
  // void process_blueprint_target( const std::string target );
  std::vector<ryml::csubstr> parse_gcc_dependency_file(const std::string &filename);

  void create_blueprint(const std::string &target, ryml::ConstNodeRef blueprint_data, c4::csubstr parent_path);

  ryml::Tree database;
  std::multimap<c4::csubstr, std::shared_ptr<blueprint>> blueprints;
  std::set<std::string> dependency_files;
  template_file_inputs template_inputs;
  // When false, dependency files are recorded on the match but not parsed (the Ninja engine reads them itself).
  bool expand_dependency_files = true;

private:
  struct render_context;
  std::unique_ptr<render_context> renderer;
  std::vector<std::shared_ptr<blueprint>> regex_blueprints;
  size_t next_blueprint_order = 0;
  ryml::Tree loaded_blueprints;
};

} // namespace yakka
