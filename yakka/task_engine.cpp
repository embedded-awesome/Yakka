/**
 * @file task_engine.cpp
 * @brief Implements task graph execution and build orchestration workflows.
 */

#include "yakka_project.hpp"
#include "task_engine.hpp"
#include "blueprint_commands.hpp"
#include "utilities.hpp"

#include <future>
#include <chrono>
#include <set>
#include <unordered_map>

using namespace std::chrono_literals;

namespace yakka {
namespace {

void log_task_phase_time(const std::string &project_name, const char *phase, std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  spdlog::info("[{}] Timing {}: {:.3f} ms", project_name, phase, elapsed);
}

} // namespace

/// @brief Executes is_valid.

bool task_engine::is_valid()
{
  std::unordered_set<tf::Task> visited;

  std::unordered_set<tf::Task> recursion_stack;

  std::function<bool(tf::Task)> search_dependencies = [&](tf::Task task) {
    if (recursion_stack.contains(task))
      return true; // Found a cycle (back edge)
    if (visited.contains(task))
      return false; // Already processed in a previous DFS tree

    recursion_stack.insert(task);

    bool is_circular = false;
    /// @brief Executes for_each_dependent.

    task.for_each_dependent([&](tf::Task dependent) {
      if (search_dependencies(dependent))
        is_circular = true;
    });

    recursion_stack.erase(task);
    visited.insert(task);
    return is_circular;
  };

  bool is_circular = false;
  /// @brief Executes for_each_task.

  taskflow.for_each_task([&](tf::Task task) {
    if (search_dependencies(task))
      is_circular = true;
  });

  return !is_circular;
}

/// @brief Executes init.

void task_engine::init(task_complete_type task_complete_handler)
{
  this->task_complete_handler = task_complete_handler;
}

void task_engine::process_data_dependency(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, project &project, std::filesystem::file_time_type start_time)
{
  // spdlog::info("{}: data", target_name);
  construct_task->last_modified = project.diff.has_data_changed(c4::to_csubstr(target_name)) ? fs::file_time_type::max() : fs::file_time_type::min();
  if (construct_task->last_modified > start_time)
    spdlog::info("{} has been updated", target_name);
}

void task_engine::update_file_timestamp(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name)
{
  construct_task->last_modified = fs::last_write_time(target_name);
  // spdlog::info("{}: timestamp {}", target_name, (uint)construct_task->last_modified.time_since_epoch().count());
}

void task_engine::process_task(const std::shared_ptr<construction_task> &construct_task, const std::string &target_name, task_engine &engine, project &project)
{
  if (engine.abort_build)
    return;
  // spdlog::info("{}: process --- {}", target_name, construct_task->task.hash_value());
  if (construct_task->last_modified != fs::file_time_type::min()) {
    spdlog::info("{} already done", target_name);
    return;
  }
  if (fs::exists(target_name))
    construct_task->last_modified = fs::last_write_time(target_name);
  if (construct_task->match) {
    if (construct_task->match->dependencies.size() == 0) {
      if (!fs::exists(target_name)) {
        try {
          auto result = engine.run_command(target_name, construct_task->match, project, project.project_summary["data"]);
          if (result.second != 0) {
            spdlog::info("Aborting: {} returned {}", target_name, result.second);
            engine.abort_build = true;
            std::error_code ec;
            if (fs::is_regular_file(target_name, ec) && fs::file_size(target_name, ec) == 0) {
              fs::remove(target_name, ec);
            }
            return;
          }
          construct_task->last_modified = fs::file_time_type::clock::now();
        } catch (const std::exception &e) {
          spdlog::error("Error running command for {}: {}", target_name, e.what());
          engine.abort_build = true;
          std::error_code ec;
          if (fs::is_regular_file(target_name, ec) && fs::file_size(target_name, ec) == 0) {
            fs::remove(target_name, ec);
          }
          return;
        }
      }
    } else if (construct_task->match->blueprint->process.valid()) {
      auto max_element = engine.todo_list.end();
      for (auto j: construct_task->match->dependencies) {
        auto temp = engine.todo_list.equal_range(j);
        if (temp.first == temp.second) {
          continue;
        }
        auto temp_element = std::max_element(temp.first, temp.second, [](auto const &i, auto const &j) {
          return i.second->last_modified < j.second->last_modified;
        });
        if (max_element == engine.todo_list.end() || temp_element->second->last_modified > max_element->second->last_modified)
          max_element = temp_element;
      }
      // No dependency is in the todo list when none of them needed rebuilding, so the target is only out of date if it is missing
      const bool dependency_is_newer = max_element != engine.todo_list.end() && max_element->second->last_modified.time_since_epoch() > construct_task->last_modified.time_since_epoch();
      if (!fs::exists(target_name) || dependency_is_newer) {
        if (dependency_is_newer)
          spdlog::info("{}: Updating because of {}", target_name, max_element->first);
        else
          spdlog::info("{}: Updating because it is missing", target_name);
        try {
          auto [output, retcode] = engine.run_command(target_name, construct_task->match, project, project.project_summary["data"]);
          if (retcode != 0) {
            spdlog::info("Aborting: {} returned {}", target_name, retcode);
            engine.abort_build = true;
            std::error_code ec;
            if (fs::is_regular_file(target_name, ec) && fs::file_size(target_name, ec) == 0) {
              fs::remove(target_name, ec);
            }
            return;
          }
          construct_task->last_modified = fs::file_time_type::clock::now();
        } catch (const std::exception &e) {
          spdlog::error("Error running command for {}: {}", target_name, e.what());
          engine.abort_build = true;
          std::error_code ec;
          if (fs::is_regular_file(target_name, ec) && fs::file_size(target_name, ec) == 0) {
            fs::remove(target_name, ec);
          }
          return;
        }
      }
    }
  }
#if USING_THE_OLD_TASK_COMPLETE_HANDLER
  if (engine.task_complete_handler)
    engine.task_complete_handler(construct_task->group);
#else
  ++construct_task->group->current_count;
#endif
}

std::shared_ptr<construction_task> task_engine::create_task_leaf_node(ryml::csubstr target_name, yakka::project &project, std::filesystem::file_time_type start_time)
{
  // spdlog::info("Creating node {}: leaf node", target_name);
  auto construct_task = std::make_shared<construction_task>();
  todo_list.insert(std::make_pair(target_name, construct_task));
  const std::string target_name_string = ryml_string(target_name);
  construct_task->task                 = taskflow.placeholder().name(target_name_string);

  // Check if target is a data dependency
  if (target_name.front() == data_dependency_identifier) {
    construct_task->task.work(std::function<void()>(std::bind(&task_engine::process_data_dependency, construct_task, target_name_string, std::ref(project), start_time)));
  }
  // Check if target name matches an existing file in filesystem
  else if (fs::exists(target_name_string)) {
    // Create a new task to retrieve the file timestamp
    construct_task->task.work(std::function<void()>(std::bind(&task_engine::update_file_timestamp, construct_task, target_name_string)));
  } else {
    spdlog::info("Target {} has no action", target_name_string);
  }
  // construct_task->task.precede(parent);
  // new_todo->second.task = task;
  return construct_task;
}

std::shared_ptr<construction_task> task_engine::create_task_node(ryml::csubstr target_name, yakka::project &project, std::shared_ptr<blueprint_match> i, const std::string &target_name_string)
{
  // spdlog::info("Creating node {}: Not a leaf node", target_name);
  // ++work_task_count;
  auto construct_task = std::make_shared<construction_task>();
  todo_list.insert(std::make_pair(target_name, construct_task));
  construct_task->match = i;

  if (i->blueprint->task_group.empty()) {
    construct_task->group = todo_task_groups["Processing"];
  } else {
    if (todo_task_groups.contains(i->blueprint->task_group))
      construct_task->group = todo_task_groups[i->blueprint->task_group];
    else {
      construct_task->group                      = std::make_shared<yakka::task_group>(i->blueprint->task_group);
      todo_task_groups[i->blueprint->task_group] = construct_task->group;
    }
  }
  ++construct_task->group->total_count;

  construct_task->task = taskflow.placeholder().name(target_name_string);
  construct_task->task.work(std::function<void()>(std::bind(&task_engine::process_task, construct_task, target_name_string, std::ref(*this), std::ref(project))));
  // new_todo->second.task = task;

  return construct_task;
}

void task_engine::create_tasks(ryml::csubstr target_name, tf::Task &parent, yakka::project &project)
{
  const std::string target_name_string = ryml_string(target_name);

  // XXX: Start time should be determined at the start of the executable and not here
  auto start_time = std::filesystem::file_time_type::clock::now();

  if (target_name.empty()) {
    spdlog::error("Empty target name");
  }
  // spdlog::info("Create tasks for: {}", target_name);

  // Check if this target has already been processed
  const auto &existing_todo = todo_list.equal_range(target_name);
  if (existing_todo.first != existing_todo.second) {
    // Add parent to the dependency graph
    for (auto i = existing_todo.first; i != existing_todo.second; ++i)
      i->second->task.precede(parent);

    // Nothing else to do
    return;
  }

  // Get targets that match the name
  const auto &targets = project.target_database.get_target(target_name);
  // const auto &targets = project.target_database.targets.equal_range(target_name);

  // If there is no targets then it must be a leaf node (source file, data dependency, etc)
  if (targets.empty()) {
    auto construct_task = create_task_leaf_node(target_name, project, start_time);
    construct_task->task.precede(parent);
  } else {
    for (const auto &i: targets) {
      auto construct_task = create_task_node(target_name, project, i, target_name_string);
      construct_task->task.precede(parent);

      // For each dependency described in blueprint, retrieve or create task, add relationship, and add item to todo list
      if (i)
        for (auto &dep_target: i->dependencies)
          create_tasks(dep_target.first(2) == "./" ? dep_target.sub(dep_target.first_not_of('/', 2)) : dep_target, construct_task->task, project);
      // else
      //     spdlog::info("{} does not have blueprint match", i->first);
    }
  }
}

std::pair<std::string, int> task_engine::run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project, ryml::NodeRef project_data)
{
  std::string captured_output = "";

  inja::Environment inja_env;
  auto curdir_path = blueprint->blueprint->parent_path;

  add_common_template_commands(inja_env);
  /// @brief Executes add_callback.

  inja_env.add_callback("$", 1, [&blueprint](inja::Arguments &args, ryml::NodeRef additional_data) {
    const auto index = args[0].val<int>().value();
    if (index < 0 || static_cast<size_t>(index) >= blueprint->regex_matches.size())
      throw std::out_of_range("blueprint capture index is out of range");
    return additional_data["values"].append_child() << blueprint->regex_matches[index];
  });

  /// @brief Executes add_callback.

  inja_env.add_callback("curdir", 0, [&](inja::Arguments &args, ryml::NodeRef additional_data) {
    return additional_data["values"].append_child() << curdir_path;
  });

  /// @brief Executes add_callback.

  inja_env.add_callback("render", 1, [&](inja::Arguments &args, ryml::NodeRef additional_data) {
    return additional_data["values"].append_child() << try_render(inja_env, args[0].val<std::string>().value(), project.project_summary);
  });

  /// @brief Executes add_callback.

  inja_env.add_callback("render", 2, [&curdir_path, &inja_env, &project](inja::Arguments &args, ryml::NodeRef additional_data) {
    auto backup = curdir_path;
    curdir_path = args[1].val();

    std::string render_output = try_render(inja_env, args[0].val<std::string>().value(), project.project_summary);
    curdir_path               = backup;
    return additional_data["values"].append_child() << render_output;
  });

  /// @brief Executes add_callback.

  inja_env.add_callback("aggregate", 1, [&](inja::Arguments &args, ryml::NodeRef additional_data) {
    const std::string path = args[0].val<std::string>().value();
    return aggregate_blueprint_data(inja_env, project.project_summary, c4::to_csubstr(path),
                                    additional_data["values"].append_child());
  });
  /// @brief Executes add_callback.

  inja_env.add_callback("load_component", 1, [&](inja::Arguments &args, ryml::NodeRef additional_data) {
    // const auto component_name     = args[0].val<std::string>().value();
    const auto component_location = project.workspace.find_component(args[0].val());

    if (!component_location.has_value()) {
      return ryml::NodeRef{};
    }
    auto [component_path, package_path] = component_location.value();
    yakka::component new_component;
    auto output = additional_data["values"].append_child();
    if (new_component.parse_file(component_path, package_path, output) == yakka::yakka_status::SUCCESS) {
      return output;
    } else {
      return ryml::NodeRef{};
    }
  });
  /// @brief Executes set_include_callback.

  inja_env.set_include_callback([&](const std::filesystem::path &path, const std::string &template_name) {
    const auto template_path = inja_env.render(template_name, project.project_summary);
    std::ifstream file;

    file.open(template_path);
    if (!file.fail()) {
      const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      return inja_env.parse(text);
    } else {
      spdlog::error("Failed to open template file: {}", template_name);
      throw inja::FileError("Failed to open template file: " + template_name);
    }
  });

  std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();

  if (blueprint->blueprint->process.valid() && !blueprint->blueprint->process.is_seq()) {
    spdlog::error("Process for target '{}' must be a sequence", target);
    return { "", -1 };
  }

  // Note: A blueprint process is a sequence of maps
  if (blueprint->blueprint->process.valid() && blueprint->blueprint->process.is_seq())
    for (const auto &command_entry: blueprint->blueprint->process.children()) {
      if (!command_entry.is_map() || command_entry.num_children() != 1) {
        spdlog::error("Command entry for target '{}' is malformed", target);
        return { "", -1 };
      }

      // Take the first entry in the map as the command
      auto command = command_entry.child(0);
      if (!command.has_key()) {
        spdlog::error("Command entry for target '{}' is missing a key", target);
        return { "", -1 };
      }

      // std::string command_name;
      // c4::from_chars(command.key(), &command_name);
      const auto command_name = command.key();
      int retcode             = 0;

      try {
        if (project.project_summary["tools"].has_child(command_name)) {
          auto tool                = project.project_summary["tools"][command_name];
          std::string command_text = "";

          if (tool.is_map()) {
            if (!tool.has_child("command") || !tool["command"].has_val()) {
              spdlog::error("Tool '{}' is missing its command", command_name);
              return { "", -1 };
            }
            command_text = tool["command"].val<std::string>().value();
          } else {
            command_text = tool.val<std::string>().value();
          }

          std::string arg_text = command.val<std::string>().value();

          // Apply template engine
          try {
            arg_text = inja_env.render(arg_text, project.project_summary);
          } catch (const std::exception &e) {
            spdlog::error("Failed to render tool arguments '{}': {}", arg_text, e.what());
            return { "", -1 };
          }

          auto [temp_output, temp_retcode] = exec(command_text, arg_text);
          retcode                          = temp_retcode;

          // If the tool declares a set of acceptable return values, enforce it
          if (tool.is_map() && tool.has_child("tool_return_success") ) {
            const auto success_values = tool["tool_return_success"];
            bool is_success           = false;
            for (const auto &value_node: success_values.children())
              if (value_node.val<int>().value_or(0) == retcode) {
                is_success = true;
                break;
              }
            if (!is_success) {
              spdlog::error("'{}' returned {} which is not an accepted success value for target '{}'", command_name, retcode, target);
              return { temp_output, -1 };
            }
            retcode = 0;
          } else if (retcode != 0) {
            spdlog::error("Returned {}\n{}", retcode, temp_output);
            return { temp_output, retcode };
          }

          captured_output = temp_output;
          // Echo the output of the command
          // TODO: Note this should be done by the main thread to ensure the outputs from multiple run_command instances don't overlap
          spdlog::info(captured_output);
        }
        // Else check if it is a built-in command
        else if (blueprint_commands.contains(ryml_string(command_name))) {
          yakka::process_return test_result = blueprint_commands.at(ryml_string(command_name))(target, command, captured_output, project.project_summary, project_data, inja_env);
          captured_output                   = test_result.result;
          retcode                           = test_result.retcode;
        } else {
          spdlog::error("{} tool doesn't exist", command_name);
          return { captured_output, -1 };
        }

        if (retcode != 0)
          return { captured_output, retcode };
      } catch (std::exception &e) {
        spdlog::error("Failed to run command: '{}' as part of {}", command_name, target);
        spdlog::error("{}", e.what());
        throw e;
      }
    }

  std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
  auto duration                                     = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}: {} milliseconds", target, duration);
  return { captured_output, 0 };
}

