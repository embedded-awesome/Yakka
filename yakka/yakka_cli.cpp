#include "yakka.hpp"
#include "yakka_workspace.hpp"
#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "target_database.hpp"
#include "utilities.hpp"
#include "yakka_config.hpp"
#include "yakka_cli_actions.hpp"
#include "cxxopts.hpp"
#include "subprocess.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "semver/semver.hpp"
#include "taskflow.hpp"
#include <indicators/dynamic_progress.hpp>
#include <indicators/progress_bar.hpp>
#include <indicators/cursor_control.hpp>
#include <indicators/termcolor.hpp>
#include <iostream>
#include <fstream>
#include <chrono>
#include <future>
#include <algorithm>

using namespace indicators;
using namespace std::chrono_literals;

static void evaluate_project_dependencies(yakka::workspace &workspace, yakka::project &project);
static void print_project_choice_errors(yakka::project &project);

struct progress_bar_task_ui : yakka::task_engine_ui {
  DynamicProgress<ProgressBar> task_progress_ui;
  std::vector<std::shared_ptr<ProgressBar>> task_progress_bars;

  void init(yakka::task_engine &task_engine)
  {
    // Find the longest name of the task groups
    size_t largest_name_length = 0;
    for (const auto &i: task_engine.todo_task_groups) {
      if (i.second->name.size() > largest_name_length)
        largest_name_length = i.second->name.size() + 1; // +1 for the space
    }

    for (auto &i: task_engine.todo_task_groups) {
      std::string spaces(largest_name_length - i.second->name.size(), ' ');
      std::shared_ptr<ProgressBar> new_task_bar = std::make_shared<ProgressBar>(option::BarWidth{ 50 }, option::ShowPercentage{ true }, option::PrefixText{ i.second->name + spaces }, option::MaxProgress{ i.second->total_count });
      task_progress_bars.push_back(new_task_bar);
      i.second->ui_id = task_progress_ui.push_back(*new_task_bar);
      task_progress_ui[i.second->ui_id].set_option(option::PostfixText{ std::to_string(i.second->current_count) + "/" + std::to_string(i.second->total_count) });
    }
    task_progress_ui.print_progress();

    //    project.task_complete_handler = [&](std::shared_ptr<yakka::task_group> group) {
    //      ++group->current_count;
    //    };
  };

  void update(yakka::task_engine &task_engine)
  {
    for (const auto &i: task_engine.todo_task_groups) {
      if (i.second->current_count != i.second->last_progress_update) {
        task_progress_ui[i.second->ui_id].set_option(option::PostfixText{ std::to_string(i.second->current_count) + "/" + std::to_string(i.second->total_count) });
        //size_t new_progress = (100 * i.second->current_count) / i.second->total_count;
        task_progress_ui[i.second->ui_id].set_progress(i.second->current_count);
        i.second->last_progress_update = i.second->current_count;
        if (i.second->current_count == i.second->total_count) {
          task_progress_ui[i.second->ui_id].mark_as_completed();
        }
      }
    }
  };

  void finish(yakka::task_engine &task_engine)
  {
    for (const auto &i: task_engine.todo_task_groups) {
      task_progress_ui[i.second->ui_id].set_option(option::PostfixText{ std::to_string(i.second->current_count) + "/" + std::to_string(i.second->total_count) });
      task_progress_ui[i.second->ui_id].set_progress(i.second->current_count);
    }
    task_progress_ui.print_progress();
  };
};

static const semver::version yakka_version{
#include "yakka_version.h"
};

