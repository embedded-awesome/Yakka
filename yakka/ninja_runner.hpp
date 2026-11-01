#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace yakka::ninja {

enum class run_status {
  success,
  build_failed,
  manifest_failed, // build.ninja could not be loaded or regenerated
  skipped,         // not built because another manifest failed and run_options::all_manifests_required is set
  stale,           // the regenerated manifest requires different project dependencies than `run_request::projects`
};

struct run_request {
  std::string manifest;
  std::vector<std::string> targets;
  /// Prefix for per-build messages such as "no work to do"; useful when several manifests are built together.
  std::string label;
  /// Indices of earlier requests that must build successfully before this one starts.
  std::vector<size_t> dependencies;
  /// Expected value of the manifest's `yakka_projects` variable (the project dependencies it was generated with).
  std::string projects;
};

struct run_options {
  size_t jobs = 1;
  /// Overrides the manifest's top-level `yakka` variable so regeneration and exec-blueprint edges use the running binary.
  std::string yakka_binary;
  /// Record the manifest's generator edge as fresh in .ninja_log; used right after Yakka regenerated the manifest itself.
  bool mark_manifest_fresh = false;
  /// Only bring the manifest up to date; do not build the requested targets.
  bool manifest_only = false;
  bool verbose = false;
  /// Skip every build when any manifest fails to load or regenerate.
  bool all_manifests_required = false;
};

/// Load a Ninja manifest, regenerate it when its generator edge is dirty, then build the targets in-process.
run_status run(const std::string &manifest, const std::vector<std::string> &targets, const run_options &options);

/// Bring each manifest up to date one at a time, then build them in parallel, each after its dependencies succeed.
/// The builds share a single budget of `options.jobs` concurrent commands and a single progress display.
/// @return one status per request, in request order.
std::vector<run_status> run(const std::vector<run_request> &requests, const run_options &options);

} // namespace yakka::ninja
