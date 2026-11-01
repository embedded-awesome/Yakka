#include "ninja_runner.hpp"

#include "build.h"
#include "build_log.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "line_printer.h"
#include "manifest_parser.h"
#include "metrics.h"
#include "state.h"
#include "status.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace yakka::ninja {
namespace {

std::string format_message(const char *format, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  const int length = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (length <= 0)
    return {};
  std::string text(static_cast<size_t>(length), '\0');
  vsnprintf(text.data(), text.size() + 1, format, args);
  return text;
}

/// Serialises a single StatusPrinter between concurrent builds so progress is aggregated on one line.
class synchronized_status : public Status {
public:
  explicit synchronized_status(const BuildConfig &config)
      : inner_(Status::factory(config)), smart_terminal_(LinePrinter().is_smart_terminal() && config.verbosity == BuildConfig::NORMAL)
  {
  }

  void EdgeAddedToPlan(const Edge *edge) override
  {
    std::lock_guard lock(mutex_);
    inner_->EdgeAddedToPlan(edge);
  }
  void EdgeRemovedFromPlan(const Edge *edge) override
  {
    std::lock_guard lock(mutex_);
    inner_->EdgeRemovedFromPlan(edge);
  }
  void BuildEdgeStarted(const Edge *edge, int64_t start_time_millis) override
  {
    std::lock_guard lock(mutex_);
    inner_->BuildEdgeStarted(edge, start_time_millis);
    line_open_ = smart_terminal_;
  }
  void BuildEdgeFinished(Edge *edge, int64_t start_time_millis, int64_t end_time_millis, ExitStatus exit_code, const std::string &output) override
  {
    std::lock_guard lock(mutex_);
    inner_->BuildEdgeFinished(edge, start_time_millis, end_time_millis, exit_code, output);
    line_open_ = smart_terminal_ && output.empty() && exit_code == ExitSuccess;
  }
  void BuildStarted() override
  {
    std::lock_guard lock(mutex_);
    if (active_builds_++ == 0)
      inner_->BuildStarted();
  }
  void BuildFinished() override
  {
    std::lock_guard lock(mutex_);
    if (active_builds_ > 0 && --active_builds_ == 0) {
      inner_->BuildFinished();
      line_open_ = false;
    }
  }
  void SetExplanations(Explanations *explanations) override
  {
    std::lock_guard lock(mutex_);
    inner_->SetExplanations(explanations);
  }
  void Info(const char *msg, ...) override
  {
    va_list args;
    va_start(args, msg);
    const auto text = format_message(msg, args);
    va_end(args);
    std::lock_guard lock(mutex_);
    end_status_line();
    inner_->Info("%s", text.c_str());
  }
  void Warning(const char *msg, ...) override
  {
    va_list args;
    va_start(args, msg);
    const auto text = format_message(msg, args);
    va_end(args);
    std::lock_guard lock(mutex_);
    end_status_line();
    inner_->Warning("%s", text.c_str());
  }
  void Error(const char *msg, ...) override
  {
    va_list args;
    va_start(args, msg);
    const auto text = format_message(msg, args);
    va_end(args);
    std::lock_guard lock(mutex_);
    end_status_line();
    inner_->Error("%s", text.c_str());
  }

private:
  /// A smart terminal leaves the progress line unterminated; messages from another build must not be appended to it.
  void end_status_line()
  {
    if (!line_open_)
      return;
    fputs("\n", stdout);
    fflush(stdout);
    line_open_ = false;
  }

  std::mutex mutex_;
  std::unique_ptr<Status> inner_;
  const bool smart_terminal_;
  bool line_open_    = false;
  int active_builds_ = 0;
};

/// Counting semaphore for the commands of all concurrent builds.
class job_budget {
public:
  explicit job_budget(size_t jobs) : available_(jobs) {}

  /// Builds blocked in acquire() have nothing running, so they take priority over builds topping up their reservations.
  size_t try_acquire(size_t wanted)
  {
    std::lock_guard lock(mutex_);
    if (waiters_ > 0)
      return 0;
    const auto granted = std::min(wanted, available_);
    available_ -= granted;
    return granted;
  }

  void acquire()
  {
    std::unique_lock lock(mutex_);
    ++waiters_;
    available_changed_.wait(lock, [this] { return available_ > 0; });
    --waiters_;
    --available_;
  }

