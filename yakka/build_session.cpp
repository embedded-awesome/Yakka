#include "build_session.hpp"
#include "yakka_project.hpp"
#include "task_engine.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

namespace yakka {
namespace {

struct project_build {
  std::unique_ptr<project> current;
  std::unique_ptr<task_engine> engine;
  size_t result_index;
};

std::string request_signature(const build_request &request, const build_options &options, const std::string &data)
{
  ryml::Tree descriptor;
  auto root = descriptor.rootref();
  root |= ryml::MAP;
  root["version"] << 1;
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

void prepare_project(project_build &build, workspace &workspace, const build_request &request,
                     const build_options &options, const std::string &data)
{
  auto &current = *build.current;
  const auto prepare_start = std::chrono::steady_clock::now();
  current.output_path = workspace.workspace_path / default_output_directory / request.project_name;
  current.project_summary_file = current.output_path / project_summary_filename;
  current.project_file = workspace.workspace_path / (request.project_name + ".yakka");
  auto phase_start = std::chrono::steady_clock::now();
  current.init_project(request.arguments);
  if (!data.empty()) {
    auto additional_data = current.project_data["request_data"];
    additional_data |= ryml::MAP;
    ryml::parse_in_arena(ryml::to_csubstr(data), additional_data);
  }
  log_phase_time(current.project_name, "project initialization", phase_start);

  const auto signature = request_signature(request, options, data);
  const auto previous_signature_node = current.project_summary.find_child("build_request_signature");
  const auto previous_signature = previous_signature_node.valid() && previous_signature_node.has_val()
                                    ? ryml_string(previous_signature_node.val()) : std::string{};

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
    throw std::runtime_error("Project evaluation failed with state " + std::to_string(static_cast<int>(current.current_state)));
  log_phase_time(current.project_name, "summary, schema, and diff", phase_start);

  const bool reuse_cached_state = current.diff.empty() && previous_signature == signature;
  spdlog::info("[{}] Cached state: reuse={}, diff_empty={}, signature_match={}", current.project_name,
               reuse_cached_state, current.diff.empty(), previous_signature == signature);
  phase_start = std::chrono::steady_clock::now();
  const bool blueprints_loaded = reuse_cached_state && fs::exists(current.output_path / "blueprints.json");
  if (blueprints_loaded)
    current.blueprint_database.load(current.output_path / "blueprints.json");
  else
    current.process_blueprints();

  for (const auto &command: current.commands) {
    if (current.blueprint_database.blueprints.contains(command) ||
        !current.blueprint_database.find_match(command, current.project_summary).empty())
      continue;
    const auto providers = workspace.find_blueprint(command);
    if (!providers || providers->num_children() != 1)
      throw std::runtime_error("No unique blueprint provider for command: " + ryml_string(command));
    const auto location = workspace.find_component((*providers)[0].val());
    if (!location)
      throw std::runtime_error("Missing component for blueprint: " + ryml_string(command));
    current.add_additional_tool(location->first);
    if (!current.blueprint_database.blueprints.contains(command) &&
        current.blueprint_database.find_match(command, current.project_summary).empty())
      throw std::runtime_error("Missing blueprint for command: " + ryml_string(command));
  }
  log_phase_time(current.project_name, "blueprint processing", phase_start);

  phase_start = std::chrono::steady_clock::now();
  if (!reuse_cached_state || current.target_database.load(current.output_path / "target_database.json") != yakka_status::SUCCESS) {
    current.generate_target_database();
    current.target_database.save(current.output_path / "target_database.json");
  }
  log_phase_time(current.project_name, "target database", phase_start);

  phase_start = std::chrono::steady_clock::now();
  if (!blueprints_loaded)
    current.save_blueprints();
  if (!reuse_cached_state) {
    current.project_summary["build_request_signature"] << signature;
    current.save_summary();
  }
  log_phase_time(current.project_name, "persist project state", phase_start);

  auto &engine = *build.engine;
  engine.project_name = current.project_name;
  bool task_state_loaded = false;
  if (reuse_cached_state) {
    phase_start = std::chrono::steady_clock::now();
    task_state_loaded = engine.load_todolist(current.output_path / "todolist.json", current) == yakka_status::SUCCESS;
    log_phase_time(current.project_name, "todolist restore", phase_start);
  }
  if (!task_state_loaded) {
    engine.taskflow.clear();
    engine.todo_list.clear();
    engine.todo_task_groups.clear();
    engine.create_taskflow(current);
    engine.save_taskflow(current.output_path / "taskflow.json");
  }
  if (!engine.is_valid())
    throw std::runtime_error("Blueprints have a circular dependency");
  log_phase_time(current.project_name, "project preparation total", prepare_start);
}

} // namespace

size_t build_options::default_jobs()
{
  return std::max(1U, std::min(32U, std::thread::hardware_concurrency()));
}

build_session::build_session(workspace &workspace, build_options options)
  : workspace_(workspace), options_(std::move(options))
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
    const auto &resolved = validated->front();
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
    const auto documents = YAML::LoadAll("{" + options_.data + "}");
    if (documents.size() != 1 || !documents.front().IsMap())
      throw std::invalid_argument("Additional build data must be a mapping");
    data = YAML::Dump(documents.front());
  }

