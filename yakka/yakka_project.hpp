#pragma once

#include "yakka.hpp"
#include "yakka_component.hpp"
#include "yakka_workspace.hpp"
#include "component_database.hpp"
#include "target_database.hpp"
#include "blueprint_database.hpp"
//#include "yaml-cpp/yaml.h"
#include "nlohmann/json.hpp"
#include "inja.hpp"
#include "spdlog/spdlog.h"
#include "indicators/progress_bar.hpp"
#include "taskflow.hpp"
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
    PROJECT_VALID
  };

public:
  project(const std::string project_name, yakka::workspace &workspace);

  virtual ~project();

  void set_project_directory(const std::string path);
  void init_project(std::vector<std::string> components, std::vector<std::string> features, std::unordered_set<std::string> commands = {});
  void init_project(const std::string build_string);
  void init_project();
  void process_build_string(const std::string build_string);
  void parse_project_string(const std::vector<std::string> &project_string);
  void process_requirements(std::shared_ptr<yakka::component> component, nlohmann::json child_node);
  state evaluate_dependencies();
  bool add_component(const std::string &component_name, component_database::flag flags);
  bool add_feature(const std::string &feature_name);
  //std::optional<fs::path> find_component(const std::string component_dotname);
  void evaluate_choices();
  void add_additional_tool(const fs::path component_path);

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
  std::vector<std::string> initial_components;
  std::vector<std::string> initial_features;
  yakka::project::state current_state;

  // Component processing
  std::unordered_set<std::string> unprocessed_components;
  std::unordered_set<std::string> unprocessed_features;
  std::unordered_set<std::string> unprocessed_choices;
  std::unordered_map<std::string, std::string> unprocessed_replacements;
  //std::unordered_set<std::string> replaced_components;
  std::unordered_map<std::string, std::string> replacements;
  std::unordered_set<std::string> required_components;
  std::unordered_set<std::string> required_features;
  std::unordered_set<std::string> provided_features;
  std::unordered_set<std::string> unprovided_features;
  std::map<std::string, const nlohmann::json> feature_recommendations;
  std::unordered_set<std::string> additional_tools;
  std::unordered_set<std::string> commands;
  std::unordered_set<std::string> unknown_components;
  std::vector<std::pair<std::string, std::string>> incomplete_choices;
  std::vector<std::string> multiple_answer_choices;
  component_database::flag component_flags;
  bool project_has_slcc;

  YAML::Node project_summary_yaml;
  std::string project_directory;
  std::string project_summary_file;
  fs::path project_file;
  fs::file_time_type project_summary_last_modified;
  std::vector<std::shared_ptr<yakka::component>> components;
  //yakka::component_database component_database;
  yakka::blueprint_database blueprint_database;
  yakka::target_database target_database;

  nlohmann::json previous_summary;
  nlohmann::json project_summary;

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

  // SLC specific
  nlohmann::json template_contributions;
  std::unordered_set<std::string> slc_required;
  std::unordered_set<std::string> slc_provided;
  std::map<std::string, const nlohmann::json> slc_recommended;
  std::multimap<std::string, std::string> instances;
  std::multimap<std::string, const std::shared_ptr<yakka::component>> slc_overrides;
  bool is_disqualified_by_unless(const nlohmann::json &node);
  bool condition_is_fulfilled(const nlohmann::json &node);
  void process_slc_rules();
  void create_config_file(const std::shared_ptr<yakka::component> component, const nlohmann::json &config, const std::string &prefix, std::string instance_name);
};

} /* namespace yakka */
