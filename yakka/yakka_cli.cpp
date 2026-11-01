/**
 * @file yakka_cli.cpp
 * @brief Implements the Yakka command-line entry point and command execution flow.
 */

#include "yakka.hpp"
#include "yakka_workspace.hpp"
#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "build_session.hpp"
#include "ninja_runner.hpp"
#include "legacy_fast_path.hpp"
#include "project_diff.hpp"

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
#include <charconv>
#include <cstring>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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
      std::string spaced_name = (task_engine.include_project_name_in_progress ? task_engine.project_name + " / " : "") +
                                yakka::ryml_string(i.second->name) + std::string(largest_name_length - i.second->name.size(), ' ');
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

static std::string current_executable_path(const char *argv0)
{
  std::error_code ec;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    buffer.resize(std::strlen(buffer.c_str()));
    const auto path = fs::canonical(buffer, ec);
    if (!ec)
      return path.generic_string();
  }
#elif defined(__linux__)
  const auto path = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return path.generic_string();
#endif
  const auto path = fs::absolute(argv0, ec);
  return ec ? std::string(argv0) : path.generic_string();
}

/// Identify an invocation (ignoring the job count) so a previous Ninja generation or taskflow no-op can be reused without loading the workspace.
static std::optional<std::pair<fs::path, size_t>> fast_path_key(int argc, char **argv)
{
  std::error_code ec;
  const auto working_directory = fs::current_path(ec);
  if (ec)
    return std::nullopt;
  std::string key = __DATE__ " " __TIME__ "\n" + working_directory.generic_string() + "\n";
  size_t jobs     = yakka::build_options::default_jobs();
  bool has_command = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-j" || argument == "--jobs") {
      if (i + 1 >= argc)
        return std::nullopt;
      jobs = std::strtoul(argv[++i], nullptr, 10);
      continue;
    }
    if (argument.starts_with("--jobs=") || (argument.starts_with("-j") && argument.size() > 2 && !argument.starts_with("--"))) {
      jobs = std::strtoul(argument.data() + (argument[1] == '-' ? 7 : 2), nullptr, 10);
      continue;
    }
    if (argument == "-h" || argument == "--help" || argument == "-r" || argument == "--refresh" || argument == "-f" || argument == "--fetch" || argument == "-o" ||
        argument == "--no-output" || argument.starts_with("--ninja-"))
      return std::nullopt;
    if (argument.ends_with("!"))
      has_command = true;
    key += argument;
    key += '\0';
  }
  if (!has_command || jobs == 0)
    return std::nullopt;
  const auto hash = yakka::project_diff::to_hex(yakka::project_diff::hash_bytes(key.data(), key.size()));
  return std::pair{ fs::path(yakka::default_output_directory) / ".yakka" / "fast" / hash, jobs };
}

/// Re-run the Ninja manifests, or re-check the taskflow inputs, recorded by a previous identical invocation. Returns nullopt when the slow path is required.
static std::optional<int> run_fast_path(const fs::path &record_path, size_t jobs, const std::string &yakka_binary)
{
  if (yakka::is_legacy_fast_record(record_path)) {
    const auto legacy = yakka::read_legacy_fast_record(record_path);
    if (!legacy || !yakka::legacy_fast_record_current(*legacy))
      return std::nullopt;
    for (const auto &project_name: legacy->projects)
      std::cout << project_name << ": success\n";
    return 0;
  }
  std::ifstream record(record_path);
  if (!record)
    return std::nullopt;
  struct entry {
    std::string project_name;
    std::string manifest;
    bool requested;
    std::string projects;
    std::vector<size_t> dependencies;
    std::vector<std::string> targets;
  };
  std::vector<entry> entries;
  std::string line;
  while (std::getline(record, line)) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', start)) {
      fields.push_back(line.substr(start, tab - start));
      start = tab + 1;
    }
    fields.push_back(line.substr(start));
    if (fields.size() < 5 || (fields[2] != "0" && fields[2] != "1"))
      return std::nullopt;
    const auto &dependency_field = fields[4];
    std::vector<size_t> dependencies;
    for (size_t position = 0; position < dependency_field.size();) {
      size_t index      = 0;
      const auto parsed = std::from_chars(dependency_field.data() + position, dependency_field.data() + dependency_field.size(), index);
      if (parsed.ec != std::errc{} || index >= entries.size())
        return std::nullopt;
      dependencies.push_back(index);
      position = size_t(parsed.ptr - dependency_field.data());
      if (position < dependency_field.size() && dependency_field[position++] != ',')
        return std::nullopt;
    }
    entries.push_back({ fields[0], fields[1], fields[2] == "1", fields[3], std::move(dependencies), { fields.begin() + 5, fields.end() } });
  }
  if (entries.empty())
    return std::nullopt;
  for (const auto &item: entries)
    if (!fs::exists(item.manifest))
      return std::nullopt;

  std::vector<yakka::ninja::run_request> requests;
  for (const auto &item: entries)
    requests.push_back({ item.manifest, item.targets, entries.size() > 1 ? item.project_name : std::string(), item.dependencies, item.projects });
  yakka::ninja::run_options options;
  options.jobs                   = jobs;
  options.yakka_binary           = yakka_binary;
  options.all_manifests_required = true;
  const auto statuses            = yakka::ninja::run(requests, options);
  for (const auto status: statuses)
    if (status == yakka::ninja::run_status::manifest_failed || status == yakka::ninja::run_status::skipped || status == yakka::ninja::run_status::stale)
      return std::nullopt;

  int exit_code = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].requested)
      continue;
    const bool success = statuses[i] == yakka::ninja::run_status::success;
    std::cout << entries[i].project_name << ": " << (success ? "success" : "failed") << '\n';
    if (!success)
      exit_code = 1;
  }
  return exit_code;
}

