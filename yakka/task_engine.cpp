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
#include <unordered_map>

using namespace std::chrono_literals;

namespace yakka {

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
    if (task.num_successors() == 0)
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
  spdlog::info("{}: data", target_name);
  auto result = has_data_dependency_changed(target_name, project.previous_summary, project.project_summary);
  if (result) {
    construct_task->last_modified = *result ? fs::file_time_type::max() : fs::file_time_type::min();
  } else {
    spdlog::error("Data dependency '{}' error: {}", target_name, result.error());
    return;
  }
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
          auto result                   = engine.run_command(target_name, construct_task->match, project, project.project_summary["data"]);
          construct_task->last_modified = fs::file_time_type::clock::now();
          if (result.second != 0) {
            spdlog::info("Aborting: {} returned {}", target_name, result.second);
            engine.abort_build = true;
            return;
          }
        } catch (const std::exception &e) {
          spdlog::error("Error running command for {}: {}", target_name, e.what());
          engine.abort_build = true;
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
      if (!fs::exists(target_name) || max_element->second->last_modified.time_since_epoch() > construct_task->last_modified.time_since_epoch()) {
        spdlog::info("{}: Updating because of {}", target_name, max_element->first);
        try {
          auto [output, retcode]        = engine.run_command(target_name, construct_task->match, project, project.project_summary["data"]);
          construct_task->last_modified = fs::file_time_type::clock::now();
          if (retcode < 0) {
            spdlog::info("Aborting: {} returned {}", target_name, retcode);
            engine.abort_build = true;
            return;
          }
        } catch (const std::exception &e) {
          spdlog::error("Error running command for {}: {}", target_name, e.what());
          engine.abort_build = true;
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
  spdlog::info("Create tasks for: {}", target_name);

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
    return additional_data["values"].append_child() << blueprint->regex_matches[args[0].val<int>().value()];
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
    ryml::NodeRef aggregate = additional_data["values"].append_child();
    aggregate |= ryml::MAP;

    auto path = ryml::Pointer{ args[0].val() };
    // Loop through components, check if object path exists, if so add it to the aggregate
    for (const auto node: project.project_summary["components"].children()) {
      if (!node.contains(path) || !node[path].valid())
        continue;

      auto v = node[path];
      if (v.is_map())
        for (const auto i: v.children()) {
          aggregate[i.key()] = i.has_val() ? i.val() : nullptr; //try_render(inja_env, i.second.as<std::string>(), project->project_summary, log);
        }
      else if (v.is_seq())
        for (const auto i: v.children())
          if (i.is_map())
            aggregate.append_child() << i.val();
          else
            aggregate.append_child() << try_render(inja_env, i.val<std::string>().value(), project.project_summary);
      else if (v.valid())
        aggregate.append_child() << try_render(inja_env, v.val<std::string>().value(), project.project_summary);
    }

    // Check project data
    if (project.project_summary["data"].contains(path)) {
      auto v = project.project_summary["data"][path];
      if (v.is_map())
        for (const auto i: v.children())
          aggregate[i.key()] = i.has_val() ? i.val() : nullptr;
      else if (v.is_seq())
        for (const auto i: v.children())
          aggregate.append_child() << inja_env.render(i.val<std::string>().value(), project.project_summary);
      else
        aggregate.append_child() << inja_env.render(v.val<std::string>().value(), project.project_summary);
    }
    return aggregate;
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
    if (new_component.parse_file(component_path, package_path) == yakka::yakka_status::SUCCESS) {
      return new_component.root;
    } else {
      return ryml::NodeRef{};
    }
  });
  /// @brief Executes set_include_callback.

  inja_env.set_include_callback([&](const std::filesystem::path &path, const std::string &template_name) {
    const auto template_path = try_render(inja_env, template_name, project.project_summary);
    std::ifstream file;

    file.open(template_path);
    if (!file.fail()) {
      const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      return inja::Template(text);
    } else {
      spdlog::error("Failed to open template file: {}", template_name);
      return inja::Template();
    }
  });

  std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();

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

          command_text.append(tool.val<std::string>().value());

          std::string arg_text = command.val<std::string>().value();

          // Apply template engine
          arg_text = try_render(inja_env, arg_text, project.project_summary);

          auto [temp_output, temp_retcode] = exec(command_text, arg_text);
          retcode                          = temp_retcode;

          if (retcode != 0)
            spdlog::error("Returned {}\n{}", retcode, temp_output);
          if (retcode < 0)
            return { temp_output, retcode };

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
        }

        if (retcode < 0)
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
  ryml::Tree output;
  auto root = output.rootref();
  root |= ryml::MAP;
  root["version"] << 1;

  auto serialized_items = root["items"];
  serialized_items |= ryml::MAP;

  for (const auto &[target_name, construct_task]: todo_list) {
    auto item = serialized_items[target_name];
    item |= ryml::MAP;

    if (construct_task) {
      item["task_name"] << construct_task->task.name();
      item["task_hash"] << construct_task->task.hash_value();
      item["last_modified"] << static_cast<int64_t>(construct_task->last_modified.time_since_epoch().count());
      item["has_match"] << static_cast<int>(construct_task->match != nullptr);

      auto group_name = item["group_name"];
      if (construct_task->group) {
        group_name << ryml_string(construct_task->group->name);
        item["group_total_count"] << construct_task->group->total_count;
        item["group_current_count"] << construct_task->group->current_count.load();
      } else {
        group_name << "";
        item["group_total_count"] << 0;
        item["group_current_count"] << 0;
      }
      item["successors"] |= ryml::SEQ;
      construct_task->task.for_each_successor([&](tf::Task successor) {
        item["successors"].append_child() << successor.name();
      });
    } else {
      item["task_name"] << "";
      item["task_hash"] << 0;
      item["last_modified"] << 0;
      item["has_match"] << 0;
      item["group_name"] << "";
      item["group_total_count"] << 0;
      item["group_current_count"] << 0;
    }
  }

  std::ofstream file(filename, std::ios::binary | std::ios::trunc);
  if (!file) {
    spdlog::error("Failed to open todo list file '{}' for writing", filename.string());
    return;
  }

  file << ryml::emitrs_json<std::string>(output);
  if (!file)
    spdlog::error("Failed to write todo list file '{}'", filename.string());
}