  void release(size_t count)
  {
    if (count == 0)
      return;
    {
      std::lock_guard lock(mutex_);
      available_ += count;
    }
    available_changed_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable available_changed_;
  size_t available_;
  size_t waiters_ = 0;
};

/// Limits a build's command runner to tokens from a shared job_budget.
/// Tokens are reserved when the builder asks for capacity, and unused reservations are returned
/// before the builder blocks waiting for a command, so idle builds never hold tokens.
class budgeted_command_runner : public CommandRunner {
public:
  budgeted_command_runner(CommandRunner *inner, job_budget &budget, const Plan &plan) : inner_(inner), budget_(budget), plan_(plan) {}
  ~budgeted_command_runner() override
  {
    budget_.release(reserved_ + running_);
  }

  size_t CanRunMore() const override
  {
    // Never reserve more than can be started, so other builds are not starved of tokens this build cannot use.
    const size_t capacity = std::min(inner_->CanRunMore(), plan_.ready_edge_count());
    if (capacity == 0)
      return 0;
    if (reserved_ < capacity)
      reserved_ += budget_.try_acquire(capacity - reserved_);
    // With nothing running the builder cannot wait for a command, so block until another build frees a token.
    if (reserved_ == 0 && running_ == 0) {
      budget_.acquire();
      reserved_ = 1;
    }
    return std::min(reserved_, capacity);
  }

  bool StartCommand(Edge *edge) override
  {
    if (!inner_->StartCommand(edge))
      return false;
    if (reserved_ > 0) {
      --reserved_;
      ++running_;
    }
    return true;
  }

  bool WaitForCommand(Result *result) override
  {
    budget_.release(std::exchange(reserved_, 0));
    const bool finished = inner_->WaitForCommand(result);
    if (result->edge && running_ > 0) {
      --running_;
      budget_.release(1);
    }
    return finished;
  }

  std::vector<Edge *> GetActiveEdges() override
  {
    return inner_->GetActiveEdges();
  }

  void Abort() override
  {
    inner_->Abort();
    budget_.release(std::exchange(reserved_, 0) + std::exchange(running_, 0));
  }

private:
  std::unique_ptr<CommandRunner> inner_;
  job_budget &budget_;
  const Plan &plan_;
  mutable size_t reserved_ = 0;
  size_t running_          = 0;
};

#ifndef _WIN32
/// Ninja's SubprocessSet installs process-wide signal handlers and restores whatever it saw on construction.
/// Concurrent sets restore them in arbitrary order, so the original handlers are put back once all builds finish.
class signal_handler_guard {
public:
  signal_handler_guard()
  {
    for (size_t i = 0; i < signals_.size(); ++i)
      sigaction(signals_[i], nullptr, &saved_[i]);
  }
  ~signal_handler_guard()
  {
    for (size_t i = 0; i < signals_.size(); ++i)
      sigaction(signals_[i], &saved_[i], nullptr);
  }

private:
  static constexpr std::array<int, 4> signals_ = { SIGINT, SIGTERM, SIGHUP, SIGCHLD };
  std::array<struct sigaction, 4> saved_ {};
};
#else
struct signal_handler_guard {};
#endif


/// Mirrors NinjaMain from ninja.cc, without the tools and flag parsing.
struct ninja_session final : public BuildLogUser {
  ninja_session(const BuildConfig &config) : config_(config), start_time_millis_(GetTimeMillis()) {}

  const BuildConfig &config_;
  State state_;
  RealDiskInterface disk_interface_;
  std::string build_dir_;
  BuildLog build_log_;
  DepsLog deps_log_;
  int64_t start_time_millis_;

  bool IsPathDead(StringPiece s) const override
  {
    Node *n = state_.LookupNode(s);
    if (n && n->in_edge())
      return false;
    std::string err;
    const TimeStamp mtime = disk_interface_.Stat(s.AsString(), &err);
    if (mtime == -1)
      Error("%s", err.c_str());
    return mtime == 0;
  }

  std::string log_path(const char *name) const
  {
    return build_dir_.empty() ? std::string(name) : build_dir_ + "/" + name;
  }

  bool ensure_build_dir_exists()
  {
    build_dir_ = state_.bindings_.LookupVariable("builddir");
    if (!build_dir_.empty() && !disk_interface_.MakeDirs(build_dir_ + "/.") && errno != EEXIST) {
      Error("creating build directory %s: %s", build_dir_.c_str(), strerror(errno));
      return false;
    }
    return true;
  }

  bool open_build_log()
  {
    const auto path = log_path(".ninja_log");
    std::string err;
    if (build_log_.Load(path, &err) == LOAD_ERROR) {
      Error("loading build log %s: %s", path.c_str(), err.c_str());
      return false;
    }
    if (!err.empty()) {
      Warning("%s", err.c_str());
      err.clear();
    }
    if (!build_log_.OpenForWrite(path, *this, &err)) {
      Error("opening build log: %s", err.c_str());
      return false;
    }
    return true;
  }

