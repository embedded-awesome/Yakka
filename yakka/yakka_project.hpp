#pragma once

#include "yakka.hpp"
#include "yakka_component.hpp"
#include "yakka_workspace.hpp"
#include "component_database.hpp"
#include "target_database.hpp"
#include "blueprint_database.hpp"
#include "yakka_schema.hpp"
//#include "yaml-cpp/yaml.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include "inja.hpp"
#include "spdlog/spdlog.h"
#include "indicators/progress_bar.hpp"
#include "taskflow.hpp"
#include "utilities.hpp"
#include <filesystem>
#include <regex>
#include <map>
#include <unordered_set>
#include <optional>
#include <functional>

namespace fs = std::filesystem;

namespace yakka {

class project {
public:
  enum class state {
    PROJECT_HAS_UNKNOWN_COMPONENTS,
    PROJECT_HAS_REMOTE_COMPONENTS,
    PROJECT_HAS_INVALID_COMPONENT,
    PROJECT_HAS_MULTIPLE_REPLACEMENTS,
    PROJECT_HAS_INCOMPLETE_CHOICES,
    PROJECT_HAS_MULTIPLE_ANSWERS_FOR_CHOICES,
    PROJECT_HAS_UNRESOLVED_REQUIREMENTS,
    PROJECT_HAS_FAILED_SCHEMA_CHECK,
    PROJECT_VALID
  };

public:
  project(yakka::workspace &workspace, const std::string project_name = "");

  virtual ~project();

  void set_project_directory(const std::string path);
  void init_project(std::vector<ryml::csubstr> components, std::vector<ryml::csubstr> features, std::unordered_set<ryml::csubstr> commands = {});
  void init_project(const std::string build_string);
  void init_project(const std::vector<std::string> build_string_list);
  void init_project();
  void process_build_string(const std::string build_string);
  void parse_project_string(const std::vector<std::string> &project_string);
  void process_requirements(std::shared_ptr<yakka::component> component, ryml::ConstNodeRef child_node);
  state evaluate_dependencies();
  bool add_component(std::string &component_name, component_database::flag flags);
  bool add_component(c4::csubstr component_name, component_database::flag flags);
  bool add_feature(c4::csubstr &feature_name);
  //std::optional<std::filesystem::path> find_component(const std::string component_dotname);
  void evaluate_choices();
  project::state process_choice(c4::csubstr &choice_name);
  void add_additional_tool(const std::filesystem::path component_path);
  void add_command(const std::string command);

  // Component processing functions
  void process_tools(const std::shared_ptr<component> c);
  void process_blueprints(const std::shared_ptr<component> c);

  void process_blueprints();
  void update_summary();
  void generate_project_summary();

  // Target database management
  void generate_target_database();

  void create_project_file();
  void process_construction(indicators::ProgressBar &bar);
  void save_summary();
  void save_blueprints();

  void validate_schema();
  void update_project_data();

  // void add_required_component(std::shared_ptr<yakka::component> component);
  // void add_required_feature(const std::string feature, std::shared_ptr<yakka::component> component);

  // Basic project data
  std::string project_name;
  std::filesystem::path output_path;
  std::string yakka_home_directory;
  std::vector<c4::csubstr> initial_components;
  std::vector<c4::csubstr> initial_features;
  yakka::project::state current_state;

  // Component processing
  std::unordered_set<c4::csubstr> unprocessed_components;
  std::unordered_set<c4::csubstr> unprocessed_features;
  std::unordered_set<c4::csubstr> unprocessed_choices;
  std::unordered_map<c4::csubstr, c4::csubstr> unprocessed_replacements;
  //std::unordered_set<c4::csubstr> replaced_components;
  std::unordered_map<c4::csubstr, c4::csubstr> replacements;
  std::unordered_set<c4::csubstr> required_components;
  std::unordered_set<c4::csubstr> required_features;
  std::unordered_set<c4::csubstr> provided_features;
  std::unordered_set<c4::csubstr> unprovided_features;
  std::map<c4::csubstr, ryml::ConstNodeRef> feature_recommendations;
  std::unordered_set<c4::csubstr> additional_tools;
  std::unordered_set<c4::csubstr> commands;
  std::unordered_set<c4::csubstr> unknown_components;
  std::vector<std::pair<c4::csubstr, c4::csubstr>> incomplete_choices;
  std::vector<c4::csubstr> multiple_answer_choices;
  component_database::flag component_flags;
  bool project_has_slcc;

  YAML::Node project_summary_yaml;
  std::string project_directory;
  std::filesystem::path project_summary_file;
  std::filesystem::path project_file;
  fs::file_time_type project_summary_last_modified;
  std::vector<std::shared_ptr<yakka::component>> components;
  //yakka::component_database component_database;
  yakka::blueprint_database blueprint_database;
  yakka::target_database target_database;

  ryml::Tree project_data;
  ryml::NodeRef previous_summary;
  ryml::NodeRef project_summary;

  yakka::schema project_schema;
  yakka::schema data_schema;

  yakka::workspace &workspace;

  // Blueprint evaluation
  inja::Environment inja_environment;
  //std::multimap<std::string, std::shared_ptr<blueprint_match> > target_database;
  // std::multimap<std::string, construction_task> todo_list;
  // int work_task_count;
  // std::map<std::string, std::shared_ptr<task_group>> todo_task_groups;

  // tf::Taskflow taskflow;
  // std::atomic<bool> abort_build;

  // std::function<void(std::shared_ptr<task_group> group)> task_complete_handler;

  // Internal helper objects
  const ryml::Pointer _requires_components_pointer = ryml::Pointer("/requires/components");
  const ryml::Pointer _requires_features_pointer = ryml::Pointer("/requires/features");
  const ryml::Pointer _provides_features_pointer = ryml::Pointer("/provides/features");
  const ryml::Pointer _supports_components_pointer = ryml::Pointer("/supports/components");
  const ryml::Pointer _supports_features_pointer = ryml::Pointer("/supports/features");
  const ryml::Pointer _replaces_components_pointer = ryml::Pointer("/replaces/components");

  // SLC specific
  // ryml::Tree template_contributions;
  ryml::NodeRef template_contributions;
  std::unordered_set<c4::csubstr> slc_required;
  std::unordered_set<c4::csubstr> slc_provided;
  std::map<c4::csubstr, ryml::ConstNodeRef> slc_recommended;
  std::multimap<c4::csubstr, c4::csubstr> instances;
  std::multimap<c4::csubstr, const std::shared_ptr<yakka::component>> slc_overrides;
  bool is_disqualified_by_unless(ryml::ConstNodeRef node);
  bool condition_is_fulfilled(ryml::ConstNodeRef node);
  void process_slc_rules();
  void create_config_file(const std::shared_ptr<yakka::component> component, ryml::ConstNodeRef config, const std::string &prefix, std::string instance_name);
};

} /* namespace yakka */