std::shared_ptr<yakka::construction_task> task_engine::build_tasks_from_todo_list(const c4::yml::NodeRef &item, yakka::project &project, std::filesystem::file_time_type start_time)
{
  const auto target_name        = item.key();
  const auto target_name_string = ryml_string(target_name);
  const auto has_match          = item["has_match"].val<bool>();

  // If it has a match, retrieve the blueprint match from the project target database
  std::shared_ptr<blueprint_match> match = nullptr;
  if (has_match) {
    const auto &targets = project.target_database.get_target(target_name);
    for (const auto &i: targets) {
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
      if (!successor.has_val())
        continue;
      const auto successor_name = successor.val();
      if (successor_name.empty())
        continue;

      const auto successor_item = todo_list_tree.rootref()["items"][successor_name];

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

  todo_list_tree = ryml_load_file(filename).value();
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
  if (!version || *version != 1) {
    spdlog::error("Todo list file '{}' has an unsupported version", filename.string());
    return yakka_status::FAIL;
  }

  auto serialized_items = root["items"];
  if (!serialized_items.is_map()) {
    spdlog::error("Todo list file '{}' has an invalid items map", filename.string());
    return yakka_status::FAIL;
  }

  todo_list.clear();
  todo_task_groups.clear();
  todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");
  // auto finish                    = taskflow.emplace([&]() {
  //   // execution_progress = 100;
  // });
  // auto start_time = std::filesystem::file_time_type::clock::now();

  const auto is_valid_item = [&](const ryml::NodeRef &item) -> bool {
    return item.is_map() && item.has_key() && !item.key().empty();
  };

  const auto has_item_changed = [&](const ryml::NodeRef &item) -> bool {
    const auto target_name        = ryml_string(item.key());
    const auto last_modified      = fs::file_time_type{ fs::file_time_type::duration(item["last_modified"].val<int64_t>().value_or(0)) };
    const auto file_last_modified = fs::exists(target_name) ? fs::last_write_time(target_name) : fs::file_time_type::min();
    return file_last_modified > last_modified;
  };

  auto start_time = std::filesystem::file_time_type::clock::now();

  for (const auto serialized_item: serialized_items.children()) {
    if (!is_valid_item(serialized_item)) {
      spdlog::warn("Skipping invalid todo list item");
      continue;
    }
    if (!has_item_changed(serialized_item)) {
      continue;
    }

    build_tasks_from_todo_list(serialized_item, project, start_time);

    // const auto target_name        = serialized_item["target"].val();
    // const auto target_name_string = ryml_string(target_name);
    // const auto has_match          = serialized_item["has_match"].val<bool>();

    // if (!has_match) {
    //   auto construct_task = create_task_leaf_node(target_name, project, start_time);
    //   construct_task->task.precede(finish);
    // } else {
    //   auto construct_task = create_task_node(target_name, project, target_name_string);
    //   construct_task->task.precede(finish);
    // }

    // auto construct_task = std::make_shared<construction_task>();
    // if (serialized_item.has_child("task_name") && serialized_item["task_name"].has_val()) {
    //   const auto task_name = ryml_string(serialized_item["task_name"].val());
    //   construct_task->task = restore_task(task_name);
    // }

    // const auto last_modified = serialized_item.has_child("last_modified") ? serialized_item["last_modified"].val<int64_t>() : std::optional<int64_t>{};
    // if (last_modified)
    //   construct_task->last_modified = fs::file_time_type{ fs::file_time_type::duration(*last_modified) };

    // std::string group_name = "Processing";
    // if (serialized_item.has_child("group_name") && serialized_item["group_name"].has_val())
    //   group_name = ryml_string(serialized_item["group_name"].val());
    // if (group_name.empty())
    //   group_name = "Processing";

    // const auto group_key = [&]() {
    //   persistent_strings.emplace_back(group_name);
    //   const auto &value = persistent_strings.back();
    //   return ryml::csubstr{ value.data(), value.size() };
    // }();

    // if (!todo_task_groups.contains(group_key)) {
    //   auto group = std::make_shared<task_group>(group_key);
    //   if (serialized_item.has_child("group_total_count"))
    //     group->total_count = serialized_item["group_total_count"].val<int>().value_or(0);
    //   if (serialized_item.has_child("group_current_count"))
    //     group->current_count = serialized_item["group_current_count"].val<int>().value_or(0);
    //   todo_task_groups[group_key] = group;
    //   construct_task->group       = group;
    // } else {
    //   construct_task->group = todo_task_groups[group_key];
    // }

    // todo_list.emplace(target_key, construct_task);

    // if (serialized_item.has_child("successors") && serialized_item["successors"].is_seq()) {
    //   for (const auto serialized_successor: serialized_item["successors"].children()) {
    //     if (!serialized_successor.has_val())
    //       continue;
    //     const auto successor_name = ryml_string(serialized_successor.val());
    //     if (successor_name.empty())
    //       continue;
    //     const auto successor_task = restore_task(successor_name);
    //     if (!successor_task.empty())
    //       construct_task->task.precede(successor_task);
    //   }
    // }
  }

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

  for (const auto &task: tasks) {
    auto serialized_task = serialized_tasks.append_child();
    serialized_task |= ryml::MAP;
    serialized_task["name"] << task.name();

    auto successors = serialized_task["successors"];
    successors |= ryml::SEQ;
    task.for_each_successor([&](tf::Task successor) {
      successors.append_child() << task_ids.at(successor);
    });
  }

  std::ofstream file(filename, std::ios::binary | std::ios::trunc);
  if (!file) {
    spdlog::error("Failed to open taskflow file '{}' for writing", filename.string());
    return;
  }

  file << ryml::emitrs_json<std::string>(output);
  if (!file)
    spdlog::error("Failed to write taskflow file '{}'", filename.string());
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
  std::vector<tf::Task> tasks;
  tasks.reserve(serialized_tasks.num_children());

  for (const auto serialized_task: serialized_tasks.children()) {
    if (!serialized_task.is_map() || !serialized_task.has_child("name") || !serialized_task["name"].has_val() || !serialized_task.has_child("successors") || !serialized_task["successors"].is_seq()) {
      spdlog::error("Taskflow file '{}' contains an invalid task", filename.string());
      return yakka_status::FAIL;
    }

    tasks.push_back(loaded_taskflow.placeholder().name(ryml_string(serialized_task["name"].val())));
  }

  size_t task_id = 0;
  for (const auto serialized_task: serialized_tasks.children()) {
    std::unordered_set<size_t> seen_successors;
    for (const auto serialized_successor: serialized_task["successors"].children()) {
      const auto successor_id = serialized_successor.val<size_t>();
      if (!successor_id || *successor_id >= tasks.size() /*|| !seen_successors.insert(*successor_id).second*/) {
        spdlog::error("Taskflow file '{}' contains an invalid successor", filename.string());
        return yakka_status::FAIL;
      }
      tasks[task_id].precede(tasks[*successor_id]);
    }
    ++task_id;
  }

  taskflow = std::move(loaded_taskflow);
  todo_list.clear();
  todo_task_groups.clear();

  return yakka_status::SUCCESS;
}

} // namespace yakka