  bool open_deps_log()
  {
    const auto path = log_path(".ninja_deps");
    std::string err;
    if (deps_log_.Load(path, &state_, &err) == LOAD_ERROR) {
      Error("loading deps log %s: %s", path.c_str(), err.c_str());
      return false;
    }
    if (!err.empty()) {
      Warning("%s", err.c_str());
      err.clear();
    }
    if (!deps_log_.OpenForWrite(path, &err)) {
      Error("opening deps log: %s", err.c_str());
      return false;
    }
    return true;
  }

  /// Record the generator edge of a manifest Yakka has just written so Ninja does not regenerate it again.
  void mark_manifest_fresh(const std::string &manifest)
  {
    std::string path = manifest;
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node *node = state_.LookupNode(path);
    if (!node || !node->in_edge())
      return;
    // Use the filesystem clock (as Ninja does for command start times) to timestamp the regeneration.
    const auto stamp_path = log_path(".yakka_regen_stamp");
    std::ofstream(stamp_path, std::ios::trunc) << '\n';
    std::string err;
    const TimeStamp now = disk_interface_.Stat(stamp_path, &err);
    if (now <= 0)
      return;
    build_log_.RecordCommand(node->in_edge(), 0, 0, now);
  }

  /// @return true when the manifest was regenerated and must be reloaded.
  bool rebuild_manifest(const std::string &manifest, std::string *err, Status *status)
  {
    std::string path = manifest;
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node *node = state_.LookupNode(path);
    if (!node)
      return false;

    Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_, status, start_time_millis_);
    if (!builder.AddTarget(node, err))
      return false;
    if (builder.AlreadyUpToDate())
      return false;
    if (builder.Build(err) != ExitSuccess)
      return false;
    // The manifest was only rebuilt if it is still dirty; a restat may have cleaned it.
    if (!node->dirty()) {
      state_.Reset();
      return false;
    }
    return true;
  }

  ExitStatus run_build(const std::vector<std::string> &targets, Status *status, const std::string &label, job_budget *budget)
  {
    const std::string prefix = label.empty() ? std::string() : label + ": ";
    std::string err;
    std::vector<Node *> nodes;
    if (targets.empty()) {
      nodes = state_.DefaultNodes(&err);
      if (!err.empty()) {
        status->Error("%s", err.c_str());
        return ExitFailure;
      }
    }
    for (const auto &target: targets) {
      std::string path = target;
      uint64_t slash_bits;
      CanonicalizePath(&path, &slash_bits);
      Node *node = state_.LookupNode(path);
      if (!node) {
        status->Error("unknown target '%s'", path.c_str());
        return ExitFailure;
      }
      nodes.push_back(node);
    }

    disk_interface_.AllowStatCache(true);
    Builder builder(&state_, config_, &build_log_, &deps_log_, &disk_interface_, status, start_time_millis_);
    if (budget)
      builder.command_runner_.reset(new budgeted_command_runner(CommandRunner::factory(config_, nullptr), *budget, builder.plan_));
    for (auto *node: nodes) {
      if (!builder.AddTarget(node, &err) && !err.empty()) {
        status->Error("%s", err.c_str());
        return ExitFailure;
      }
    }
    // Make sure restat rules do not see stale timestamps.
    disk_interface_.AllowStatCache(false);

    if (builder.AlreadyUpToDate()) {
      if (config_.verbosity != BuildConfig::NO_STATUS_UPDATE)
        status->Info("%sno work to do.", prefix.c_str());
      return ExitSuccess;
    }

    const auto exit_status = builder.Build(&err);
    if (exit_status != ExitSuccess)
      status->Info("%sbuild stopped: %s.", prefix.c_str(), err.c_str());
    return exit_status;
  }

  /// The console pool is a process-wide static with mutable state, and console jobs rely on SIGCHLD,
  /// which cannot be routed to the right thread. Concurrent builds therefore run console jobs normally.
  void disable_console_pool()
  {
    for (auto *edge: state_.edges_)
      if (edge->pool() == &State::kConsolePool)
        edge->pool_ = &State::kDefaultPool;
  }
};

/// Load the manifest and regenerate it until it is up to date.
/// @return the loaded session, or nullptr with the failure in `failure`.
std::unique_ptr<ninja_session> prepare_session(const std::string &manifest, const BuildConfig &config, const run_options &options, Status *status, run_status &failure)
{
  failure         = run_status::manifest_failed;
  bool mark_fresh = options.mark_manifest_fresh;
  // Limit the number of regenerations to prevent infinite loops.
  constexpr int cycle_limit = 100;
  for (int cycle = 1; cycle <= cycle_limit; ++cycle) {
    auto ninja = std::make_unique<ninja_session>(config);
    ManifestParser parser(&ninja->state_, &ninja->disk_interface_, ManifestParserOptions{});
    std::string err;
    if (!parser.Load(manifest, &err)) {
      status->Error("%s", err.c_str());
      return nullptr;
    }
    if (!options.yakka_binary.empty())
      ninja->state_.bindings_.AddBinding("yakka", options.yakka_binary);

    if (!ninja->ensure_build_dir_exists() || !ninja->open_build_log() || !ninja->open_deps_log())
      return nullptr;

    if (mark_fresh) {
      ninja->mark_manifest_fresh(manifest);
      mark_fresh = false;
    }

    if (ninja->rebuild_manifest(manifest, &err, status))
      continue;
    if (!err.empty()) {
      status->Error("rebuilding '%s': %s", manifest.c_str(), err.c_str());
      return nullptr;
    }
    return ninja;
  }

  status->Error("manifest '%s' still dirty after %d tries", manifest.c_str(), cycle_limit);
  return nullptr;
}

} // namespace

