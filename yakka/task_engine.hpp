#pragma once

#include "yakka.hpp"
#include "yakka_project.hpp"
#include "blueprint_database.hpp"
#include "taskflow.hpp"
#include "json.hpp"
#include <string>
#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <map>

namespace yakka {

class task_engine;

struct task_group {
  std::string name;
  int total_count;
  std::atomic<int> current_count;
  size_t ui_id;
  int last_progress_update;

  task_group(const std::string name) : name(name)
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
	void init(task_engine& task_engine) {};
	void update(task_engine& task_engine) {};
	void finish(task_engine& task_engine) {};
};

class task_engine {
public:
  task_engine()  = default;
  ~task_engine() = default;

  typedef std::function<void(std::shared_ptr<task_group> group)> task_complete_type;

  void init(task_complete_type task_complete_handler);
  void create_tasks(const std::string target_name, tf::Task &parent, yakka::project &project);
  std::pair<std::string, int> run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project);
  void run_taskflow(yakka::project &project, task_engine_ui& ui);

  std::atomic<bool> abort_build;
  nlohmann::json project_data;
  tf::Taskflow taskflow;

  task_complete_type task_complete_handler;
  std::multimap<std::string, construction_task> todo_list;
  std::map<std::string, std::shared_ptr<task_group>> todo_task_groups;
};
} // namespace yakka