/// @brief Executes run_taskflow.

void task_engine::create_taskflow(yakka::project &project)
{
  todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");
  /// @brief Executes emplace.

  auto finish = taskflow.emplace([&]() {
    // execution_progress = 100;
  });

  auto t1 = std::chrono::high_resolution_clock::now();

  for (auto &i: project.commands)
    create_tasks(i, finish, project);

#ifdef DEBUG_TASK_ENGINE
  std::ofstream graph_file("task_engine_graph.txt");
  if (graph_file.is_open()) {
    taskflow.dump(graph_file);
    graph_file.close();
  } else {
    spdlog::error("Failed to open task_engine_graph.txt for writing");
  }
#endif

  auto t2       = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to create tasks", duration);
}

void task_engine::run_taskflow(yakka::project &project, task_engine_ui *ui)
{
  tf::Executor executor(std::min(32U, std::thread::hardware_concurrency()));

  auto t1 = std::chrono::high_resolution_clock::now();
  if (!is_valid()) {
    spdlog::error("Blueprints have circular dependency");
    return;
  }
  auto t2       = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to validate task graph", duration);

  ui->init(*this);

  auto execution_future = executor.run(taskflow);

  do {
    ui->update(*this);
  } while (execution_future.wait_for(500ms) != std::future_status::ready);

  ui->finish(*this);
}