int main(int argc, char **argv)
{
  auto yakka_start_time = fs::file_time_type::clock::now();

  // Setup logging
  std::error_code error_code;
  fs::remove("yakka.log", error_code);

  auto console = spdlog::stdout_color_mt("console");

  auto console_error = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  console_error->set_level(spdlog::level::warn);
  console_error->set_pattern("[%^%l%$]: %v");
  std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_log;
  try {
    file_log = std::make_shared<spdlog::sinks::basic_file_sink_mt>("yakka.log", true);
  } catch (...) {
    try {
      auto time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      file_log  = std::make_shared<spdlog::sinks::basic_file_sink_mt>("yakka-" + std::to_string(time) + ".log", true);
    } catch (...) {
      std::cerr << "Cannot open yakka.log";
      exit(1);
    }
  }
  file_log->set_level(spdlog::level::trace);

  auto yakkalog = std::make_shared<spdlog::logger>("yakkalog", spdlog::sinks_init_list{ console_error, file_log });
  spdlog::set_default_logger(yakkalog);

  // Create a workspace
  yakka::workspace workspace;
  workspace.init(fs::current_path());

  cxxopts::Options options("yakka", "Yakka the embedded builder. Ver " + yakka_version.str());
  options.allow_unrecognised_options();
  options.positional_help("<action> [optional args]");
  // clang-format off
  options.add_options()("h,help", "Print usage")("r,refresh", "Refresh component database", cxxopts::value<bool>()->default_value("false"))
                       ("n,no-eval", "Skip the dependency and choice evaluation", cxxopts::value<bool>()->default_value("false"))
                       ("i,ignore-eval", "Ignore dependency and choice evaluation errors", cxxopts::value<bool>()->default_value("false"))
                       ("o,no-output", "Do not generate output folder", cxxopts::value<bool>()->default_value("false"))
                       ("f,fetch", "Automatically fetch missing components", cxxopts::value<bool>()->default_value("false"))
                       ("p,project-name", "Set the project name", cxxopts::value<std::string>()->default_value(""))
                       ("w,with", "Additional SLC feature", cxxopts::value<std::vector<std::string>>())
                       ("d,data", "Additional data", cxxopts::value<std::string>())
                       ("no-slcc", "Ignore SLC files", cxxopts::value<bool>()->default_value("false"))
                       ("no-yakka", "Ignore Yakka files", cxxopts::value<bool>()->default_value("false"))
                       ("action", "Select from 'register', 'list', 'update', 'git', 'remove', 'fetch', 'serve' or a command", cxxopts::value<std::string>());
  // clang-format on

  options.parse_positional({ "action" });
  auto result = options.parse(argc, argv);
  if (result.count("help") || argc == 1) {
    std::cout << options.help() << std::endl;
    return 0;
  }
  if (result["refresh"].as<bool>()) {
    workspace.local_database.erase();
    workspace.local_database.clear();

    std::cout << "Scanning '.' for components\n";
    workspace.local_database.scan_for_components();
    auto result = workspace.local_database.save();
    if (!result.has_value()) {
      std::cerr << "Failed to save local database: " << result.error().message() << "\n";
      return -1;
    }

    for (auto &db: workspace.package_databases) {
      std::cout << "Scanning '" << db.get_path().string() << "' for components\n";
      db.erase();
      db.clear();
      db.scan_for_components();
      auto result = db.save();
      if (!result.has_value()) {
        std::cerr << "Failed to save package database: " << result.error().message() << "\n";
        return -1;
      }
    }
    std::cout << "Scan complete.\n";
  }

  // Check if there is no action. If so, print the help
  if (!result.count("action")) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  auto action = result["action"].as<std::string>();
  if (action.back() != '!') {
    // Check if the action exists in our map
    auto action_it = yakka::cli_actions.find(action);
    if (action_it != yakka::cli_actions.end()) {
      // Call the corresponding handler function
      return action_it->second(workspace, result);
    }

    std::cout << "Must provide an action or a command (commands end with !)\n";
    return 0;
  } else {
    // Action must be a command. Drop the !
    action.pop_back();
  }

  // Process the command line options
  std::string project_name;
  std::string feature_suffix;
  std::vector<std::string> components;
  std::vector<std::string> features;
  std::unordered_set<std::string> commands;
  for (auto s: result.unmatched()) {
    // Identify features, commands, and components
    if (s.front() == '+') {
      feature_suffix += s;
      features.push_back(s.substr(1));
    } else if (s.back() == '!')
      commands.insert(s.substr(0, s.size() - 1));
    else {
      components.push_back(s);

      // Compose the project name by concatenation all the components in CLI order.
      // The features will be added at the end
      project_name += s + "-";
    }
  }

  if (components.size() == 0) {
    spdlog::error("No components identified");
    return -1;
  }

  // Remove the extra "-" and add the feature suffix
  project_name.pop_back();
  project_name += feature_suffix;

  auto cli_set_project_name = result["project-name"].as<std::string>();
  if (!cli_set_project_name.empty())
    project_name = cli_set_project_name;

  // Create a project and output
  yakka::project project(project_name, workspace);

  // Add the action as a command
  commands.insert(action);

  // Init the project
  project.init_project(components, features, commands);

  // Check if we don't want Yakka files
  if (result["no-yakka"].count() != 0) {
    project.component_flags = yakka::component_database::flag::IGNORE_YAKKA;
  }

  // Check if SLC needs to be supported
  if (result["no-slcc"].count() != 0) {
    project.component_flags = yakka::component_database::flag::IGNORE_ALL_SLC;
  } else {
    // Add SLC features
    if (result["with"].count() != 0) {
      const auto slc_features = result["with"].as<std::vector<std::string>>();
      for (const auto &f: slc_features)
        project.slc_required.insert(f);
    }
  }

  if (!result["no-eval"].as<bool>()) {
    evaluate_project_dependencies(workspace, project);

    if (!project.unknown_components.empty()) {
      if (result["fetch"].as<bool>()) {
        download_unknown_components(workspace, project);
      } else {
        for (const auto &i: project.unknown_components)
          spdlog::error("Missing component '{}'", i);
        spdlog::error("Try adding the '-f' command line option to automatically fetch components");
        spdlog::shutdown();
        exit(0);
      }
    }

    project.evaluate_choices();
    if (!result["ignore-eval"].as<bool>() && (!project.incomplete_choices.empty() || !project.multiple_answer_choices.empty()))
      print_project_choice_errors(project);
  } else {
    spdlog::info("Skipping project evalutaion");

    for (const auto &i: components) {
      // Convert string to id
      const auto component_id = yakka::component_dotname_to_id(i);
      // Find the component in the project component database
      auto component_location = workspace.find_component(component_id, project.component_flags);
      if (!component_location) {
        continue;
      }

      // Add component to the required list and continue if this is not a new component
      // Insert component and continue if this is not new
      if (project.required_components.insert(component_id).second == false)
        continue;

      auto [component_path, package_path]             = component_location.value();
      std::shared_ptr<yakka::component> new_component = std::make_shared<yakka::component>();
      if (new_component->parse_file(component_path, package_path) == yakka::yakka_status::SUCCESS) {
        project.components.push_back(new_component);
      } else {
        if (!result["ignore-eval"].as<bool>()) {
          spdlog::error("Failed to parse {}", component_path.generic_string());
          exit(-1);
        }
      }
    }
  }

  if (result["no-slcc"].count() == 0)
    project.process_slc_rules();

  // Project evaluation is complete
  project.generate_project_summary();

  // Merge project data
  project.update_project_data();

  // Evaluate the project schema including defaults
  auto t1 = std::chrono::high_resolution_clock::now();
  project.validate_schema();
  auto t2       = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to validate schemas", duration);

  // Print a list of required features
  spdlog::info("Required features:");
  for (auto f: project.required_features)
    spdlog::info("- {}", f);

  // Generate and save the summary
  project.save_summary();

  if (project.current_state != yakka::project::state::PROJECT_VALID && !result["ignore-eval"].as<bool>())
    exit(-1);

  // Insert additional command line data before processing blueprints
  if (result["data"].count() != 0) {
    spdlog::info("Processing additional data: {}", result["data"].as<std::string>());
    const auto additional_data = "{" + result["data"].as<std::string>() + "}";
    YAML::Node yaml_data       = YAML::Load(additional_data);
    nlohmann::json json_data   = yaml_data.as<nlohmann::json>();
    spdlog::info("Additional data: {}", json_data.dump());
    yakka::json_node_merge("/data"_json_pointer, project.project_summary["data"], json_data);
  }

  t1 = std::chrono::high_resolution_clock::now();
  project.process_blueprints();

  project.save_blueprints();

  // Ensure all the commands have a blueprint
  spdlog::info("Checking for missing blueprints");
  for (const auto &c: project.commands) {
    if (project.blueprint_database.blueprints.contains(c))
      continue;

    // Find a component that has that blueprint
    auto result = workspace.find_blueprint(c);
    if (result) {
      const auto blueprint_options = result.value();
      if (blueprint_options.size() == 1) {
        const auto &component_name = blueprint_options[0].get<std::string>();
        auto component_paths       = workspace.find_component(component_name);
        if (component_paths) {
          auto [component_path, db_path] = component_paths.value();
          spdlog::info("Found a blueprint for {}: {}", c, component_path.string());
          project.add_additional_tool(component_path);
        } else {
          spdlog::error("Could not find component for blueprint: {}", c);
        }
      } else {
        spdlog::error("Multiple options for missing blueprint {}", c);
        for (const auto &o: blueprint_options)
          spdlog::error("- {}", o.get<std::string>());
        return -1;
      }
    } else {
      spdlog::info("Did not find a blueprint for {}", c);
    }
  }

  try {
    spdlog::debug("Generating target database");
    project.generate_target_database();
  } catch (const std::exception &e) {
    spdlog::error("Failed to generate target database: {}", e.what());
    return -1;
  }
  t2       = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to process blueprints", duration);

  yakka::task_engine task_engine;
  progress_bar_task_ui progress_bar_ui;
  try {
    task_engine.run_taskflow(project, &progress_bar_ui);
  } catch (const std::exception &e) {
    spdlog::error("Running task engine failed: {}", e.what());
    return -1;
  }

  auto yakka_end_time = fs::file_time_type::clock::now();
  std::cout << "Complete in " << std::chrono::duration_cast<std::chrono::milliseconds>(yakka_end_time - yakka_start_time).count() << " milliseconds" << std::endl;

  spdlog::shutdown();
  show_console_cursor(true);

  if (task_engine.abort_build)
    return -1;
  else
    return 0;
}

