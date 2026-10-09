/**
 * @file yakka_cli.cpp
 * @brief Implements the Yakka command-line entry point and command execution flow.
 */

#include "yakka.hpp"
#include "yakka_workspace.hpp"
#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "build_session.hpp"

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

struct progress_bar_task_ui : yakka::task_engine_ui {
  DynamicProgress<ProgressBar> task_progress_ui;
  std::vector<std::shared_ptr<ProgressBar>> task_progress_bars;

/// @brief Executes init.

  void init(yakka::task_engine &task_engine)
  {
    // Find the longest name of the task groups

    size_t largest_name_length = 0;
    for (const auto &i: task_engine.todo_task_groups) {
      if (i.second->name.size() > largest_name_length)
        largest_name_length = i.second->name.size() + 1; // +1 for the space
    }

    for (auto &i: task_engine.todo_task_groups) {
      std::string spaced_name = task_engine.project_name + " / " + yakka::ryml_string(i.second->name) + std::string(largest_name_length - i.second->name.size(), ' ');
      std::shared_ptr<ProgressBar> new_task_bar = std::make_shared<ProgressBar>(option::BarWidth{ 50 }, option::ShowPercentage{ true }, option::PrefixText{ spaced_name }, option::MaxProgress{ i.second->total_count });
      task_progress_bars.push_back(new_task_bar);
      i.second->ui_id = task_progress_ui.push_back(*new_task_bar);
      task_progress_ui[i.second->ui_id].set_option(option::PostfixText{ std::to_string(i.second->current_count) + "/" + std::to_string(i.second->total_count) });
    }
    task_progress_ui.print_progress();

    //    project.task_complete_handler = [&](std::shared_ptr<yakka::task_group> group) {
    //      ++group->current_count;
    //    };
  };

/// @brief Executes update.

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

/// @brief Executes finish.

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

/// @brief Executes main.

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
                       ("j,jobs", "Shared build worker count", cxxopts::value<int>()->default_value(std::to_string(yakka::build_options::default_jobs())))
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

  progress_bar_task_ui progress_bar_ui;
  int exit_code = 0;
  try {
    auto arguments = result.unmatched();
    arguments.push_back(action + "!");
    const auto requests = yakka::expand_build_requests(arguments, result["project-name"].as<std::string>());
    if (!requests)
      throw std::invalid_argument(requests.error());
    const auto jobs = result["jobs"].as<int>();
    if (jobs <= 0)
      throw std::invalid_argument("--jobs must be greater than zero");
    yakka::build_options build_options;
    build_options.jobs = static_cast<size_t>(jobs);
    build_options.no_eval = result["no-eval"].as<bool>();
    build_options.ignore_eval = result["ignore-eval"].as<bool>();
    build_options.ignore_yakka = result["no-yakka"].as<bool>();
    build_options.ignore_slc = result["no-slcc"].as<bool>();
    if (result.count("with"))
      build_options.slc_features = result["with"].as<std::vector<std::string>>();
    if (result.count("data"))
      build_options.data = result["data"].as<std::string>();
    if (result["fetch"].as<bool>())
      build_options.fetch_missing = [&workspace](yakka::project &project) { download_unknown_components(workspace, project); };
    yakka::build_session session(workspace, std::move(build_options));
    const auto results = session.run(*requests, &progress_bar_ui);
    for (const auto &build_result: results) {
      std::cout << build_result.project_name << ": " << (build_result.success ? "success" : "failed") << '\n';
      if (!build_result.success) {
        spdlog::error("[{}] {}", build_result.project_name, build_result.error);
        exit_code = 1;
      }
    }
  } catch (const std::exception &e) {
    spdlog::error("{}", e.what());
    exit_code = 1;
  }

  auto yakka_end_time = fs::file_time_type::clock::now();
  std::cout << "Complete in " << std::chrono::duration_cast<std::chrono::milliseconds>(yakka_end_time - yakka_start_time).count() << " milliseconds" << std::endl;

  spdlog::shutdown();
  show_console_cursor(true);
  return exit_code;
}