void task_engine::save_todolist(const std::filesystem::path &filename)
{
  struct saved_item {
    std::string target;
    int64_t last_modified = 0;
    bool has_match = false;
    std::vector<size_t> successors;
  };

  const auto construction_start = std::chrono::steady_clock::now();
  std::vector<saved_item> items;
  std::unordered_map<std::string, size_t> item_indices;
  std::unordered_map<tf::Task, size_t> task_indices;
  std::vector<std::pair<size_t, tf::Task>> current_tasks;
  const auto get_or_add_item = [&](const std::string &target) -> saved_item & {
    const auto found = item_indices.find(target);
    if (found != item_indices.end())
      return items[found->second];
    const auto index = items.size();
    item_indices.emplace(target, index);
    items.push_back({ target });
    return items.back();
  };

  auto phase_start = std::chrono::steady_clock::now();
  for (const auto &[target_name, construct_task]: todo_list) {
    const auto target = ryml_string(target_name);
    auto &item = get_or_add_item(target);
    const auto item_index = item_indices.at(target);
    if (!construct_task) {
      item.last_modified = 0;
      item.has_match = false;
      item.successors.clear();
      continue;
    }
    item.last_modified = static_cast<int64_t>(construct_task->last_modified.time_since_epoch().count());
    item.has_match = construct_task->match != nullptr;
    item.successors.clear();
    task_indices.insert_or_assign(construct_task->task, item_index);
    current_tasks.emplace_back(item_index, construct_task->task);
  }
  log_task_phase_time(project_name, "todolist current target indexing", phase_start);

  // Carry targets skipped as up to date into the next snapshot.
  phase_start = std::chrono::steady_clock::now();
  std::vector<size_t> previous_index_to_item;
  std::vector<size_t> carried_v2_items;
  std::vector<ryml::ConstNodeRef> carried_v1_items;
  if (!todo_list_tree.empty()) {
    const auto previous_root = todo_list_tree.rootref();
    if (previous_root.is_map() && previous_root.has_child("version") && previous_root.has_child("items")) {
      const auto previous_version = previous_root["version"].val<int>().value_or(0);
      const auto previous_items = previous_root["items"];
      const bool previous_items_valid = (previous_version == 1 && previous_items.is_map()) ||
                                        (previous_version == 2 && previous_items.is_seq());
      if (previous_items_valid) {
        const auto carry_forward = [&](const ryml::ConstNodeRef &previous_item, size_t previous_index) {
          const auto previous_target = previous_version == 1 ? previous_item.key() :
                                       previous_item.has_child("target") ? previous_item["target"].val() : c4::csubstr{};
          if (previous_target.empty())
            return items.size();
          const auto target = ryml_string(previous_target);
          const auto existing = item_indices.find(target);
          if (existing != item_indices.end())
            return existing->second;

          auto &item = get_or_add_item(target);
          item.last_modified = previous_item.has_child("last_modified")
                                 ? previous_item["last_modified"].val<int64_t>().value_or(0) : 0;
          item.has_match = previous_item.has_child("has_match") &&
                           previous_item["has_match"].val<bool>().value_or(false);
          if (previous_version == 1)
            carried_v1_items.push_back(previous_item);
          else
            carried_v2_items.push_back(previous_index);
          return item_indices.at(target);
        };

        if (previous_version == 2)
          previous_index_to_item.resize(previous_items.num_children(), previous_items.num_children());
        size_t previous_index = 0;
        for (const auto previous_item: previous_items.children()) {
          const auto output_index = carry_forward(previous_item, previous_index);
          if (previous_version == 2)
            previous_index_to_item[previous_index] = output_index;
          ++previous_index;
        }
      }
    }
  }
  log_task_phase_time(project_name, "todolist carry-forward indexing", phase_start);

  phase_start = std::chrono::steady_clock::now();
  for (const auto &[item_index, task]: current_tasks) {
    task.for_each_successor([&](tf::Task successor) {
      const auto successor_index = task_indices.find(successor);
      if (successor_index != task_indices.end()) {
        items[item_index].successors.push_back(successor_index->second);
        return;
      }
      const auto successor_name = successor.name();
      const auto persisted_index = item_indices.find(successor_name);
      if (persisted_index != item_indices.end())
        items[item_index].successors.push_back(persisted_index->second);
      else
        spdlog::warn("Todo list successor '{}' of '{}' is missing while saving", successor_name, items[item_index].target);
    });
  }

  if (!todo_list_tree.empty()) {
    const auto previous_root = todo_list_tree.rootref();
    if (previous_root.is_map() && previous_root.has_child("version") && previous_root.has_child("items")) {
      const auto previous_version = previous_root["version"].val<int>().value_or(0);
      const auto previous_items = previous_root["items"];
      if (previous_version == 1) {
        for (const auto &previous_item: carried_v1_items) {
          if (!previous_item.has_child("successors") || !previous_item["successors"].is_seq())
            continue;
          const auto previous_target = ryml_string(previous_item.key());
          const auto item_index = item_indices.find(previous_target);
          if (item_index == item_indices.end())
            continue;
          for (const auto successor: previous_item["successors"].children()) {
            if (!successor.has_val() || successor.val().empty())
              continue;
            const auto successor_index = item_indices.find(ryml_string(successor.val()));
            if (successor_index != item_indices.end())
              items[item_index->second].successors.push_back(successor_index->second);
          }
        }
      } else if (previous_version == 2) {
        for (const auto previous_index: carried_v2_items) {
          const auto previous_item = previous_items.child(previous_index);
          if (!previous_item.has_child("successors") || !previous_item["successors"].is_seq())
            continue;
          const auto output_index = previous_index_to_item[previous_index];
          if (output_index >= items.size())
            continue;
          for (const auto successor: previous_item["successors"].children()) {
            const auto successor_index = successor.val<size_t>();
            if (!successor_index || *successor_index >= previous_index_to_item.size())
              continue;
            const auto mapped_index = previous_index_to_item[*successor_index];
            if (mapped_index < items.size())
              items[output_index].successors.push_back(mapped_index);
          }
        }
      }
    }
  }
  log_task_phase_time(project_name, "todolist successor indexing", phase_start);
  log_task_phase_time(project_name, "todolist save record construction", construction_start);

  ryml::Tree output;
  auto root = output.rootref();
  root |= ryml::MAP;
  root["version"] << 2;

  auto serialized_items = root["items"];
  serialized_items |= ryml::SEQ;
  for (const auto &saved: items) {
    auto item = serialized_items.append_child();
    item |= ryml::MAP;
    item["target"] << saved.target;
    item["last_modified"] << saved.last_modified;
    item["has_match"] << static_cast<int>(saved.has_match);
    item["successors"] |= ryml::SEQ;
    for (const auto successor_index: saved.successors)
      item["successors"].append_child() << successor_index;
  }

  const auto serialization_start = std::chrono::steady_clock::now();
  const auto serialized = ryml::emitrs_json<std::string>(output);
  log_task_phase_time(project_name, "todolist JSON serialization", serialization_start);
  const auto write_start = std::chrono::steady_clock::now();
  write_file_if_changed(filename, serialized);
  log_task_phase_time(project_name, "todolist compare/write", write_start);
}