static void evaluate_project_dependencies(yakka::workspace &workspace, yakka::project &project)
{
  auto t1 = std::chrono::high_resolution_clock::now();

  if (project.evaluate_dependencies() == yakka::project::state::PROJECT_HAS_INVALID_COMPONENT)
    exit(1);

  // If we're missing a component, update the component database and try again
  if (!project.unknown_components.empty()) {
    spdlog::info("Scanning workspace to find missing components");
    workspace.local_database.scan_for_components();
    workspace.shared_database.scan_for_components();
    project.unprocessed_components.swap(project.unknown_components);
    project.evaluate_dependencies();
  }

  auto t2       = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  spdlog::info("{}ms to process components", duration);
}

static void print_project_choice_errors(yakka::project &project)
{
  for (auto &i: project.incomplete_choices) {
    bool valid_options = false;
    spdlog::error("Component '{}' has a choice '{}' - Must choose from the following", i.first, i.second);
    if (project.project_summary["choices"][i.second].contains("features")) {
      valid_options = true;
      spdlog::error("Features: ");
      for (auto &b: project.project_summary["choices"][i.second]["features"])
        spdlog::error("  - {}", b.get<std::string>());
    }

    if (project.project_summary["choices"][i.second].contains("components")) {
      valid_options = true;
      spdlog::error("Components: ");
      for (auto &b: project.project_summary["choices"][i.second]["components"])
        spdlog::error("  - {}", b.get<std::string>());
    }

    if (!valid_options) {
      spdlog::error("ERROR: Choice data is invalid");
    }
    project.current_state = yakka::project::state::PROJECT_HAS_INCOMPLETE_CHOICES;
  }

  for (auto a: project.multiple_answer_choices) {
    spdlog::error("Choice {} - Has multiple selections", a);
    project.current_state = yakka::project::state::PROJECT_HAS_MULTIPLE_ANSWERS_FOR_CHOICES;
  }
}
