#include "build_session.hpp"
#include "yakka_project.hpp"
#include "task_engine.hpp"
#include "file_check.hpp"
#include "glob/glob.h"
#include "ninja_generator.hpp"
#include "ninja_runner.hpp"
#include "legacy_fast_path.hpp"
#include "blueprint_commands.hpp"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace yakka {

struct project_build {
  std::unique_ptr<project> current;
  std::unique_ptr<task_engine> engine;
  size_t result_index;
  std::vector<size_t> dependencies;
  bool summary_reused = false;
};

namespace {

std::string request_signature(const build_request &request, const build_options &options, const std::string &data, const workspace &workspace)
{
  ryml::Tree descriptor;
  auto root = descriptor.rootref();
  root |= ryml::MAP;
  root["version"] << 3;
  root["file_checks"] << file_check_policy;
  root["engine"] << (options.engine == build_engine::ninja ? "ninja" : "taskflow");
  root["build"] << __DATE__ " " __TIME__;
  root["working_directory"] << fs::current_path().generic_string();
  root["workspace"] << ryml::emitrs_json<std::string>(workspace.summary);
  root["databases"] |= ryml::SEQ;
  const auto add_database = [&](const component_database &database) {
    const auto serialized = database.dump();
    root["databases"].append_child() << project_diff::to_hex(project_diff::hash_bytes(serialized.data(), serialized.size()));
  };
  add_database(workspace.local_database);
  add_database(workspace.shared_database);
  for (const auto &database: workspace.package_databases)
    add_database(database);
  root["arguments"] |= ryml::SEQ;
  for (const auto &argument: request.arguments)
    root["arguments"].append_child() << argument;
  root["slc_features"] |= ryml::SEQ;
  for (const auto &feature: options.slc_features)
    root["slc_features"].append_child() << feature;
  root["no_eval"] << options.no_eval;
  root["ignore_eval"] << options.ignore_eval;
  root["ignore_yakka"] << options.ignore_yakka;
  root["ignore_slc"] << options.ignore_slc;
  root["data"] << data;
  const auto serialized = ryml::emitrs_json<std::string>(descriptor);
  return project_diff::to_hex(project_diff::hash_bytes(serialized.data(), serialized.size()));
}

std::vector<std::string> glob_files(const std::vector<std::string> &patterns)
{
  std::vector<std::string> matches;
  for (const auto &path: glob::rglob(patterns))
    matches.push_back(path.generic_string());
  std::sort(matches.begin(), matches.end());
  return matches;
}

/// Inputs whose content is unchanged but whose record lacks a matching settled stat are added to `refresh`.
bool cached_summary_is_current(const project &current, const std::string &signature, std::vector<std::string> &refresh)
{
  if (!current.summary_loaded || !current.project_summary.has_child("build_request_signature") || current.project_summary["build_request_signature"].val() != ryml::to_csubstr(signature))
    return false;
  const auto cache = current.project_summary.find_child("input_cache");
  if (!cache.valid() || !cache.is_map() || !cache.has_child("valid") || !cache["valid"].val<bool>().value_or(false) || !cache.has_child("files") || !cache["files"].is_map() || cache["files"].num_children() < 2)
    return false;
  for (const auto input: cache["files"].children()) {
    const auto state = check_file(input, ryml_path(input.key()));
    if (state == file_check_state::changed) {
      spdlog::info("[{}] Cached project input changed: {}", current.project_name, input.key());
      return false;
    }
    if (state == file_check_state::refresh || !file_check_settled(input))
      refresh.push_back(ryml_string(input.key()));
  }
  if (cache.has_child("globs")) {
    if (!cache["globs"].is_seq())
      return false;
    for (const auto expansion: cache["globs"].children()) {
      if (!expansion.is_map() || !expansion.has_child("patterns") || !expansion["patterns"].is_seq() || !expansion.has_child("matches") || !expansion["matches"].is_seq())
        return false;
      std::vector<std::string> patterns;
      std::vector<std::string> previous_matches;
      for (const auto pattern: expansion["patterns"].children())
        patterns.push_back(ryml_string(pattern.val()));
      for (const auto match: expansion["matches"].children())
        previous_matches.push_back(ryml_string(match.val()));
      if (glob_files(patterns) != previous_matches) {
        spdlog::info("[{}] Cached template glob changed", current.project_name);
        return false;
      }
    }
  }
  return true;
}

void record_project_inputs(project &current)
{
  auto cache = current.project_summary["input_cache"];
  if (cache.has_child("files") && cache["files"].is_map())
    for (const auto input: cache["files"].children())
      seed_file_check(input, ryml_path(input.key()));
  cache.clear_children();
  cache |= ryml::MAP;
  // SLC processing also discovers override files and generates configuration files.
  cache["valid"] << (current.current_state == project::state::PROJECT_VALID && !current.project_has_slcc);
  auto files = cache["files"];
  files |= ryml::MAP;
  const auto record = [&](const fs::path &path) {
    const auto absolute = fs::absolute(path).lexically_normal().generic_string();
    auto input          = files.find_child(ryml::to_csubstr(absolute));
    if (!input.valid()) {
      input = files.append_child();
      input.set_key_serialized(absolute);
    }
    record_file_check(input, path);
  };
  record(current.project_file);
  record(current.workspace.workspace_path / "config.yaml");
  for (const auto &component: current.components)
    record(component->file_path);
  for (const auto &dependency_file: current.blueprint_database.dependency_files)
    record(dependency_file);
  for (const auto &path: current.template_inputs.files)
    record(path);
  for (const auto &path: current.blueprint_database.template_inputs.files)
    record(path);
  auto patterns = current.template_inputs.globs;
  patterns.insert(current.blueprint_database.template_inputs.globs.begin(), current.blueprint_database.template_inputs.globs.end());
  cache["globs"] |= ryml::SEQ;
  for (const auto &pattern: patterns) {
    auto expansion = cache["globs"].append_child();
    expansion |= ryml::MAP;
    expansion["patterns"] |= ryml::SEQ;
    for (const auto &argument: pattern)
      expansion["patterns"].append_child() << argument;
    expansion["matches"] |= ryml::SEQ;
    for (const auto &match: glob_files(pattern))
      expansion["matches"].append_child() << match;
  }
  // blueprints.json and target_database.json are Yakka's own outputs: a missing or unreadable one is regenerated, so they are not inputs
}

void check_choices(project &current)
{
  for (const auto &[component, choice]: current.incomplete_choices) {
    spdlog::error("[{}] Component '{}' has an incomplete choice '{}'", current.project_name, component, choice);
    current.current_state = project::state::PROJECT_HAS_INCOMPLETE_CHOICES;
  }
  for (const auto &choice: current.multiple_answer_choices) {
    spdlog::error("[{}] Choice '{}' has multiple selections", current.project_name, choice);
    current.current_state = project::state::PROJECT_HAS_MULTIPLE_ANSWERS_FOR_CHOICES;
  }
}

void log_phase_time(const std::string &project_name, const char *phase, std::chrono::steady_clock::time_point start)
{
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  spdlog::info("[{}] Timing {}: {:.3f} ms", project_name, phase, elapsed);
}

/// @param prepare_tasks Build the taskflow graph.
/// @param persist Save the summary and databases; false for concurrent read-only invocations such as exec-blueprint.
/// Requests for the sub-projects named by the project's `project` dependencies, in name order without duplicates.
std::vector<build_request> project_dependency_requests(const project &current)
{
  std::map<std::string, build_request> requests;
  for (const auto &[target, matches]: current.target_database.targets)
    for (const auto &match: matches)
      for (const auto &arguments: match->project_dependencies) {
        std::vector<std::string> build_arguments;
        for (const auto argument: arguments)
          build_arguments.push_back(ryml_string(argument));
        const auto expanded = expand_build_requests(build_arguments);
        if (!expanded)
          throw std::invalid_argument("Invalid project dependency: " + expanded.error());
        for (const auto &child: *expanded) {
          const auto [existing, inserted] = requests.emplace(child.project_name, child);
          if (!inserted && existing->second.arguments != child.arguments)
            throw std::invalid_argument("Conflicting project output: " + child.project_name);
        }
      }
  std::vector<build_request> result;
  for (auto &[name, request]: requests)
    result.push_back(std::move(request));
  return result;
}

void prepare_project(project_build &build, workspace &workspace, const build_request &request, const build_options &options, const std::string &data, bool prepare_tasks = true, bool persist = true)
{
  auto &current                = *build.current;
  const auto prepare_start     = std::chrono::steady_clock::now();
  current.output_path          = workspace.workspace_path / default_output_directory / request.project_name;
  current.project_summary_file = current.output_path / project_summary_filename;
  current.project_file         = workspace.workspace_path / (request.project_name + ".yakka");
  auto phase_start             = std::chrono::steady_clock::now();
  current.init_project(request.arguments, project::summary_mode::defer_evaluation);
  if (!data.empty()) {
    auto additional_data = current.project_data["request_data"];
    additional_data |= ryml::MAP;
    ryml::parse_in_arena(ryml::to_csubstr(data), additional_data);
  }
  log_phase_time(current.project_name, "project initialization", phase_start);

  const auto signature               = request_signature(request, options, data, workspace);
  const auto previous_signature_node = current.project_summary.find_child("build_request_signature");
  const auto previous_signature      = previous_signature_node.valid() && previous_signature_node.has_val() ? ryml_string(previous_signature_node.val()) : std::string{};
  phase_start                        = std::chrono::steady_clock::now();
  std::vector<std::string> refresh_inputs;
  const bool summary_reused          = cached_summary_is_current(current, signature, refresh_inputs);
  build.summary_reused               = summary_reused;
  if (summary_reused && !refresh_inputs.empty() && persist) {
    // Record the current stat so the next freshness scan does not hash these files again
    auto files = current.project_summary["input_cache"]["files"];
    for (const auto &path: refresh_inputs)
      record_file_check(files[ryml::to_csubstr(path)], path);
    current.save_summary();
  }
  log_phase_time(current.project_name, "project input freshness scan", phase_start);
  spdlog::info("[{}] Cached summary reused: {}", current.project_name, summary_reused);
  if (summary_reused)
    current.diff.use_cached_summary();
  else if (current.summary_loaded) {
    current.diff.capture_baseline(current.project_summary);
    current.detect_changed_components();
  }
  if (!summary_reused)
    current.load_project_file();

  if (options.ignore_yakka)
    current.component_flags = component_database::flag::IGNORE_YAKKA;
  if (options.ignore_slc)
    current.component_flags = component_database::flag::IGNORE_ALL_SLC;
  else {
    auto features = current.project_data["slc_request_features"];
    features |= ryml::SEQ;
    features.clear_children();
    for (const auto &feature: options.slc_features) {
      auto stored = features.append_child() << feature;
      current.slc_required.insert(stored.val());
    }
  }

  if (!summary_reused) {
    phase_start = std::chrono::steady_clock::now();
    if (!options.no_eval) {
      auto evaluation = current.evaluate_dependencies();
      if (evaluation == project::state::PROJECT_HAS_INVALID_COMPONENT)
        throw std::runtime_error("Invalid component");
      if (!current.unknown_components.empty()) {
        workspace.local_database.scan_for_components();
        workspace.shared_database.scan_for_components();
        current.unprocessed_components.swap(current.unknown_components);
        evaluation = current.evaluate_dependencies();
      }
      if (!current.unknown_components.empty() && options.fetch_missing) {
        options.fetch_missing(current);
        evaluation = current.evaluate_dependencies();
      }
      if (!current.unknown_components.empty()) {
        std::string missing;
        for (const auto &component: current.unknown_components)
          missing += " " + ryml_string(component);
        throw std::runtime_error("Missing components:" + missing + "; use --fetch to download them");
      }
      current.current_state = evaluation;
      current.evaluate_choices();
      check_choices(current);
    }
    log_phase_time(current.project_name, "dependency evaluation", phase_start);

    phase_start = std::chrono::steady_clock::now();
    if (!options.ignore_slc)
      current.process_slc_rules();
    log_phase_time(current.project_name, "SLC processing", phase_start);

    phase_start = std::chrono::steady_clock::now();
    current.generate_project_summary();
    current.project_summary["project_output"] << current.output_path.generic_string();
    current.update_project_data();
    current.validate_schema();
    current.finalize_diff();
    if (current.current_state != project::state::PROJECT_VALID && !options.ignore_eval)
      throw std::runtime_error(current.evaluation_error());
    log_phase_time(current.project_name, "summary, schema, and diff", phase_start);
  }

  const bool reuse_cached_state = summary_reused;
  spdlog::info("[{}] Cached state: reuse={}, diff_empty={}, signature_match={}", current.project_name, reuse_cached_state, current.diff.empty(), previous_signature == signature);
  phase_start                  = std::chrono::steady_clock::now();
  // Ninja reads compiler dependency files itself, so they must not become summary inputs or graph edges.
  current.blueprint_database.expand_dependency_files = options.engine == build_engine::taskflow;
  const bool blueprints_loaded = reuse_cached_state && fs::exists(current.output_path / "blueprints.json");
  if (blueprints_loaded)
    current.blueprint_database.load(current.output_path / "blueprints.json");
  else
    current.process_blueprints();

  for (const auto &command: current.commands) {
    if (current.blueprint_database.blueprints.contains(command) || !current.blueprint_database.find_match(command, current.project_summary).empty())
      continue;
    const auto providers = workspace.find_blueprint(command);
    if (!providers || providers->num_children() != 1)
      throw std::runtime_error("No unique blueprint provider for command: " + ryml_string(command));
    const auto location = workspace.find_component((*providers)[0].val());
    if (!location)
      throw std::runtime_error("Missing component for blueprint: " + ryml_string(command));
    current.add_additional_tool(location->first);
    if (!current.blueprint_database.blueprints.contains(command) && current.blueprint_database.find_match(command, current.project_summary).empty())
      throw std::runtime_error("Missing blueprint for command: " + ryml_string(command));
  }
  log_phase_time(current.project_name, "blueprint processing", phase_start);

  phase_start = std::chrono::steady_clock::now();
  if (!reuse_cached_state || current.target_database.load(current.output_path / "target_database.json") != yakka_status::SUCCESS) {
    const auto generate_start = std::chrono::steady_clock::now();
    current.generate_target_database();
    log_phase_time(current.project_name, "target database generation", generate_start);
    if (persist) {
      const auto save_start = std::chrono::steady_clock::now();
      current.target_database.save(current.output_path / "target_database.json");
      log_phase_time(current.project_name, "target database save", save_start);
    }
  }
  log_phase_time(current.project_name, "target database", phase_start);

  phase_start = std::chrono::steady_clock::now();
  if (!blueprints_loaded && persist)
    current.save_blueprints();
  if (!summary_reused && persist) {
    current.project_summary["build_request_signature"] << request_signature(request, options, data, workspace);
    record_project_inputs(current);
    current.save_summary();
  }
  log_phase_time(current.project_name, "persist project state", phase_start);
  if (!prepare_tasks) {
    log_phase_time(current.project_name, "project preparation total", prepare_start);
    return;
  }

  auto &engine             = *build.engine;
  engine.project_name      = current.project_name;
  bool task_state_loaded   = false;
  // Sub-projects build after preparation, so the restore compares their outputs by timestamp when the graph runs.
  engine.project_dependency_outputs.clear();
  for (const auto &child: project_dependency_requests(current))
    engine.project_dependency_outputs.push_back(fs::absolute(workspace.workspace_path / default_output_directory / child.project_name).lexically_normal().generic_string());
  if (reuse_cached_state) {
    phase_start       = std::chrono::steady_clock::now();
    task_state_loaded = engine.load_todolist(current.output_path / "todolist.json", current) == yakka_status::SUCCESS;
    log_phase_time(current.project_name, "todolist restore", phase_start);
  }
  if (!task_state_loaded) {
    phase_start = std::chrono::steady_clock::now();
    if (engine.create_dirty_taskflow(current, current.output_path / "todolist.json") != yakka_status::SUCCESS)
      throw std::runtime_error("Cannot create the task graph");
    log_phase_time(current.project_name, "task graph creation", phase_start);
  }
  if (!engine.is_valid())
    throw std::runtime_error("Blueprints have a circular dependency");
  log_phase_time(current.project_name, "project preparation total", prepare_start);
}

/// Arguments that make `yakka` rebuild exactly this project request (the command must come first for the CLI parser).
std::string request_arguments(const build_request &request, const build_options &options)
{
  std::vector<std::string> arguments = { "-p", request.project_name };
  if (options.no_eval)
    arguments.push_back("-n");
  if (options.ignore_eval)
    arguments.push_back("-i");
  if (options.ignore_yakka)
    arguments.push_back("--no-yakka");
  if (options.ignore_slc)
    arguments.push_back("--no-slcc");
  for (const auto &feature: options.slc_features)
    arguments.insert(arguments.end(), { "-w", feature });
  if (!options.data.empty())
    arguments.insert(arguments.end(), { "-d", options.data });
  if (!request.arguments.empty()) {
    // The CLI appends the action to the remaining positionals, so passing the last argument first reproduces the order.
    arguments.push_back(request.arguments.back());
    arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end() - 1);
  }
  std::string result;
  for (const auto &argument: arguments) {
    if (!result.empty())
      result += ' ';
    result += shell_quote(argument);
  }
  return result;
}

} // namespace