std::shared_ptr<yakka::construction_task> task_engine::build_tasks_from_todo_list(const c4::yml::NodeRef &item, yakka::project &project, std::filesystem::file_time_type start_time)
{
  const auto version = todo_list_tree.rootref()["version"].val<int>().value_or(1);
  const auto target_name = version == 1 ? item.key() : item["target"].val();
  const auto target_name_string = ryml_string(target_name);

  // Reuse an already reconstructed target. This also terminates recursion on shared or circular successors
  const auto existing_todo = todo_list.equal_range(target_name);
  if (existing_todo.first != existing_todo.second)
    return existing_todo.first->second;

  const bool has_match = item.has_child("has_match") && item["has_match"].val<bool>().value_or(false);

  // If it has a match, retrieve the blueprint match from the project target database
  std::shared_ptr<blueprint_match> match = nullptr;
  if (has_match) {
    const auto targets = project.target_database.targets.find(target_name);
    if (targets != project.target_database.targets.end())
      for (const auto &i: targets->second) {
        if (i) {
          match = i;
          break;
        }
      }
  }

  // Create a node for the current match
  auto construct_task = (!has_match || match == nullptr) ? create_task_leaf_node(target_name, project, start_time) : create_task_node(target_name, project, match, target_name_string);

  // Create parent nodes
  if (item.has_child("successors") && item["successors"].is_seq()) {
    for (const auto successor: item["successors"].children()) {
      auto items          = todo_list_tree.rootref()["items"];
      ryml::NodeRef successor_item;
      if (version == 1) {
        if (!successor.has_val() || successor.val().empty())
          continue;
        successor_item = items.find_child(successor.val());
      } else {
        const auto successor_index = successor.val<size_t>();
        if (!successor_index || *successor_index >= items.num_children()) {
          spdlog::warn("Todo list successor index of '{}' is out of range", target_name);
          continue;
        }
        successor_item = items.child(*successor_index);
      }
      if (!successor_item.valid()) {
        spdlog::warn("Todo list successor of '{}' is missing", target_name);
        continue;
      }

      const auto successor_task = build_tasks_from_todo_list(successor_item, project, start_time);
      if (successor_task)
        construct_task->task.precede(successor_task->task);
    }
  }
  return construct_task;
}