run_status run(const std::string &manifest, const std::vector<std::string> &targets, const run_options &options)
{
  return run(std::vector<run_request> { { manifest, targets, {} } }, options).front();
}

std::vector<run_status> run(const std::vector<run_request> &requests, const run_options &options)
{
  BuildConfig config;
  config.parallelism = static_cast<int>(options.jobs == 0 ? 1 : options.jobs);
  config.verbosity   = options.verbose ? BuildConfig::VERBOSE : BuildConfig::NORMAL;
  synchronized_status status(config);

  // Regeneration runs in the console pool and may rewrite shared inputs, so manifests are brought up to date one at a time.
  std::vector<run_status> results(requests.size(), run_status::success);
  std::vector<std::unique_ptr<ninja_session>> sessions(requests.size());
  bool manifest_failed = false;
  for (size_t i = 0; i < requests.size(); ++i) {
    sessions[i] = prepare_session(requests[i].manifest, config, options, &status, results[i]);
    if (sessions[i] && sessions[i]->state_.bindings_.LookupVariable("yakka_projects") != requests[i].projects) {
      sessions[i].reset();
      results[i] = run_status::stale;
    }
    if (!sessions[i]) {
      manifest_failed = true;
      if (options.all_manifests_required)
        break;
    } else {
      results[i] = run_status::success;
    }
  }
  if (options.manifest_only)
    return results;

  std::vector<size_t> builds;
  for (size_t i = 0; i < requests.size(); ++i) {
    if (sessions[i] && !(manifest_failed && options.all_manifests_required))
      builds.push_back(i);
    else if (results[i] == run_status::success)
      results[i] = run_status::skipped;
  }

  std::mutex finished_mutex;
  std::condition_variable finished_changed;
  std::vector<bool> finished(requests.size(), true);
  for (const auto index: builds)
    finished[index] = false;
  const auto finish = [&](size_t index, run_status result) {
    {
      std::lock_guard lock(finished_mutex);
      results[index]  = result;
      finished[index] = true;
    }
    finished_changed.notify_all();
  };

  const auto build = [&](size_t index, job_budget *budget) {
    const auto &dependencies = requests[index].dependencies;
    {
      // Only earlier requests may be dependencies, which rules out waiting on a cycle.
      std::unique_lock lock(finished_mutex);
      finished_changed.wait(lock, [&] {
        return std::all_of(dependencies.begin(), dependencies.end(), [&](size_t dependency) {
          return dependency >= index || finished[dependency];
        });
      });
    }
    for (const auto dependency: dependencies) {
      if (dependency < index && results[dependency] == run_status::success)
        continue;
      const auto &name = dependency >= index ? requests[index].manifest : requests[dependency].label.empty() ? requests[dependency].manifest : requests[dependency].label;
      status.Error("%s%sproject dependency '%s' failed", requests[index].label.c_str(), requests[index].label.empty() ? "" : ": ", name.c_str());
      finish(index, run_status::build_failed);
      return;
    }
    auto result = run_status::build_failed;
    try {
      const auto exit_status = sessions[index]->run_build(requests[index].targets, &status, requests[index].label, budget);
      result                 = exit_status == ExitSuccess ? run_status::success : run_status::build_failed;
    } catch (const std::exception &error) {
      status.Error("%s", error.what());
    }
    finish(index, result);
  };

  if (builds.size() == 1) {
    build(builds.front(), nullptr);
    return results;
  }

  signal_handler_guard signal_guard;
  job_budget budget(static_cast<size_t>(config.parallelism));
  std::vector<std::thread> threads;
  threads.reserve(builds.size());
  for (const auto index: builds) {
    sessions[index]->disable_console_pool();
    threads.emplace_back(build, index, &budget);
  }
  for (auto &thread: threads)
    thread.join();
  return results;
}

} // namespace yakka::ninja