size_t build_options::default_jobs()
{
  return std::max(1U, std::min(32U, std::thread::hardware_concurrency()));
}

build_session::build_session(workspace &workspace, build_options options) : workspace_(workspace), options_(std::move(options))
{
  if (options_.jobs == 0)
    throw std::invalid_argument("The worker count must be greater than zero");
}

std::vector<build_result> build_session::run(const std::vector<build_request> &requests, task_engine_ui *ui)
{
  std::vector<build_request> unique_requests;
  std::map<std::string, std::vector<std::string>> output_claims;
  for (const auto &request: requests) {
    const auto validated = expand_build_requests(request.arguments, request.project_name);
    if (!validated || validated->size() != 1)
      throw std::invalid_argument(validated ? "Expand matrix arguments before running a session" : validated.error());
    const auto &resolved         = validated->front();
    const auto [claim, inserted] = output_claims.emplace(resolved.project_name, resolved.arguments);
    if (!inserted) {
      if (claim->second != resolved.arguments)
        throw std::invalid_argument("Conflicting project output: " + resolved.project_name);
      continue;
    }
    unique_requests.push_back(resolved);
  }

  std::string data;
  if (!options_.data.empty()) {
    const auto document = ryml_parse_yaml(c4::to_csubstr("{" + options_.data + "}"));
    if (!document.crootref().is_map())
      throw std::invalid_argument("Additional build data must be a mapping");
    data = ryml::emitrs_yaml<std::string>(document);
  }

  std::vector<build_result> results;
  for (const auto &request: unique_requests)
    results.push_back({ request.project_name, false, {} });
  std::vector<std::unique_ptr<project_build>> builds;
  // Ninja edges re-invoke the yakka executable, so library callers without one use the taskflow engine.
  if (options_.engine == build_engine::ninja && options_.yakka_binary.empty())
    options_.engine = build_engine::taskflow;
  if (options_.engine == build_engine::ninja) {
    if (auto ninja_results = run_ninja(unique_requests, data))
      return *ninja_results;
    spdlog::info("Using the taskflow engine for this request");
    options_.engine = build_engine::taskflow;
  }

  std::map<std::string, size_t> prepared;
  std::set<std::string> preparing;
  std::function<size_t(const build_request &)> prepare = [&](const build_request &request) {
    if (preparing.contains(request.project_name))
      throw std::runtime_error("Circular project dependency: " + request.project_name);
    if (const auto found = prepared.find(request.project_name); found != prepared.end())
      return found->second;
    if (prepared.size() >= 4096)
      throw std::runtime_error("Project dependencies exceed 4096 projects");
    const auto root = std::find_if(unique_requests.begin(), unique_requests.end(), [&](const auto &entry) {
      return entry.project_name == request.project_name;
    });
    const auto result_index = root == unique_requests.end() ? results.size() : size_t(root - unique_requests.begin());
    if (root == unique_requests.end())
      results.push_back({ request.project_name, false, {} });
    prepared.emplace(request.project_name, result_index);
    preparing.insert(request.project_name);
    try {
      auto build          = std::make_unique<project_build>();
      build->result_index = result_index;
      build->current      = std::make_unique<project>(workspace_, request.project_name);
      build->engine       = std::make_unique<task_engine>();
      prepare_project(*build, workspace_, request, options_, data);
      for (const auto &[target, matches]: build->current->target_database.targets)
        for (const auto &match: matches)
          for (const auto &arguments: match->project_dependencies) {
            std::vector<std::string> build_arguments;
            for (const auto argument: arguments)
              build_arguments.push_back(ryml_string(argument));
            const auto expanded = expand_build_requests(build_arguments);
            if (!expanded)
              throw std::invalid_argument("Invalid project dependency: " + expanded.error());
            for (const auto &child: *expanded) {
              const auto [claim, inserted] = output_claims.emplace(child.project_name, child.arguments);
              if (!inserted && claim->second != child.arguments)
                throw std::invalid_argument("Conflicting project output: " + child.project_name);
              const auto child_index = prepare(child);
              if (!results[child_index].error.empty())
                throw std::runtime_error("Project dependency '" + child.project_name + "' failed: " + results[child_index].error);
              build->dependencies.push_back(child_index);
            }
          }
      builds.push_back(std::move(build));
    } catch (const std::exception &error) {
      results[result_index].error = error.what();
      spdlog::error("[{}] {}", request.project_name, error.what());
    }
    preparing.erase(request.project_name);
    return result_index;
  };
  for (const auto &request: unique_requests)
    prepare(request);

  std::map<std::string, size_t> target_claims;
  for (const auto &build: builds)
    for (const auto &[target, matches]: build->current->target_database.targets) {
      if (build->current->commands.contains(target))
        continue;
      if (std::none_of(matches.begin(), matches.end(), [](const auto &match) {
            return match->blueprint->process.valid();
          }))
        continue;
      const auto path              = fs::absolute(ryml_string(target)).lexically_normal().generic_string();
      const auto [claim, inserted] = target_claims.emplace(path, build->result_index);
      if (!inserted && claim->second != build->result_index) {
        results[claim->second].error       = "Conflicting output target: " + path;
        results[build->result_index].error = "Conflicting output target: " + path;
      }
    }

  const auto active_build_count = std::count_if(builds.begin(), builds.end(), [&results](const auto &build) {
    return results[build->result_index].error.empty();
  });
  tf::Executor executor(options_.jobs);
  tf::Taskflow taskflow;
  std::map<size_t, tf::Task> completed;
  std::vector<project_build *> running;
  running.reserve(builds.size());
  for (auto &build: builds) {
    if (!results[build->result_index].error.empty())
      continue;
    build->engine->include_project_name_in_progress = active_build_count > 1;
    if (ui)
      ui->init(*build->engine);
    auto *current = build.get();
    auto ready = taskflow.emplace([&, current] {
      for (const auto dependency: current->dependencies)
        if (!results[dependency].success) {
          results[current->result_index].error = "Project dependency '" + results[dependency].project_name +
                                                "' failed: " + results[dependency].error;
          current->engine->abort_build = true;
          break;
        }
      if (!current->engine->abort_build)
        spdlog::info("[{}] Building", current->current->project_name);
    });
    auto execute = taskflow.composed_of(current->engine->taskflow);
    auto finish = taskflow.emplace([&, current] {
      auto &result = results[current->result_index];
      result.success = !current->engine->abort_build;
      if (!result.success && result.error.empty())
        result.error = "Task execution failed";
    });
    ready.precede(execute);
    execute.precede(finish);
    for (const auto dependency: current->dependencies) {
      if (const auto found = completed.find(dependency); found != completed.end())
        found->second.precede(ready);
      else {
        results[current->result_index].error = "Project dependency '" + results[dependency].project_name +
                                              "' failed: " + results[dependency].error;
        current->engine->abort_build = true;
      }
    }
    completed.emplace(current->result_index, finish);
    running.push_back(current);
  }

  auto future = executor.run(taskflow);
  try {
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
      if (ui)
        for (auto *active: running)
          ui->update(*active->engine);
    future.get();
  } catch (const std::exception &error) {
    if (future.valid())
      future.wait();
    for (auto *build: running) {
      results[build->result_index].success = false;
      results[build->result_index].error = error.what();
    }
  }
  for (auto *build: running) {
    auto &result = results[build->result_index];
    try {
      if (!result.success)
        build->engine->abort_build = true;
      const auto save_start = std::chrono::steady_clock::now();
      build->engine->save_todolist(build->current->output_path / "todolist.json");
      log_phase_time(build->current->project_name, "save todolist", save_start);
      if (ui)
        ui->finish(*build->engine);
    } catch (const std::exception &error) {
      result.success = false;
      result.error   = error.what();
    }
  }
  if (!options_.fast_path_record.empty())
    write_legacy_fast_record(builds, results, unique_requests);
  results.resize(unique_requests.size());
  return results;
}