yakka_status task_engine::load_todolist(const std::filesystem::path &filename, yakka::project &project)
{
  if (!fs::exists(filename)) {
    return yakka_status::FILE_NOT_FOUND;
  }

  auto phase_start = std::chrono::steady_clock::now();
  auto file_content = get_file_contents<std::string>(filename);
  log_task_phase_time(project_name, "todolist file read", phase_start);
  if (!file_content) {
    spdlog::error("Failed to read todo list file '{}': {}", filename.string(), file_content.error().message());
    return yakka_status::FAIL;
  }

  phase_start = std::chrono::steady_clock::now();
  try {
    todo_list_tree = ryml::parse_in_arena(ryml::to_csubstr(*file_content));
  } catch (const std::exception &error) {
    spdlog::error("Failed to parse todo list file '{}': {}", filename.string(), error.what());
    return yakka_status::FAIL;
  } catch (...) {
    spdlog::error("Failed to parse todo list file '{}'", filename.string());
    return yakka_status::FAIL;
  }
  log_task_phase_time(project_name, "todolist JSON parse", phase_start);

  if (todo_list_tree.empty()) {
    spdlog::error("Failed to load todo list file '{}'", filename.string());
    return yakka_status::FAIL;
  }

  auto root = todo_list_tree.rootref();
  if (!root.is_map() || !root.has_child("version") || !root.has_child("items")) {
    spdlog::error("Todo list file '{}' has an invalid root", filename.string());
    return yakka_status::FAIL;
  }

  const auto version = root["version"].val<int>();
  if (!version || (*version != 1 && *version != 2)) {
    spdlog::error("Todo list file '{}' has an unsupported version", filename.string());
    return yakka_status::FAIL;
  }

  auto serialized_items = root["items"];
  if ((*version == 1 && !serialized_items.is_map()) || (*version == 2 && !serialized_items.is_seq())) {
    spdlog::error("Todo list file '{}' has an invalid items collection", filename.string());
    return yakka_status::FAIL;
  }

  // The todo list is the authoritative source of the graph it rebuilds below, so discard any tasks a previous load left behind
  taskflow.clear();
  todo_list.clear();
  todo_task_groups.clear();
  todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");

  const auto item_target = [&](const ryml::NodeRef &item) {
    return *version == 1 ? item.key() : item["target"].val();
  };

  const auto has_item_changed = [&](const ryml::NodeRef &item) -> bool {
    const auto target_name = ryml_string(item_target(item));
    std::error_code filesystem_error;
    const auto file_last_modified = fs::last_write_time(target_name, filesystem_error);
    const bool target_exists = !filesystem_error;
    if (!target_exists && item.has_child("has_match") && item["has_match"].val<bool>().value_or(false))
      return true;
    const auto last_modified = fs::file_time_type{ fs::file_time_type::duration(item["last_modified"].val<int64_t>().value_or(0)) };
    return target_exists && file_last_modified > last_modified;
  };

  const auto is_valid_item = [&](const ryml::NodeRef &item) -> bool {
    if (!item.is_map())
      return false;
    if (*version == 1)
      return item.has_key() && !item.key().empty();
    return item.has_child("target") && item["target"].has_val() && !item["target"].val().empty();
  };

  const auto has_uninitialized_timestamp = [&](const ryml::NodeRef &item) -> bool {
    if (!item.has_child("last_modified"))
      return true;
    const auto last_modified = item["last_modified"].val<int64_t>().value_or(0);
    return last_modified == 0 || last_modified == -1;
  };

  auto start_time = std::filesystem::file_time_type::clock::now();

  phase_start = std::chrono::steady_clock::now();
  std::vector<ryml::NodeRef> items_to_restore;
  items_to_restore.reserve(serialized_items.num_children());
  size_t scanned_items = 0;
  for (const auto serialized_item: serialized_items.children()) {
    if (!is_valid_item(serialized_item)) {
      spdlog::warn("Skipping invalid todo list item");
      continue;
    }
    ++scanned_items;
    const auto target_name = item_target(serialized_item);
    if (has_item_changed(serialized_item) || project.commands.contains(target_name) || has_uninitialized_timestamp(serialized_item))
      items_to_restore.push_back(serialized_item);
  }
  log_task_phase_time(project_name, "todolist freshness scan", phase_start);

  phase_start = std::chrono::steady_clock::now();
  for (const auto &serialized_item: items_to_restore)
    build_tasks_from_todo_list(serialized_item, project, start_time);
  log_task_phase_time(project_name, "todolist graph reconstruction", phase_start);
  spdlog::info("[{}] Todolist entries scanned: {}, graph roots restored: {}", project_name, scanned_items, items_to_restore.size());

  return yakka_status::SUCCESS;
}

