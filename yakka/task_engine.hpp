#pragma once
#include <optional>

#include "yakka.hpp"
#include "yakka_project.hpp"
#include "blueprint_database.hpp"
#include "taskflow.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <string>
#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <map>
#include <unordered_set>
#include <vector>

namespace yakka {

class task_engine;

struct task_group {
  ryml::csubstr name;
  int total_count;
  std::atomic<int> current_count;
  size_t ui_id;
  int last_progress_update;

  task_group(const ryml::csubstr name) : name(name)
  {
    total_count          = 0;
    current_count        = 0;
    ui_id                = 0;
    last_progress_update = 0;
  }
};

struct construction_task {
  std::shared_ptr<blueprint_match> match;
  std::filesystem::file_time_type last_modified;
  tf::Task task;
  std::shared_ptr<task_group> group;
  bool force_rebuild = false;

  construction_task() : match(nullptr), last_modified(fs::file_time_type::min())
  {
  }
};

struct task_engine_ui {
  virtual void init(task_engine &task_engine)   = 0;
  virtual void update(task_engine &task_engine) = 0;
  virtual void finish(task_engine &task_engine) = 0;
};

class task_engine {
private:
  tf::Task finish_task;

  static void process_data_dependency(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, project &project, std::filesystem::file_time_type start_time);
  static void update_file_timestamp(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name);
  static void process_task(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, task_engine &engine, project &project);
  std::shared_ptr<construction_task> build_tasks_from_todo_list(const ryml::NodeRef &item, project &project, std::filesystem::file_time_type start_time, const std::vector<ryml::NodeRef> &items);
  std::shared_ptr<construction_task> build_tasks_from_todo_list(const ryml::NodeRef &item, project &project, std::filesystem::file_time_type start_time, const std::vector<ryml::NodeRef> &items, bool forced);

public:
  task_engine()  = default;
  ~task_engine() = default;

  typedef std::function<void(std::shared_ptr<task_group> group)> task_complete_type;

  void init(task_complete_type task_complete_handler);
  void create_taskflow(yakka::project &project);
  void create_tasks(ryml::csubstr target_name, tf::Task &parent, yakka::project &project);
  std::pair<std::string, int> run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project, ryml::NodeRef project_data);
  /// Render a process made only of tool/execute/shell/create_directory steps into a single shell command.
  /// Returns nullopt when a step needs Yakka at build time (built-in commands, tool return-code rules, multi-line commands).
  static std::optional<std::string> render_shell_process(const std::string &target, const std::shared_ptr<blueprint_match> &blueprint, const project &project);
  void run_taskflow(yakka::project &project, task_engine_ui *ui);
  bool is_valid();

  void save_taskflow(const std::filesystem::path &filename);
  yakka_status load_taskflow(const std::filesystem::path &filename);
  void save_todolist(const std::filesystem::path &filename);
  /// True when the loaded todolist has fingerprints for this policy, none still need rehashing, and every hash is quoted.
  bool todo_list_checks_current() const;
  yakka_status load_todolist(const std::filesystem::path &filename, yakka::project &project);
  /// Build the full graph from the target database, then keep only targets that changed since the previous todolist and their successors.
  yakka_status create_dirty_taskflow(yakka::project &project, const std::filesystem::path &previous_todolist);
  /// Restore the graph of changed targets from todo_list_tree. Loaded todolists must cover every command and target.
  yakka_status restore_todolist(const std::filesystem::path &filename, yakka::project &project, bool require_all_targets);
  /// Absolute output folders of sub-projects; their files are compared by timestamp when the graph runs.
  std::vector<std::string> project_dependency_outputs;
  /// Apply previous file fingerprints to a newly generated graph without reusing its old topology.
  void check_file_changes(const std::filesystem::path &filename);
  std::shared_ptr<construction_task> create_task_leaf_node(ryml::csubstr target_name, yakka::project &project, std::filesystem::file_time_type start_time);
  std::shared_ptr<construction_task> create_task_node(ryml::csubstr target_name, yakka::project &project, std::shared_ptr<blueprint_match> i, const std::string &target_name_string);
  std::shared_ptr<yakka::construction_task> build_tasks_from_todo_list(const c4::yml::NodeRef &serialized_item, yakka::project &project, std::filesystem::file_time_type start_time);

  std::atomic<bool> abort_build{ false };
  std::string project_name;
  bool include_project_name_in_progress = true;
  ryml::Tree project_data;
  tf::Taskflow taskflow;

  task_complete_type task_complete_handler;
  std::multimap<ryml::csubstr, std::shared_ptr<construction_task>> todo_list;
  std::map<ryml::csubstr, std::shared_ptr<task_group>> todo_task_groups;

  ryml::Tree todo_list_tree;
  ryml::Tree taskflow_tree;
  /// The todolist written by the last save_todolist, or empty when the loaded todo_list_tree was already current.
  ryml::Tree saved_todo_list_tree;
  /// Number of blueprint processes run, used to tell a no-op build.
  std::atomic<size_t> commands_run{ 0 };

private:
  bool is_project_dependency_output(ryml::csubstr target) const;
  std::unordered_set<ryml::csubstr> unforced_roots;
  /// Targets whose content is unchanged but whose recorded stat is stale, so saving records them again.
  std::unordered_set<std::string> refresh_targets;
};
} // namespace yakka