void build_session::write_legacy_fast_record(const std::vector<std::unique_ptr<project_build>> &builds, const std::vector<build_result> &results, const std::vector<build_request> &requests) const
{
  std::error_code error;
  fs::remove(options_.fast_path_record, error);
  const bool no_op = std::all_of(results.begin(), results.end(), [](const auto &result) {
    return result.success;
  }) && std::all_of(builds.begin(), builds.end(), [](const auto &build) {
    return build->summary_reused && build->engine->commands_run == 0;
  });
  if (!no_op || builds.empty())
    return;

  legacy_fast_record record;
  for (const auto &request: requests)
    record.projects.push_back(request.project_name);
  std::set<std::string> seen;
  // A record is only safe when the stat could not hide a later write in the same timestamp tick.
  bool settled = true;
  const auto add_recorded = [&](const std::string &path, ryml::ConstNodeRef check) {
    const auto absolute = fs::absolute(path).lexically_normal().generic_string();
    if (!seen.insert(absolute).second)
      return;
    if (!file_check_settled(check)) {
      settled = false;
      return;
    }
    const bool exists = check["exists"].val<bool>().value_or(false);
    const auto stat   = recorded_file_stat(check);
    if (exists && !stat) {
      settled = false;
      return;
    }
    record.files.push_back({ absolute, exists ? stat : std::nullopt });
  };
  const auto add_current = [&](const fs::path &path, bool written_by_yakka) {
    const auto absolute = fs::absolute(path).lexically_normal().generic_string();
    if (!seen.insert(absolute).second)
      return;
    const auto stat = stat_file(absolute);
    // Yakka's own state files only change through Yakka, which replaces this record, so they need not settle first.
    if (stat && !written_by_yakka && !file_stat_settled(*stat))
      settled = false;
    record.files.push_back({ absolute, stat });
  };

  add_current(workspace_.local_database.get_path() / database_filename, false);
  add_current(workspace_.shared_database.get_path() / database_filename, false);
  for (const auto &database: workspace_.package_databases)
    add_current(database.get_path() / database_filename, false);
  for (const auto &build: builds) {
    const auto &current = *build->current;
    for (const auto &state: { project_summary_filename, std::string("blueprints.json"), std::string("target_database.json"), std::string("todolist.json") })
      add_current(current.output_path / state, true);
    const auto cache = current.project_summary.find_child("input_cache");
    if (!cache.valid() || !cache.has_child("files"))
      return;
    for (const auto input: cache["files"].children())
      add_recorded(ryml_string(input.key()), input);
    if (cache.has_child("globs"))
      for (const auto expansion: cache["globs"].children()) {
        legacy_fast_record::glob_expansion entry;
        for (const auto pattern: expansion["patterns"].children())
          entry.patterns.push_back(ryml_string(pattern.val()));
        for (const auto match: expansion["matches"].children())
          entry.matches.push_back(ryml_string(match.val()));
        record.globs.push_back(std::move(entry));
      }
    const auto &engine = *build->engine;
    const auto &tree   = engine.saved_todo_list_tree.empty() ? engine.todo_list_tree : engine.saved_todo_list_tree;
    if (tree.empty() || !tree.crootref().has_child("items"))
      return;
    for (const auto item: tree.crootref()["items"].children()) {
      if (!item.has_child("target") || !item.has_child("file_check"))
        continue;
      add_recorded(ryml_string(item["target"].val()), item["file_check"]);
    }
  }
  if (!settled) {
    spdlog::info("Legacy fast path not recorded: some inputs changed too recently");
    return;
  }
  if (!yakka::write_legacy_fast_record(options_.fast_path_record, record))
    spdlog::warn("Cannot write fast path record '{}'", options_.fast_path_record);
  else
    spdlog::info("Legacy fast path recorded with {} files and {} globs", record.files.size(), record.globs.size());
}

