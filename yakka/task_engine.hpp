#pragma once

#include "yakka.hpp"
#include "blueprint_database.hpp"
#include "taskflow.hpp"
#include "ryml.hpp"
#include "ryml_std.hpp"
#include <string>
#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <map>
#include <mutex>

namespace yakka {

class task_engine;
class project;

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
  static void process_data_dependency(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, task_engine &engine, project &project, std::filesystem::file_time_type start_time);
  static void update_file_timestamp(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name);
  static void process_task(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, task_engine &engine, project &project);

public:
  task_engine()  = default;
  ~task_engine() = default;

  typedef std::function<void(std::shared_ptr<task_group> group)> task_complete_type;

  void init(task_complete_type task_complete_handler);
  void create_taskflow(yakka::project &project);
  void create_tasks(ryml::csubstr target_name, tf::Task &parent, yakka::project &project);
  std::pair<std::string, int> run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project, ryml::NodeRef project_data);
  void run_taskflow(yakka::project &project, task_engine_ui *ui);
  bool is_valid();
  void mark_target_for_refresh(const ryml::csubstr target_name);

  void save_taskflow(const std::filesystem::path &filename);
  yakka_status load_taskflow(const std::filesystem::path &filename);
  void save_todolist(const std::filesystem::path &filename);
  yakka_status load_todolist(const std::filesystem::path &filename, yakka::project &project);
  std::shared_ptr<construction_task> create_task_leaf_node(ryml::csubstr target_name, yakka::project &project, std::filesystem::file_time_type start_time);
  std::shared_ptr<construction_task> create_task_node(ryml::csubstr target_name, yakka::project &project, std::shared_ptr<blueprint_match> i, const std::string &target_name_string);
  std::shared_ptr<yakka::construction_task> build_tasks_from_todo_list(const c4::yml::NodeRef &serialized_item, yakka::project &project, std::filesystem::file_time_type start_time);

  std::atomic<bool> abort_build;
  ryml::Tree project_data;
  tf::Taskflow taskflow;

  // Blueprint commands run in parallel (via taskflow worker threads) but they all read
  // and mutate the single shared project.project_summary/project_data ryml::Tree.
  // ryml::Tree is not thread-safe for concurrent mutation (even of disjoint subtrees,
  // since appends can trigger shared node-pool/arena reallocation), so any access to
  // that tree from run_command() must be serialized with this mutex.
  std::mutex project_data_mutex;

  task_complete_type task_complete_handler;
  std::multimap<ryml::csubstr, std::shared_ptr<construction_task>> todo_list;
  std::map<ryml::csubstr, std::shared_ptr<task_group>> todo_task_groups;

  ryml::Tree todo_list_tree;
  ryml::Tree taskflow_tree;
};
} // namespace yakka
