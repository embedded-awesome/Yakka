/**
 * @file task_engine.cpp
 * @brief Implements task graph execution and build orchestration workflows.
 */

#include "yakka_project.hpp"
#include "task_engine.hpp"
#include "file_check.hpp"
#include "blueprint_commands.hpp"
#include "utilities.hpp"

#include <future>
#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace std::chrono_literals;

namespace yakka {
namespace {

void log_task_phase_time(const std::string &project_name, const char *phase, std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  spdlog::info("[{}] Timing {}: {:.3f} ms", project_name, phase, elapsed);
}

// Unlike merge_nodes, copies keys and values into dst's arena so dst stays valid after src's tree is destroyed
void copy_node(ryml::NodeRef dst, ryml::ConstNodeRef src)
{
  if (src.is_map() || src.is_seq()) {
    dst |= src.is_map() ? ryml::MAP : ryml::SEQ;
    for (const auto child: src.children()) {
      auto copy = dst.append_child();
      if (src.is_map()) {
        const auto child_key = child.key();
        copy << ryml::key(child_key);
      }
      copy_node(copy, child);
    }
  } else if (src.has_val()) {
    dst << src.val();
    // Hashes carried forward from todolists written before they were quoted must be quoted too
    if (src.is_val_quoted() || (src.has_key() && src.key() == "hash"))
      dst.tree()->_add_flags(dst.id(), ryml::VALQUO);
  }
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
  if (construct_task->match)
    for (const auto dependency: construct_task->match->dependencies) {
      const auto tasks = engine.todo_list.equal_range(dependency);
      for (auto task = tasks.first; task != tasks.second; ++task)
        if (task->second->force_rebuild)
          construct_task->force_rebuild = true;
    }
  // spdlog::info("{}: process --- {}", target_name, construct_task->task.hash_value());
  if (!construct_task->force_rebuild && construct_task->last_modified != fs::file_time_type::min()) {
    spdlog::info("{} already done", target_name);
    return;
  }
  if (fs::exists(target_name))
    construct_task->last_modified = fs::last_write_time(target_name);
  if (construct_task->match) {
    if (construct_task->match->dependencies.size() == 0) {
      if (construct_task->force_rebuild || !fs::exists(target_name)) {
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
      if (construct_task->force_rebuild || !fs::exists(target_name) || dependency_is_newer) {
        if (dependency_is_newer)
          spdlog::info("{}: Updating because of {}", target_name, max_element->first);
        else if (construct_task->force_rebuild)
          spdlog::info("{}: Updating because it is out of date", target_name);
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
  } else {
    construct_task->task.work([construct_task, target_name_string] {
      // A sub-project may create this file after the parent graph is prepared.
      if (fs::exists(target_name_string))
        update_file_timestamp(construct_task, target_name_string);
      else
        spdlog::info("Target {} has no action", target_name_string);
    });
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

namespace {

/// Register the blueprint-specific template callbacks ($, curdir, render, aggregate, load_component, include).
void setup_blueprint_environment(inja::Environment &inja_env, const std::shared_ptr<blueprint_match> &blueprint, const project &project, c4::csubstr &curdir_path)
{
  add_common_template_commands(inja_env);
  /// @brief Executes add_callback.

  inja_env.add_callback("$", 1, [blueprint](inja::Arguments &args, ryml::NodeRef additional_data) {
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
    std::string render_template = args[0].has_val() ? args[0].val<std::string>().value() : "";
    std::string render_output = try_render(inja_env, render_template, project.project_summary);
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

}

} // namespace

std::pair<std::string, int> task_engine::run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project, ryml::NodeRef project_data)
{
  ++commands_run;
  std::string captured_output = "";

  inja::Environment inja_env;
  auto curdir_path = blueprint->blueprint->parent_path;
  setup_blueprint_environment(inja_env, blueprint, project, curdir_path);

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
              spdlog::error("'{}' returned {} which is not an accepted success value for target '{}'\n{}", command_name, retcode, target, temp_output);
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

std::optional<std::string> task_engine::render_shell_process(const std::string &target, const std::shared_ptr<blueprint_match> &blueprint, const project &project)
{
#if defined(_WIN64) || defined(_WIN32) || defined(__CYGWIN__)
  return std::nullopt;
#else
  if (!blueprint || !blueprint->blueprint || !blueprint->blueprint->process.valid() || !blueprint->blueprint->process.is_seq())
    return std::nullopt;

  inja::Environment inja_env;
  auto curdir_path = blueprint->blueprint->parent_path;
  setup_blueprint_environment(inja_env, blueprint, project, curdir_path);

  std::vector<std::string> steps;
  try {
    for (const auto &command_entry: blueprint->blueprint->process.children()) {
      if (!command_entry.is_map() || command_entry.num_children() != 1 || !command_entry.child(0).has_key())
        return std::nullopt;
      const auto command      = command_entry.child(0);
      const auto command_name = command.key();
      if (project.project_summary["tools"].has_child(command_name)) {
        const auto tool = project.project_summary["tools"][command_name];
        std::string command_text;
        if (tool.is_map()) {
          if (tool.has_child("tool_return_success") || !tool.has_child("command") || !tool["command"].has_val())
            return std::nullopt;
          command_text = ryml_string(tool["command"].val());
        } else if (tool.has_val()) {
          command_text = ryml_string(tool.val());
        } else {
          return std::nullopt;
        }
        if (!command.has_val())
          return std::nullopt;
        const auto arg_text = inja_env.render(std::string_view(command.val().str, command.val().len), project.project_summary);
        steps.push_back(arg_text.empty() ? command_text : command_text + " " + arg_text);
      } else if (command_name == "execute" || command_name == "shell") {
        if (!command.has_val())
          return std::nullopt;
        steps.push_back(try_render(inja_env, ryml_string(command.val()), project.project_summary));
      } else if (command_name == "create_directory") {
        if (!command.has_val() || command.val().empty())
          continue;
        const fs::path directory = try_render(inja_env, ryml_string(command.val()), project.project_summary);
        // Ninja creates the parent directories of every output before running an edge.
        if (directory.empty() || directory.lexically_normal() == fs::path(target).lexically_normal() || directory.lexically_normal() == fs::path(target).parent_path().lexically_normal())
          continue;
        steps.push_back("mkdir -p '" + directory.generic_string() + "'");
      } else {
        return std::nullopt;
      }
    }
  } catch (const std::exception &e) {
    spdlog::debug("Cannot render '{}' as a shell command: {}", target, e.what());
    return std::nullopt;
  }

  if (steps.empty())
    return std::nullopt;
  std::string result;
  for (const auto &step: steps) {
    if (step.empty() || step.find_first_of("\r\n") != std::string::npos)
      return std::nullopt;
    if (!result.empty())
      result += " && ";
    result += steps.size() > 1 ? "(" + step + ")" : step;
  }
  return result;
#endif
}

/// @brief Executes run_taskflow.

void task_engine::create_taskflow(yakka::project &project)
{
  todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");
  /// @brief Executes emplace.

  finish_task = taskflow.emplace([&]() {
    // execution_progress = 100;
  });

  auto t1 = std::chrono::high_resolution_clock::now();

  for (auto &i: project.commands)
    create_tasks(i, finish_task, project);

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

bool task_engine::todo_list_checks_current() const
{
  if (todo_list_tree.empty())
    return false;
  const auto root = todo_list_tree.crootref();
  if (!root.is_map() || !root.has_child("file_checks") || !root.has_child("items") || !root["items"].is_seq())
    return false;
  for (const auto item: root["items"].children()) {
    if (!item.has_child("file_check"))
      continue;
    const auto check = item["file_check"];
    // Todolists written before hashes were quoted are rewritten once so they are strict JSON
    if (!file_check_settled(check) || (check.has_child("hash") && !check["hash"].is_val_quoted()))
      return false;
  }
  return true;
}

void task_engine::save_todolist(const std::filesystem::path &filename)
{
  if (todo_list.empty() && refresh_targets.empty() && todo_list_checks_current())
    return;

  struct saved_item {
    std::string target;
    int64_t last_modified = 0;
    bool has_match = false;
    std::vector<size_t> successors;
    ryml::ConstNodeRef file_check;
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
      item.has_match     = false;
      item.successors.clear();
      continue;
    }
    item.last_modified = static_cast<int64_t>(construct_task->last_modified.time_since_epoch().count());
    item.has_match     = construct_task->match != nullptr;
    item.successors.clear();
    task_indices.insert_or_assign(construct_task->task, item_index);
    current_tasks.emplace_back(item_index, construct_task->task);
  }
  log_task_phase_time(project_name, "todolist current target indexing", phase_start);

  // Carry targets skipped as up to date into the next snapshot.
  phase_start = std::chrono::steady_clock::now();
  std::vector<size_t> previous_index_to_item;
  std::vector<std::pair<size_t, ryml::ConstNodeRef>> carried_v2_items;
  std::vector<ryml::ConstNodeRef> carried_v1_items;
  if (!todo_list_tree.empty()) {
    const auto previous_root = todo_list_tree.rootref();
    if (previous_root.is_map() && previous_root.has_child("version") && previous_root.has_child("items")) {
      const auto previous_version     = previous_root["version"].val<int>().value_or(0);
      const auto previous_items       = previous_root["items"];
      const bool previous_items_valid = (previous_version == 1 && previous_items.is_map()) || (previous_version == 2 && previous_items.is_seq());
      if (previous_items_valid) {
        const auto carry_forward = [&](const ryml::ConstNodeRef &previous_item, size_t previous_index) {
          const auto previous_target = previous_version == 1 ? previous_item.key() : previous_item.has_child("target") ? previous_item["target"].val() : c4::csubstr{};
          if (previous_target.empty())
            return items.size();
          const auto target   = ryml_string(previous_target);
          const auto existing = item_indices.find(target);
          if (existing != item_indices.end())
            return existing->second;

          auto &item         = get_or_add_item(target);
          item.last_modified = previous_item.has_child("last_modified") ? previous_item["last_modified"].val<int64_t>().value_or(0) : 0;
          item.has_match     = previous_item.has_child("has_match") && previous_item["has_match"].val<bool>().value_or(false);
          item.file_check    = previous_item.find_child("file_check");
          if (previous_version == 1)
            carried_v1_items.push_back(previous_item);
          else
            carried_v2_items.emplace_back(previous_index, previous_item);
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
      // The internal completion task is not a persisted build target.
      if (successor == finish_task)
        return;
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
        for (const auto &[previous_index, previous_item]: carried_v2_items) {
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
  root["file_checks"] << file_check_policy;
  root["successful"] << !abort_build.load();

  auto serialized_items = root["items"];
  serialized_items |= ryml::SEQ;
  for (const auto &saved: items) {
    auto item = serialized_items.append_child();
    item |= ryml::MAP;
    item["target"] << saved.target;
    item["last_modified"] << saved.last_modified;
    item["has_match"] << static_cast<int>(saved.has_match);
    if (!saved.target.empty() && saved.target.front() != data_dependency_identifier) {
      if (saved.file_check.valid() && file_check_settled(saved.file_check) && !refresh_targets.contains(saved.target))
        copy_node(item["file_check"], saved.file_check);
      else
        record_file_check(item["file_check"], saved.target);
    }
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
  saved_todo_list_tree = std::move(output);
}

std::shared_ptr<yakka::construction_task> task_engine::build_tasks_from_todo_list(const c4::yml::NodeRef &item, yakka::project &project, std::filesystem::file_time_type start_time)
{
  std::vector<ryml::NodeRef> items;
  auto serialized_items = todo_list_tree.rootref()["items"];
  items.reserve(serialized_items.num_children());
  for (const auto serialized_item: serialized_items.children())
    items.push_back(serialized_item);
  return build_tasks_from_todo_list(item, project, start_time, items);
}

std::shared_ptr<construction_task> task_engine::build_tasks_from_todo_list(const ryml::NodeRef &item, project &project, std::filesystem::file_time_type start_time, const std::vector<ryml::NodeRef> &items)
{
  const auto version     = todo_list_tree.rootref()["version"].val<int>().value_or(1);
  const auto target_name = version == 1 ? item.key() : item["target"].val();
  return build_tasks_from_todo_list(item, project, start_time, items, !unforced_roots.contains(target_name));
}

std::shared_ptr<construction_task> task_engine::build_tasks_from_todo_list(const ryml::NodeRef &item, project &project, std::filesystem::file_time_type start_time, const std::vector<ryml::NodeRef> &items, bool forced)
{
  const auto version            = todo_list_tree.rootref()["version"].val<int>().value_or(1);
  const auto target_name        = version == 1 ? item.key() : item["target"].val();
  const auto target_name_string = ryml_string(target_name);
  const bool has_match          = item.has_child("has_match") && item["has_match"].val<bool>().value_or(false);
  // Changed roots and everything after them rebuild; targets reached only from unchanged sub-project outputs are left to the timestamp comparison.
  const bool force = has_match && forced;

  // Reuse an already reconstructed target. This also terminates recursion on shared or circular successors
  const auto existing_todo = todo_list.equal_range(target_name);
  if (existing_todo.first != existing_todo.second) {
    if (!forced || existing_todo.first->second->force_rebuild || !has_match)
      return existing_todo.first->second;
    // First reached from an unforced root; force it and everything after it now
    for (auto node = existing_todo.first; node != existing_todo.second; ++node)
      node->second->force_rebuild = true;
    if (item.has_child("successors") && item["successors"].is_seq())
      for (const auto successor: item["successors"].children()) {
        const auto successor_index = version == 2 ? successor.val<size_t>() : std::optional<size_t>{};
        const auto successor_item  = version == 1 ? todo_list_tree.rootref()["items"].find_child(successor.val()) : (successor_index && *successor_index < items.size() ? items[*successor_index] : ryml::NodeRef{});
        if (successor_item.valid())
          build_tasks_from_todo_list(successor_item, project, start_time, items, true);
      }
    return existing_todo.first->second;
  }

  // Create a node per blueprint match, as create_tasks does
  std::vector<std::shared_ptr<construction_task>> nodes;
  if (has_match) {
    const auto targets = project.target_database.targets.find(target_name);
    if (targets != project.target_database.targets.end())
      for (const auto &match: targets->second)
        if (match)
          nodes.push_back(create_task_node(target_name, project, match, target_name_string));
  }
  if (nodes.empty())
    nodes.push_back(create_task_leaf_node(target_name, project, start_time));
  for (const auto &node: nodes)
    node->force_rebuild = force;

  // Create parent nodes
  if (item.has_child("successors") && item["successors"].is_seq()) {
    for (const auto successor: item["successors"].children()) {
      ryml::NodeRef successor_item;
      if (version == 1) {
        if (!successor.has_val() || successor.val().empty())
          continue;
        successor_item = todo_list_tree.rootref()["items"].find_child(successor.val());
      } else {
        const auto successor_index = successor.val<size_t>();
        if (!successor_index || *successor_index >= items.size()) {
          spdlog::warn("Todo list successor index of '{}' is out of range", target_name);
          continue;
        }
        successor_item = items[*successor_index];
      }
      if (!successor_item.valid()) {
        spdlog::warn("Todo list successor of '{}' is missing", target_name);
        continue;
      }

      const auto successor_task = build_tasks_from_todo_list(successor_item, project, start_time, items, forced);
      if (!successor_task)
        continue;
      const auto successor_name = version == 1 ? successor_item.key() : successor_item["target"].val();
      const auto successor_nodes = todo_list.equal_range(successor_name);
      for (const auto &node: nodes)
        for (auto successor_node = successor_nodes.first; successor_node != successor_nodes.second; ++successor_node)
          node->task.precede(successor_node->second->task);
    }
  }
  return nodes.front();
}

yakka_status task_engine::create_dirty_taskflow(yakka::project &project, const std::filesystem::path &previous_todolist)
{
  const auto phase_start = std::chrono::steady_clock::now();
  taskflow.clear();
  todo_list.clear();
  todo_task_groups.clear();
  todo_list_tree.clear();
  create_taskflow(project);
  save_taskflow(previous_todolist.parent_path() / "taskflow.json");

  // Previous fingerprints by target, so unchanged parts of the new graph are skipped
  ryml::Tree previous;
  std::unordered_map<std::string, ryml::ConstNodeRef> previous_items;
  bool previous_successful = false;
  if (fs::exists(previous_todolist)) {
    if (auto loaded = ryml_load_file(previous_todolist)) {
      previous = std::move(*loaded);
      const auto root = previous.crootref();
      if (root.is_map() && root.has_child("version") && root["version"].val<int>().value_or(0) == 2 && root.has_child("items") && root["items"].is_seq()
          && root.has_child("file_checks") && root["file_checks"].val() == ryml::to_csubstr(file_check_policy)) {
        previous_successful = !root.has_child("successful") || root["successful"].val<bool>().value_or(false);
        for (const auto item: root["items"].children())
          if (item.is_map() && item.has_child("target") && item["target"].has_val())
            previous_items.emplace(ryml_string(item["target"].val()), item);
      }
    }
  }

  std::vector<std::string> targets;
  std::unordered_map<std::string, size_t> target_indices;
  std::unordered_map<tf::Task, size_t> task_indices;
  for (const auto &[target_name, node]: todo_list) {
    auto target                     = ryml_string(target_name);
    const auto [existing, inserted] = target_indices.emplace(target, targets.size());
    if (inserted)
      targets.push_back(std::move(target));
    task_indices.emplace(node->task, existing->second);
  }
  std::vector<std::vector<size_t>> successors(targets.size());
  std::vector<bool> has_match(targets.size(), false);
  for (const auto &[target_name, node]: todo_list) {
    const auto index = task_indices.at(node->task);
    has_match[index] = has_match[index] || node->match != nullptr;
    node->task.for_each_successor([&](tf::Task successor) {
      const auto found = task_indices.find(successor);
      if (found != task_indices.end() && std::find(successors[index].begin(), successors[index].end(), found->second) == successors[index].end())
        successors[index].push_back(found->second);
    });
  }

  ryml::Tree generated;
  auto root = generated.rootref();
  root |= ryml::MAP;
  root["version"] << 2;
  root["file_checks"] << file_check_policy;
  root["successful"] << previous_successful;
  auto items = root["items"];
  items |= ryml::SEQ;
  for (size_t index = 0; index < targets.size(); ++index) {
    auto item = items.append_child();
    item |= ryml::MAP;
    item["target"] << targets[index];
    const auto found = previous_items.find(targets[index]);
    item["last_modified"] << (found != previous_items.end() && found->second.has_child("last_modified") ? found->second["last_modified"].val<int64_t>().value_or(0) : int64_t{ 0 });
    item["has_match"] << static_cast<int>(has_match[index]);
    if (found != previous_items.end() && found->second.has_child("file_check"))
      copy_node(item["file_check"], found->second["file_check"]);
    item["successors"] |= ryml::SEQ;
    for (const auto successor: successors[index])
      item["successors"].append_child() << successor;
  }
  log_task_phase_time(project_name, "full graph creation", phase_start);

  taskflow.clear();
  todo_list.clear();
  todo_task_groups.clear();
  finish_task    = tf::Task{};
  todo_list_tree = std::move(generated);
  return restore_todolist(previous_todolist, project, false);
}

bool task_engine::is_project_dependency_output(ryml::csubstr target) const
{
  if (project_dependency_outputs.empty())
    return false;
  const auto path = fs::absolute(ryml_path(target)).lexically_normal().generic_string();
  return std::any_of(project_dependency_outputs.begin(), project_dependency_outputs.end(), [&](const std::string &folder) {
    return path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/';
  });
}

void task_engine::check_file_changes(const std::filesystem::path &filename)
{
  for (const auto &[target, task]: todo_list)
    if (!target.empty() && target.front() != data_dependency_identifier)
      task->force_rebuild = true;
  if (!fs::exists(filename))
    return;
  const auto previous = ryml_load_file(filename);
  if (!previous) {
    spdlog::warn("Cannot read previous file fingerprints from '{}'; rebuilding targets", filename.string());
    return;
  }
  const auto root = previous->crootref();
  if (!root.is_map() || !root.has_child("version") || root["version"].val<int>().value_or(0) != 2 || !root.has_child("items") || !root["items"].is_seq()) {
    spdlog::info("Previous todolist '{}' has no compatible file fingerprints; rebuilding targets", filename.string());
    return;
  }
  const bool successful = !root.has_child("successful") || root["successful"].val<bool>().value_or(false);
  for (const auto item: root["items"].children()) {
    if (!item.is_map() || !item.has_child("target") || !item["target"].has_val() || !item.has_child("file_check"))
      continue;
    const auto target = item["target"].val();
    if (target.empty() || target.front() == data_dependency_identifier)
      continue;
    const auto tasks = todo_list.equal_range(target);
    if (tasks.first == tasks.second)
      continue;
    const bool unchanged = file_check_matches(item["file_check"], ryml_path(target));
    for (auto task = tasks.first; task != tasks.second; ++task)
      task->second->force_rebuild = !unchanged || (!successful && task->second->match != nullptr);
  }
}

yakka_status task_engine::load_todolist(const std::filesystem::path &filename, yakka::project &project)
{
  if (!fs::exists(filename)) {
    return yakka_status::FILE_NOT_FOUND;
  }

  auto phase_start  = std::chrono::steady_clock::now();
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
  return restore_todolist(filename, project, true);
}

yakka_status task_engine::restore_todolist(const std::filesystem::path &filename, yakka::project &project, bool require_all_targets)
{
  auto phase_start = std::chrono::steady_clock::now();
  auto root        = todo_list_tree.rootref();
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

  // ryml child counts and positional lookups traverse siblings, so index once before visiting edges.
  std::vector<ryml::NodeRef> items;
  items.reserve(serialized_items.num_children());
  for (const auto item: serialized_items.children())
    items.push_back(item);

  // The todo list is the authoritative source of the graph it rebuilds below, so discard any tasks a previous load left behind
  taskflow.clear();
  todo_list.clear();
  todo_task_groups.clear();

  const auto item_target = [&](const ryml::NodeRef &item) {
    return *version == 1 ? item.key() : item["target"].val();
  };

  const auto has_item_changed = [&](const ryml::NodeRef &item) -> bool {
    const auto target = item_target(item);
    if (target.front() == data_dependency_identifier)
      return project.diff.has_data_changed(target);
    const auto target_name = ryml_string(target);
    const auto matches     = project.target_database.targets.find(target);
    if (matches != project.target_database.targets.end() && !matches->second.empty() && std::none_of(matches->second.begin(), matches->second.end(), [](const auto &match) {
          return match && match->blueprint && match->blueprint->process.valid();
        }))
      return false;
    if (item.has_child("file_check")) {
      const auto state = check_file(item["file_check"], target_name);
      if (state == file_check_state::refresh)
        refresh_targets.insert(target_name);
      return state == file_check_state::changed;
    }
    std::error_code filesystem_error;
    const auto file_last_modified = fs::last_write_time(target_name, filesystem_error);
    const bool target_exists      = !filesystem_error;
    if (!target_exists && item.has_child("has_match") && item["has_match"].val<bool>().value_or(false))
      return true;
    if (!item.has_child("last_modified"))
      return true;
    const auto previous_timestamp = item["last_modified"].val<int64_t>();
    if (!previous_timestamp)
      return true;
    const auto last_modified = fs::file_time_type{ fs::file_time_type::duration(*previous_timestamp) };
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
  items_to_restore.reserve(items.size());
  size_t scanned_items = 0;
  std::unordered_set<ryml::csubstr> scanned_targets;
  scanned_targets.reserve(items.size());
  unforced_roots.clear();
  refresh_targets.clear();
  for (const auto serialized_item: items) {
    if (!is_valid_item(serialized_item)) {
      spdlog::error("Todo list file '{}' has an invalid item", filename.string());
      return yakka_status::FAIL;
    }
    ++scanned_items;
    const auto target_name = item_target(serialized_item);
    if (!scanned_targets.insert(target_name).second || !serialized_item.has_child("successors") || !serialized_item["successors"].is_seq()) {
      spdlog::error("Todo list file '{}' has duplicate targets or invalid successors", filename.string());
      return yakka_status::FAIL;
    }
    for (const auto successor: serialized_item["successors"].children()) {
      if (!successor.has_val()) {
        spdlog::error("Todo list file '{}' has a non-scalar successor of '{}'", filename.string(), target_name);
        return yakka_status::FAIL;
      }
      const auto index = *version == 2 ? successor.val<size_t>() : std::optional<size_t>{};
      if ((*version == 2 && (!index || *index >= items.size())) || (*version == 1 && (!successor.has_val() || !serialized_items.has_child(successor.val())))) {
        spdlog::error("Todo list file '{}' has an invalid successor of '{}'", filename.string(), target_name);
        return yakka_status::FAIL;
      }
    }
    const auto matches            = project.target_database.targets.find(target_name);
    const bool passive_target     = matches != project.target_database.targets.end() && !matches->second.empty() && std::none_of(matches->second.begin(), matches->second.end(), [](const auto &match) {
                                  return match && match->blueprint && match->blueprint->process.valid();
                                    });
    const bool command_has_action = project.commands.contains(target_name) && (matches == project.target_database.targets.end() || std::any_of(matches->second.begin(), matches->second.end(), [](const auto &match) {
                                                                                 return match && match->blueprint && match->blueprint->process.valid();
                                                                               }));
    // Sub-projects build after preparation, so their outputs are restored unforced and compared by timestamp when the graph runs.
    if (target_name.front() != data_dependency_identifier && is_project_dependency_output(target_name)) {
      const bool changed = has_item_changed(serialized_item);
      items_to_restore.push_back(serialized_item);
      if (!changed)
        unforced_roots.insert(target_name);
      continue;
    }
    const bool retry_failed       = root.has_child("successful") && !root["successful"].val<bool>().value_or(false) && serialized_item.has_child("has_match") && serialized_item["has_match"].val<bool>().value_or(false);
    const bool changed       = has_item_changed(serialized_item);
    const bool uninitialized = !passive_target && has_uninitialized_timestamp(serialized_item) && target_name.front() != data_dependency_identifier;
    if (changed || command_has_action || retry_failed || uninitialized) {
      spdlog::debug("[{}] Restoring {}:{}{}{}{}", project_name, target_name, changed ? " changed" : "", command_has_action ? " command" : "", retry_failed ? " retry" : "", uninitialized ? " uninitialized" : "");
      items_to_restore.push_back(serialized_item);
    }
  }
  for (const auto command: project.commands)
    if (require_all_targets && !scanned_targets.contains(command)) {
      spdlog::error("Todo list file '{}' is missing command '{}'", filename.string(), command);
      return yakka_status::FAIL;
    }
  for (const auto &[target, matches]: project.target_database.targets)
    if (require_all_targets && !scanned_targets.contains(target)) {
      spdlog::error("Todo list file '{}' is missing target '{}'", filename.string(), target);
      return yakka_status::FAIL;
    }
  log_task_phase_time(project_name, "todolist freshness scan", phase_start);

  phase_start = std::chrono::steady_clock::now();
  if (!items_to_restore.empty())
    todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");
  for (const auto &serialized_item: items_to_restore)
    build_tasks_from_todo_list(serialized_item, project, start_time, items);
  unforced_roots.clear();
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

  finish_task = tf::Task{};
  taskflow = std::move(loaded_taskflow);
  todo_list.clear();
  todo_task_groups.clear();

  return yakka_status::SUCCESS;
}

} // namespace yakka