std::optional<std::vector<build_result>> build_session::run_ninja(const std::vector<build_request> &requests, const std::string &data)
{
  struct generated_manifest {
    size_t result_index;
    std::string manifest;
    std::vector<std::string> targets;
    std::vector<size_t> dependencies; // Indices into manifests, always earlier entries
    std::string projects;
  };
  struct prepared_project {
    size_t result_index;
    std::optional<size_t> manifest;
  };
  // Thrown past the per-project error handling when a request needs the taskflow engine.
  struct legacy_required {};

  std::vector<build_result> results;
  for (const auto &request: requests)
    results.push_back({ request.project_name, false, {} });
  std::vector<generated_manifest> manifests;
  const bool prepare_only = options_.action != ninja_action::build;
  bool all_generated      = true;
  std::map<std::string, std::vector<std::string>> output_claims;
  for (const auto &request: requests)
    output_claims.emplace(request.project_name, request.arguments);
  std::map<std::string, prepared_project> prepared;
  std::set<std::string> preparing;

  // Sub-projects are prepared before their parent's manifest is generated, so manifests are in dependency order.
  std::function<const prepared_project &(const build_request &)> prepare = [&](const build_request &request) -> const prepared_project & {
    if (preparing.contains(request.project_name))
      throw std::runtime_error("Circular project dependency: " + request.project_name);
    if (const auto found = prepared.find(request.project_name); found != prepared.end())
      return found->second;
    if (prepared.size() >= 4096)
      throw std::runtime_error("Project dependencies exceed 4096 projects");
    const auto root = std::find_if(requests.begin(), requests.end(), [&](const auto &entry) {
      return entry.project_name == request.project_name;
    });
    const auto result_index = root == requests.end() ? results.size() : size_t(root - requests.begin());
    if (root == requests.end())
      results.push_back({ request.project_name, false, {} });
    auto &entry = prepared.emplace(request.project_name, prepared_project{ result_index, std::nullopt }).first->second;
    preparing.insert(request.project_name);

    project_build build;
    build.result_index = result_index;
    build.current      = std::make_unique<project>(workspace_, request.project_name);
    build.engine       = std::make_unique<task_engine>();
    auto &current      = *build.current;
    try {
      prepare_project(build, workspace_, request, options_, data, false, options_.action != ninja_action::exec);
      if (current.project_has_slcc) {
        if (!prepare_only)
          throw legacy_required{};
        throw std::runtime_error("SLC projects are not supported by the Ninja engine yet");
      }

      if (options_.action == ninja_action::exec) {
        const auto target = current.target_database.targets.find(ryml::to_csubstr(options_.exec_target));
        if (target == current.target_database.targets.end())
          throw std::runtime_error("Unknown target: " + options_.exec_target);
        save_preserves_unchanged_files = true;
        for (const auto &match: target->second) {
          if (!match || !match->blueprint || !match->blueprint->process.valid())
            continue;
          const auto [output, retcode] = build.engine->run_command(options_.exec_target, match, current, current.project_summary["data"]);
          if (retcode != 0)
            throw std::runtime_error("Failed to build " + options_.exec_target);
        }
        results[result_index].success = true;
      } else {
        // Regeneration only rewrites this project's manifest; sub-project manifests regenerate themselves.
        std::vector<size_t> dependencies;
        std::string projects;
        for (const auto &child: project_dependency_requests(current)) {
          const auto [claim, inserted] = output_claims.emplace(child.project_name, child.arguments);
          if (!inserted && claim->second != child.arguments)
            throw std::invalid_argument("Conflicting project output: " + child.project_name);
          if (!projects.empty())
            projects += ' ';
          projects += child.project_name;
          if (prepare_only)
            continue;
          const auto &dependency = prepare(child);
          if (!results[dependency.result_index].error.empty() || !dependency.manifest)
            throw std::runtime_error("Project dependency '" + child.project_name + "' failed: " + results[dependency.result_index].error);
          dependencies.push_back(*dependency.manifest);
        }

        const auto phase_start = std::chrono::steady_clock::now();
        ninja_generator_options generator_options;
        generator_options.yakka_binary    = options_.yakka_binary;
        generator_options.yakka_arguments = request_arguments(request, options_);
        generator_options.projects        = projects;
        const auto manifest               = generate_ninja_manifest(current, workspace_, generator_options);
        log_phase_time(current.project_name, "ninja generation", phase_start);
        if (!manifest)
          throw std::runtime_error(manifest.error());

        std::vector<std::string> targets;
        for (const auto &command: current.commands)
          targets.push_back(ryml_string(command));
        std::sort(targets.begin(), targets.end());
        const auto relative = manifest->lexically_relative(fs::current_path());
        entry.manifest      = manifests.size();
        manifests.push_back({ result_index, relative.empty() ? manifest->generic_string() : relative.generic_string(), std::move(targets), std::move(dependencies), std::move(projects) });
        results[result_index].success = prepare_only;
      }
    } catch (const std::exception &error) {
      results[result_index].error = error.what();
      all_generated               = false;
      spdlog::error("[{}] {}", request.project_name, error.what());
    }
    preparing.erase(request.project_name);
    return entry;
  };

  try {
    for (const auto &request: requests)
      prepare(request);
  } catch (const legacy_required &) {
    return std::nullopt;
  }
  if (prepare_only) {
    results.resize(requests.size());
    return results;
  }

  if (!manifests.empty()) {
    std::vector<ninja::run_request> run_requests;
    for (const auto &entry: manifests)
      run_requests.push_back({ entry.manifest, entry.targets, manifests.size() > 1 ? results[entry.result_index].project_name : std::string(), entry.dependencies, entry.projects });
    ninja::run_options run_options;
    run_options.jobs                = options_.jobs;
    run_options.yakka_binary        = options_.yakka_binary;
    run_options.mark_manifest_fresh = true;
    const auto phase_start          = std::chrono::steady_clock::now();
    const auto statuses             = ninja::run(run_requests, run_options);
    for (size_t i = 0; i < manifests.size(); ++i) {
      auto &result   = results[manifests[i].result_index];
      result.success = statuses[i] == ninja::run_status::success;
      if (statuses[i] == ninja::run_status::manifest_failed)
        result.error = "Failed to load or regenerate " + manifests[i].manifest;
      else if (statuses[i] == ninja::run_status::stale)
        result.error = "Regenerated " + manifests[i].manifest + " requires different project dependencies";
      else if (!result.success) {
        result.error = "Build failed";
        for (const auto dependency: manifests[i].dependencies) {
          const auto &child = results[manifests[dependency].result_index];
          if (!child.success) {
            result.error = "Project dependency '" + child.project_name + "' failed: " + child.error;
            break;
          }
        }
      }
      log_phase_time(result.project_name, "ninja build", phase_start);
    }
  }

  if (!options_.fast_path_record.empty() && all_generated && !manifests.empty()) {
    std::error_code ec;
    fs::create_directories(fs::path(options_.fast_path_record).parent_path(), ec);
    std::ofstream record(options_.fast_path_record, std::ios::trunc);
    // Fields: project name, manifest, 1 if requested directly, sub-project names, comma-separated dependency line indices, targets...
    for (const auto &entry: manifests) {
      record << results[entry.result_index].project_name << '\t' << entry.manifest << '\t' << (entry.result_index < requests.size() ? '1' : '0') << '\t' << entry.projects << '\t';
      for (size_t i = 0; i < entry.dependencies.size(); ++i)
        record << (i ? "," : "") << entry.dependencies[i];
      for (const auto &target: entry.targets)
        record << '\t' << target;
      record << '\n';
    }
  }
  results.resize(requests.size());
  return results;
}

} // namespace yakka