void task_engine::save_taskflow(const std::filesystem::path &filename)
{
  ryml::Tree output;
  auto root = output.rootref();
  root |= ryml::MAP;
  root["version"] << 1;

  auto serialized_tasks = root["tasks"];
  serialized_tasks |= ryml::SEQ;

  std::vector<tf::Task> tasks;
  std::unordered_map<tf::Task, size_t> task_ids;
  taskflow.for_each_task([&](tf::Task task) {
    task_ids.emplace(task, tasks.size());
    tasks.push_back(task);
  });

  std::vector<std::vector<size_t>> predecessors_by_task(tasks.size());
  for (size_t source_id = 0; source_id < tasks.size(); ++source_id) {
    const auto &source_task = tasks[source_id];
    source_task.for_each_successor([&](tf::Task successor) {
      const auto successor_id = task_ids.at(successor);
      predecessors_by_task[successor_id].push_back(source_id);
    });
  }

  for (size_t task_id = 0; task_id < tasks.size(); ++task_id) {
    const auto &task = tasks[task_id];
    auto serialized_task = serialized_tasks.append_child();
    serialized_task |= ryml::MAP;
    serialized_task["name"] << task.name();

    auto predecessors = serialized_task["predecessors"];
    predecessors |= ryml::SEQ;
    for (const auto predecessor_id: predecessors_by_task[task_id]) {
      predecessors.append_child() << predecessor_id;
    }

    auto successors = serialized_task["successors"];
    successors |= ryml::SEQ;
    task.for_each_successor([&](tf::Task successor) {
      successors.append_child() << task_ids.at(successor);
    });
  }

  write_file_if_changed(filename, ryml::emitrs_json<std::string>(output));
}

