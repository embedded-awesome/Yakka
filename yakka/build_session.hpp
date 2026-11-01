#pragma once

#include "build_request.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yakka {

class workspace;
class project;
struct task_engine_ui;
struct project_build;

enum class build_engine {
  taskflow, // Taskflow engine with todolist.json (default)
  ninja,    // Generate build.ninja and run the embedded Ninja core
};

enum class ninja_action {
  build,      // Regenerate if needed, then build
  regenerate, // Only regenerate build.ninja (invoked by the manifest's generator edge)
  exec,       // Run one target's blueprint process (invoked by `yakka` edges)
};

struct build_options {
  build_engine engine = build_engine::taskflow;
  ninja_action action = ninja_action::build;
  /// Target for ninja_action::exec.
  std::string exec_target;
  /// Absolute path of the running yakka binary, used in generated manifests. The Ninja engine requires it; when empty the taskflow engine is used.
  std::string yakka_binary;
  /// When set, record the generated manifests (or, for the taskflow engine, the inputs of a no-op build) here so later identical invocations can skip loading the workspace.
  std::string fast_path_record;
  bool no_eval = false;
  bool ignore_eval = false;
  bool ignore_yakka = false;
  bool ignore_slc = false;
  std::vector<std::string> slc_features;
  std::string data;
  std::function<void(project &)> fetch_missing;
  size_t jobs = default_jobs();

  static size_t default_jobs();
};

struct build_result {
  std::string project_name;
  bool success = false;
  std::string error;
};

class build_session {
public:
  build_session(workspace &workspace, build_options options = {});

  /// Prepare projects and their project dependencies serially, then execute dependency-ordered graphs on one worker pool.
  std::vector<build_result> run(const std::vector<build_request> &requests, task_engine_ui *ui = nullptr);

private:
  /// Returns nullopt when a request needs a feature the Ninja engine does not support yet.
  std::optional<std::vector<build_result>> run_ninja(const std::vector<build_request> &requests, const std::string &data);
  /// After a taskflow build that ran nothing, record every input stat so an identical invocation can skip loading the workspace.
  void write_legacy_fast_record(const std::vector<std::unique_ptr<project_build>> &builds, const std::vector<build_result> &results, const std::vector<build_request> &requests) const;

  workspace &workspace_;
  build_options options_;
};

} // namespace yakka