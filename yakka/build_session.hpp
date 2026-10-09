#pragma once

#include "build_request.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace yakka {

class workspace;
class project;
struct task_engine_ui;

struct build_options {
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

  /// Prepare isolated projects serially, then execute their graphs on one worker pool.
  std::vector<build_result> run(const std::vector<build_request> &requests, task_engine_ui *ui = nullptr);

private:
  workspace &workspace_;
  build_options options_;
};

} // namespace yakka