  std::vector<build_result> results;
  results.reserve(unique_requests.size());
  std::vector<std::unique_ptr<project_build>> builds;
  for (const auto &request: unique_requests) {
    const auto result_index = results.size();
    results.push_back({ request.project_name, false, {} });
    try {
      auto build = std::make_unique<project_build>();
      build->result_index = result_index;
      build->current = std::make_unique<project>(workspace_, request.project_name);
      build->engine = std::make_unique<task_engine>();
      prepare_project(*build, workspace_, request, options_, data);
      builds.push_back(std::move(build));
    } catch (const std::exception &error) {
      results[result_index].error = error.what();
      spdlog::error("[{}] {}", request.project_name, error.what());
    }
  }

  std::map<std::string, size_t> target_claims;
  for (const auto &build: builds)
    for (const auto &[target, matches]: build->current->target_database.targets) {
      if (build->current->commands.contains(target))
        continue;
      if (std::none_of(matches.begin(), matches.end(), [](const auto &match) { return match->blueprint->process.valid(); }))
        continue;
      const auto path = fs::absolute(ryml_string(target)).lexically_normal().generic_string();
      const auto [claim, inserted] = target_claims.emplace(path, build->result_index);
      if (!inserted && claim->second != build->result_index) {
        results[claim->second].error = "Conflicting output target: " + path;
        results[build->result_index].error = "Conflicting output target: " + path;
      }
    }

  tf::Executor executor(options_.jobs);
  std::vector<std::pair<project_build *, tf::Future<void>>> running;
  running.reserve(builds.size());
  for (auto &build: builds) {
    if (!results[build->result_index].error.empty())
      continue;
    if (ui)
      ui->init(*build->engine);
  }
  for (auto &build: builds) {
    if (!results[build->result_index].error.empty())
      continue;
    spdlog::info("[{}] Building", build->current->project_name);
    running.emplace_back(build.get(), executor.run(build->engine->taskflow));
  }

  for (auto &[build, future]: running) {
    auto &result = results[build->result_index];
    try {
      while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (ui)
          for (auto &[active, active_future]: running)
            ui->update(*active->engine);
      future.get();
      result.success = !build->engine->abort_build;
      if (!result.success)
        result.error = "Task execution failed";
    } catch (const std::exception &error) {
      if (future.valid())
        future.wait();
      result.success = false;
      result.error = error.what();
    }
  }
  for (auto &[build, future]: running) {
    auto &result = results[build->result_index];
    try {
      const auto save_start = std::chrono::steady_clock::now();
      build->engine->save_todolist(build->current->output_path / "todolist.json");
      log_phase_time(build->current->project_name, "save todolist", save_start);
      if (ui)
        ui->finish(*build->engine);
    } catch (const std::exception &error) {
      result.success = false;
      result.error = error.what();
    }
  }
  return results;
}

} // namespace yakka
