#include "yakka_project.hpp"
#include "task_engine.hpp"
#include "blueprint_commands.hpp"
#include "utilities.hpp"
#include <future>
#include <chrono>

using namespace std::chrono_literals;

namespace yakka {

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
    task.for_each_dependent([&](tf::Task dependent) {
      if (search_dependencies(dependent))
        is_circular = true;
    });

    recursion_stack.erase(task);
    visited.insert(task);
    return is_circular;
  };

  bool is_circular = false;
  taskflow.for_each_task([&](tf::Task task) {
    if (task.num_successors() == 0)
      if (search_dependencies(task))
        is_circular = true;
  });

  return !is_circular;
}

void task_engine::init(task_complete_type task_complete_handler)
{
  this->task_complete_handler = task_complete_handler;
}

void task_engine::create_tasks(ryml::csubstr target_name, tf::Task &parent, yakka::project &project)
{
  const std::string target_name_string = ryml_string(target_name);
  // XXX: Start time should be determined at the start of the executable and not here
  auto start_time = std::filesystem::file_time_type::clock::now();

  if (target_name.empty()) {
    spdlog::error("Empty target name");
  }
  //spdlog::info("Create tasks for: {}", target_name);

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
    //spdlog::info("{}: leaf node", target_name);
    auto construct_task = std::make_shared<construction_task>();
    todo_list.insert(std::make_pair(target_name, construct_task));
    construct_task->task = taskflow.placeholder().name(target_name_string);

    // Check if target is a data dependency
    if (target_name.front() == data_dependency_identifier) {
      construct_task->task.work([&, construct_task, target_name_string]() {
        // spdlog::info("{}: data", target_name);
        // auto d     = static_cast<std::shared_ptr<construction_task>>(task.data());
        auto result = has_data_dependency_changed(target_name_string, project.previous_summary, project.project_summary);
        if (result) {
          construct_task->last_modified = *result ? fs::file_time_type::max() : fs::file_time_type::min();
        } else {
          spdlog::error("Data dependency '{}' error: {}", target_name_string, result.error());
          return;
        }
        if (construct_task->last_modified > start_time)
          spdlog::info("{} has been updated", target_name_string);
        return;
      });
    }
    // Check if target name matches an existing file in filesystem
    else if (fs::exists(target_name_string)) {
      // Create a new task to retrieve the file timestamp
      construct_task->task.work([construct_task, target_name_string]() {
        // uint8_t hash[32];
        // hash_file(target_name, hash);
        construct_task->last_modified = fs::last_write_time(target_name_string);
        //spdlog::info("{}: timestamp {}", target_name, (uint)d->last_modified.time_since_epoch().count());
        return;
      });
    } else {
      spdlog::info("Target {} has no action", target_name_string);
    }
    construct_task->task.precede(parent);
    // new_todo->second.task = task;
    return;
  }

  for (const auto &i: targets) {
    // spdlog::info("{}: Not a leaf node", target_name);
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

    construct_task->task.work([construct_task, target_name_string, this, i, &project]() {
      if (abort_build)
        return;
      // spdlog::info("{}: process --- {}", target_name, task.hash_value());
      if (construct_task->last_modified != fs::file_time_type::min()) {
        // I don't think this event happens. This check can probably be removed
        spdlog::info("{} already done", target_name_string);
        return;
      }
      if (fs::exists(target_name_string)) {
        construct_task->last_modified = fs::last_write_time(target_name_string);
        // spdlog::info("{}: timestamp {}", target_name, (uint)d->last_modified.time_since_epoch().count());
      }
      if (construct_task->match) {
        // Check if there are no dependencies
        if (construct_task->match->dependencies.size() == 0) {
          // If it doesn't exist as a file, run the command
          if (!fs::exists(target_name_string)) {
            try {
              auto result                   = run_command(target_name_string, construct_task->match, project, project.project_summary["data"]);
              construct_task->last_modified = fs::file_time_type::clock::now();
              if (result.second != 0) {
                spdlog::info("Aborting: {} returned {}", target_name_string, result.second);
                abort_build = true;
                return;
              }
            } catch (const std::exception &e) {
              spdlog::error("Error running command for {}: {}", target_name_string, e.what());
              abort_build = true;
              return;
            }
          }
        } else if (construct_task->match->blueprint->process.valid()) {
          auto max_element = todo_list.end();
          for (auto j: construct_task->match->dependencies) {
            auto temp         = todo_list.equal_range(j);
            auto temp_element = std::max_element(temp.first, temp.second, [](auto const &i, auto const &j) {
              return i.second->last_modified < j.second->last_modified;
            });
            //spdlog::info("{}: Check max element {}: {} vs {}", target_name, temp_element->first, (int64_t)temp_element->second.last_modified.time_since_epoch().count(), (int64_t)max_element->second.last_modified.time_since_epoch().count());
            if (max_element == todo_list.end() || temp_element->second->last_modified > max_element->second->last_modified) {
              max_element = temp_element;
            }
          }
          //spdlog::info("{}: Max element is {}", target_name, max_element->first);
          if (!fs::exists(target_name_string) || max_element->second->last_modified.time_since_epoch() > construct_task->last_modified.time_since_epoch()) {
            spdlog::info("{}: Updating because of {}", target_name_string, max_element->first);
            try {
              auto [output, retcode]        = run_command(target_name_string, construct_task->match, project, project.project_summary["data"]);
              construct_task->last_modified = fs::file_time_type::clock::now();
              if (retcode < 0) {
                spdlog::info("Aborting: {} returned {}", target_name_string, retcode);
                abort_build = true;
                return;
              }
            } catch (const std::exception &e) {
              spdlog::error("Error running command for {}: {}", target_name_string, e.what());
              abort_build = true;
              return;
            }
          }
        } else {
          //spdlog::info("{} has no process", target_name_string);
        }
      }
#if USING_THE_OLD_TASK_COMPLETE_HANDLER
      if (task_complete_handler) {
        task_complete_handler(d->group);
      }
#else
      ++construct_task->group->current_count;
#endif

      return;
    });

    construct_task->task.precede(parent);
    // new_todo->second.task = task;

    // For each dependency described in blueprint, retrieve or create task, add relationship, and add item to todo list
    if (i)
      for (auto &dep_target: i->dependencies)
        create_tasks(dep_target.first(2) == "./" ? dep_target.sub(dep_target.first_not_of('/', 2)) : dep_target, construct_task->task, project);
    // else
    //     spdlog::info("{} does not have blueprint match", i->first);
  }
}