/// @brief Executes main.

int main(int argc, char **argv)
{
  auto yakka_start_time = fs::file_time_type::clock::now();
  const auto yakka_binary = current_executable_path(argv[0]);

  const auto fast_key = fast_path_key(argc, argv);
  if (fast_key)
    if (const auto exit_code = run_fast_path(fast_key->first, fast_key->second, yakka_binary)) {
      std::cout << "Complete in " << std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() - yakka_start_time).count() << " milliseconds" << std::endl;
      return *exit_code;
    }

  // Child invocations from Ninja edges share the parent's log instead of replacing it.
  bool ninja_child = false;
  for (int i = 1; i < argc; ++i)
    if (std::string_view(argv[i]).starts_with("--ninja-"))
      ninja_child = true;

  // Setup logging
  std::error_code error_code;
  if (!ninja_child)
    fs::remove("yakka.log", error_code);

  auto console = spdlog::stdout_color_mt("console");

  auto console_error = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  console_error->set_level(spdlog::level::warn);
  console_error->set_pattern("[%^%l%$]: %v");
  std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_log;
  try {
    file_log = std::make_shared<spdlog::sinks::basic_file_sink_mt>("yakka.log", !ninja_child);
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
                       ("engine", "Build engine: 'taskflow' (default) or 'ninja'", cxxopts::value<std::string>()->default_value("taskflow"))
                       ("ninja-regenerate", "Internal: regenerate build.ninja only", cxxopts::value<bool>()->default_value("false"))
                       ("ninja-exec", "Internal: run the blueprint process for one target", cxxopts::value<std::string>())
                       ("action", "Select from 'register', 'list', 'update', 'git', 'remove', 'fetch', 'serve', 'benchmark' or a command", cxxopts::value<std::string>());
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
    const auto engine  = result["engine"].as<std::string>();
    // 'legacy' is the former name of the taskflow engine
    if (engine == "ninja")
      build_options.engine = yakka::build_engine::ninja;
    else if (engine != "taskflow" && engine != "legacy")
      throw std::invalid_argument("--engine must be 'taskflow' or 'ninja'");
    build_options.yakka_binary = yakka_binary;
    // Manifest callbacks only come from the Ninja engine and must share its request signature
    if (result["ninja-regenerate"].as<bool>()) {
      build_options.engine = yakka::build_engine::ninja;
      build_options.action = yakka::ninja_action::regenerate;
    }
    if (result.count("ninja-exec")) {
      build_options.engine      = yakka::build_engine::ninja;
      build_options.action      = yakka::ninja_action::exec;
      build_options.exec_target = result["ninja-exec"].as<std::string>();
    }
    if (fast_key)
      build_options.fast_path_record = fast_key->first.generic_string();
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
      if (!ninja_child)
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
  if (!ninja_child)
    std::cout << "Complete in " << std::chrono::duration_cast<std::chrono::milliseconds>(yakka_end_time - yakka_start_time).count() << " milliseconds" << std::endl;

  spdlog::shutdown();
  show_console_cursor(true);
  return exit_code;
}