yakka_status task_engine::load_taskflow(const std::filesystem::path &filename)
{
  if (!fs::exists(filename)) {
    return yakka_status::FILE_NOT_FOUND;
  }
  auto serialized = ryml_load_file(filename);
  if (!serialized) {
    spdlog::error("Failed to load taskflow file '{}': {}", filename.string(), serialized.error().message());
    return yakka_status::FAIL;
  }

  auto root = serialized->rootref();
  if (!root.is_map() || !root.has_child("version") || !root.has_child("tasks")) {
    spdlog::error("Taskflow file '{}' has an invalid root", filename.string());
    return yakka_status::FAIL;
  }

  const auto version = root["version"].val<int>();
  if (!version || *version != 1) {
    spdlog::error("Taskflow file '{}' has an unsupported version", filename.string());
    return yakka_status::FAIL;
  }

  auto serialized_tasks = root["tasks"];
  if (!serialized_tasks.is_seq()) {
    spdlog::error("Taskflow file '{}' has an invalid tasks list", filename.string());
    return yakka_status::FAIL;
  }

  tf::Taskflow loaded_taskflow;
  std::vector<ryml::NodeRef> serialized_task_nodes;
  std::vector<tf::Task> tasks;
  tasks.reserve(serialized_tasks.num_children());

  for (const auto serialized_task: serialized_tasks.children()) {
    if (!serialized_task.is_map() || !serialized_task.has_child("name") || !serialized_task["name"].has_val()) {
      spdlog::error("Taskflow file '{}' contains an invalid task", filename.string());
      return yakka_status::FAIL;
    }

    if (serialized_task.has_child("successors") && !serialized_task["successors"].is_seq()) {
      spdlog::error("Taskflow file '{}' contains a non-sequence successor list", filename.string());
      return yakka_status::FAIL;
    }

    if (serialized_task.has_child("predecessors") && !serialized_task["predecessors"].is_seq()) {
      spdlog::error("Taskflow file '{}' contains a non-sequence predecessor list", filename.string());
      return yakka_status::FAIL;
    }

    serialized_task_nodes.push_back(serialized_task);
    tasks.push_back(loaded_taskflow.placeholder().name(ryml_string(serialized_task["name"].val())));
  }

  std::set<std::pair<size_t, size_t>> seen_edges;
  const auto add_edge = [&](size_t from, size_t to) -> bool {
    if (from >= tasks.size() || to >= tasks.size()) {
      return false;
    }
    if (!seen_edges.insert({from, to}).second) {
      return true;
    }
    tasks[from].precede(tasks[to]);
    return true;
  };

  for (size_t task_id = 0; task_id < serialized_task_nodes.size(); ++task_id) {
    const auto &serialized_task = serialized_task_nodes[task_id];
    if (serialized_task.has_child("successors") && serialized_task["successors"].is_seq()) {
      for (const auto serialized_successor: serialized_task["successors"].children()) {
        const auto successor_id = serialized_successor.val<size_t>();
        if (!successor_id || *successor_id >= tasks.size()) {
          spdlog::error("Taskflow file '{}' contains an invalid successor", filename.string());
          return yakka_status::FAIL;
        }
        if (!add_edge(task_id, *successor_id)) {
          spdlog::error("Taskflow file '{}' contains an invalid edge", filename.string());
          return yakka_status::FAIL;
        }
      }
    }

    if (serialized_task.has_child("predecessors") && serialized_task["predecessors"].is_seq()) {
      for (const auto serialized_predecessor: serialized_task["predecessors"].children()) {
        const auto predecessor_id = serialized_predecessor.val<size_t>();
        if (!predecessor_id || *predecessor_id >= tasks.size()) {
          spdlog::error("Taskflow file '{}' contains an invalid predecessor", filename.string());
          return yakka_status::FAIL;
        }
        if (!add_edge(*predecessor_id, task_id)) {
          spdlog::error("Taskflow file '{}' contains an invalid edge", filename.string());
          return yakka_status::FAIL;
        }
      }
    }
  }

  taskflow = std::move(loaded_taskflow);
  todo_list.clear();
  todo_task_groups.clear();

  return yakka_status::SUCCESS;
}

} // namespace yakka