std::pair<std::string, int> task_engine::run_command(const std::string target, std::shared_ptr<blueprint_match> blueprint, const project &project, ryml::NodeRef project_data)
{
  std::string captured_output = "";
  inja::Environment inja_env  = inja::Environment();
  auto curdir_path     = blueprint->blueprint->parent_path;

  add_common_template_commands(inja_env);

  inja_env.add_callback("store", 3, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    if (args[0].valid() && args[1].valid()) {
      ryml::Pointer ptr{ args[0].val() };
      auto key = args[1].val<std::string>().value();
      if (key.front() == '/')
        ptr = ptr / key;
      else
        ptr = ptr / key;

      additional_data.rootref()["store"][ptr] << args[2].val();
    }
    return ryml::NodeRef{};
  });
  inja_env.add_callback("store", 2, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::Pointer ptr{ args[0].val() };
    additional_data.rootref()["store"][ptr] << args[1].val();
    return ryml::NodeRef{};
  });
  inja_env.add_callback("push_back", 2, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::Pointer ptr{ args[0].val() };
    if (!additional_data.rootref()["store"].contains(ptr)) {
      additional_data.rootref()["store"][ptr] << ryml::SEQ;
    }
    additional_data.rootref()["store"][ptr].append_child() << args[1].val();
    return ryml::NodeRef{};
  });
  inja_env.add_callback("push_back", 3, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    if (args[0].valid() && args[1].valid()) {
      ryml::Pointer ptr{ args[0].val() };
      auto key = args[1].val<std::string>().value();
      if (key.front() == '/')
        ptr = ptr / ryml::Pointer{ key };
      else
        ptr = ptr / key;

      if (!additional_data.rootref()["store"].contains(ptr)) {
        additional_data.rootref()["store"][ptr] << ryml::SEQ;
      }
      additional_data.rootref()["store"][ptr].append_child() << args[2].val();
    }
    return ryml::NodeRef{};
  });
  inja_env.add_callback("unique", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    auto filtered = additional_data.rootref().append_child() << ryml::SEQ;
    std::unordered_set<ryml::csubstr> seen;
    for (auto i: args[0].children())
      seen.insert(i.val());
    for (auto i: seen)
      filtered.append_child() << i;
    return filtered;
  });

  inja_env.add_callback("fetch", 2, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::Pointer ptr{ args[0].val() };
    auto key = args[1].val();
    return additional_data.rootref()["store"][ptr][key];
  });
  inja_env.add_callback("fetch", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::Pointer ptr{ args[0].val()};
    return additional_data.rootref()["store"][ptr];
  });
  inja_env.add_callback("erase", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::Pointer ptr{ args[0].val() };
    additional_data.rootref()["store"][ptr].clear();
    return ryml::NodeRef{};
  });
  inja_env.add_callback("$", 1, [&blueprint](inja::Arguments &args, ryml::Tree &additional_data) {
    return additional_data.rootref().append_child() << blueprint->regex_matches[args[0].val<int>().value()];
  });
  inja_env.add_callback("curdir", 0, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    return additional_data.rootref().append_child() << curdir_path;
  });
  inja_env.add_callback("render", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    return additional_data.rootref().append_child() << try_render(inja_env, args[0].val<std::string>().value(), project.project_summary);
  });
  inja_env.add_callback("render", 2, [&curdir_path, &inja_env, &project](inja::Arguments &args, ryml::Tree &additional_data) {
    auto backup               = curdir_path;
    curdir_path               = args[1].val();
    std::string render_output = try_render(inja_env, args[0].val<std::string>().value(), project.project_summary);
    curdir_path               = backup;
    return additional_data.rootref().append_child() << render_output;
  });

  inja_env.add_callback("aggregate", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
    ryml::NodeRef aggregate = additional_data.rootref().append_child() << ryml::MAP;
    auto path = ryml::Pointer{ args[0].val() };
    // Loop through components, check if object path exists, if so add it to the aggregate
    for (const auto node: project.project_summary["components"].children()) {
      if (!node[path].valid())
        continue;

      auto v = node[path];
      if (v.is_map())
        for (const auto i: v.children()) {
          aggregate[i.key()] = i.val(); //try_render(inja_env, i.second.as<std::string>(), project->project_summary, log);
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
          aggregate[i.key()] = i.val();
      else if (v.is_seq())
        for (const auto i: v.children())
          aggregate.append_child() << inja_env.render(i.val<std::string>().value(), project.project_summary);
      else
        aggregate.append_child() << inja_env.render(v.val<std::string>().value(), project.project_summary);
    }
    return aggregate;
  });
  inja_env.add_callback("load_component", 1, [&](inja::Arguments &args, ryml::Tree &additional_data) {
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
      int retcode = 0;

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

void task_engine::run_taskflow(yakka::project &project, task_engine_ui *ui)
{
  tf::Executor executor(std::min(32U, std::thread::hardware_concurrency()));
  todo_task_groups["Processing"] = std::make_shared<yakka::task_group>("Processing");
  auto finish                    = taskflow.emplace([&]() {
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

  t1 = std::chrono::high_resolution_clock::now();
  if (!is_valid()) {
    spdlog::error("Blueprints have circular dependency");
    return;
  }
  t2       = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to validate task graph", duration);

  ui->init(*this);

  auto execution_future = executor.run(taskflow);

  do {
    ui->update(*this);
  } while (execution_future.wait_for(500ms) != std::future_status::ready);

  ui->finish(*this);
}

} // namespace yakka
