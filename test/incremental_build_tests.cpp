// =============================================================================
// Incremental build tests
//
// Covers the second-run path where a project is processed while a previous
// yakka_summary, blueprints.json, target_database.json, taskflow.json and
// todolist.json are still on disk.
//
// Regression driver: a component was edited to require a new feature, that
// feature contributed new `/flags/ld/global` values from a *different*
// component, and the incremental build relinked using the stale options file
// because the `:/components/*/flags/ld/global` data dependency was reported as
// unchanged. A clean build behaved correctly.
//
// Run: ./unit_tests --gtest_filter="ProjectDiffDataDependency*:IncrementalProjectState*:TodoListReload*"
// =============================================================================

#include <gtest/gtest.h>

#include "project_diff.hpp"
#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void silence_logging()
{
  if (!spdlog::get("console")) {
    auto logger = std::make_shared<spdlog::logger>("console", std::make_shared<spdlog::sinks::null_sink_st>());
    spdlog::register_logger(logger);
  }
  if (!spdlog::get("yakkalog")) {
    auto logger = std::make_shared<spdlog::logger>("yakkalog", std::make_shared<spdlog::sinks::null_sink_st>());
    spdlog::set_default_logger(logger);
  }
}

std::string unique_suffix()
{
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// ---------------------------------------------------------------------------
// Section 1 - project_diff data dependency resolution
//
// These drive project_diff directly with two in-memory summaries so the
// expected answer for every data dependency path is unambiguous.
// ---------------------------------------------------------------------------

class ProjectDiffDataDependencyTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    silence_logging();
  }

  // Build the diff between two summary documents exactly as a second run would.
  void diff_summaries(const std::string &before, const std::string &after)
  {
    baseline_tree = ryml::parse_in_arena(ryml::to_csubstr(before));
    current_tree  = ryml::parse_in_arena(ryml::to_csubstr(after));

    diff = std::make_unique<yakka::project_diff>();
    diff->capture_baseline(baseline_tree.rootref());

    std::unordered_set<ryml::csubstr> required_components;
    std::unordered_set<ryml::csubstr> required_features;
    const auto root = current_tree.rootref();
    if (root.has_child("components"))
      for (const auto child: root["components"].children())
        required_components.insert(child.key());
    if (root.has_child("features"))
      for (const auto child: root["features"].children())
        required_features.insert(child.val());

    diff->finalize(root, required_components, required_features);
  }

  bool changed(const char *path) const
  {
    return diff->has_data_changed(ryml::to_csubstr(path));
  }

  ryml::Tree baseline_tree;
  ryml::Tree current_tree;
  std::unique_ptr<yakka::project_diff> diff;

  // A project built before the new feature was required.
  static constexpr const char *summary_before = R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -O2
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
features:
  - base
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
initial:
  components:
    - app
  features: []
)";
};

TEST_F(ProjectDiffDataDependencyTest, WildcardLinkerFlagsDetectNewContributingComponent)
{
  // The reported defect: app now requires a feature, a new component joins the
  // build and contributes /flags/ld/global. The link options file must rebuild.
  diff_summaries(summary_before, R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -O2
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
  ldcheck:
    yakka_file: ldcheck/ldcheck.yakka
    flags:
      ld:
        global:
          - -Wl,--fatal-warnings
features:
  - base
  - ld_check
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
initial:
  components:
    - app
  features: []
)");

  EXPECT_FALSE(diff->empty()) << "A new component and feature must invalidate the cached state";
  EXPECT_TRUE(changed(":/components/*/flags/ld/global"));
}

TEST_F(ProjectDiffDataDependencyTest, WildcardLinkerFlagsDetectModifiedValueWithoutComponentChurn)
{
  // Same component set, only the flag value differs. This cannot be rescued by
  // the components_added/components_removed shortcut, so it exercises the path
  // parsing of ':/components/*/...' directly.
  diff_summaries(summary_before, R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -O2
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
          - -Wl,--fatal-warnings
features:
  - base
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
initial:
  components:
    - app
  features: []
)");

  EXPECT_TRUE(changed(":/components/*/flags/ld/global"));
}

TEST_F(ProjectDiffDataDependencyTest, WildcardLinkerFlagsUnchangedForIdenticalSummaries)
{
  diff_summaries(summary_before, summary_before);

  EXPECT_TRUE(diff->empty());
  EXPECT_FALSE(changed(":/components/*/flags/ld/global"));
  EXPECT_FALSE(changed(":/components/*/sources"));
  EXPECT_FALSE(changed(":/components/*/libraries")) << "A path missing from both summaries contributes nothing";
}

TEST_F(ProjectDiffDataDependencyTest, NamedComponentLocalFlagsTrackTheirOwnComponent)
{
  diff_summaries(summary_before, R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -Os
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
features:
  - base
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
initial:
  components:
    - app
  features: []
)");

  EXPECT_TRUE(changed(":/components/app/flags/c/local"));
  EXPECT_FALSE(changed(":/components/gcc_arm/flags/ld/global"));
  EXPECT_FALSE(changed(":/components/app/sources"));
}

TEST_F(ProjectDiffDataDependencyTest, ComponentPathWithoutSectionPrefixStillResolves)
{
  // ':/<component>/<path>' is the shorthand form and must behave identically to
  // the ':/components/<component>/<path>' form emitted by the toolchains.
  diff_summaries(summary_before, R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -Os
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
features:
  - base
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
initial:
  components:
    - app
  features: []
)");

  EXPECT_TRUE(changed(":/app/flags/c/local"));
  EXPECT_FALSE(changed(":/gcc_arm/flags/ld/global"));
}

TEST_F(ProjectDiffDataDependencyTest, ProjectDataPathsAreTracked)
{
  diff_summaries(summary_before, R"(
components:
  app:
    yakka_file: app/app.yakka
    sources:
      - main.c
    flags:
      c:
        local:
          - -O2
  gcc_arm:
    yakka_file: gcc_arm/gcc_arm.yakka
    flags:
      ld:
        global:
          - -nostartfiles
features:
  - base
tools: {}
choices: {}
data:
  flags:
    c:
      global:
        - -Wall
        - -Werror
initial:
  components:
    - app
  features: []
)");

  EXPECT_TRUE(changed(":/data/flags/c/global"));
  EXPECT_FALSE(changed(":/data/includes/global"));
}

TEST_F(ProjectDiffDataDependencyTest, NonDataDependencyTargetsAreNotDataChanges)
{
  diff_summaries(summary_before, summary_before);

  EXPECT_FALSE(changed("output/project/project.elf_options"));
  EXPECT_FALSE(changed(""));
}

TEST_F(ProjectDiffDataDependencyTest, FirstRunReportsEverythingAsChanged)
{
  yakka::project_diff fresh;
  EXPECT_TRUE(fresh.has_data_changed(ryml::to_csubstr(":/components/*/flags/ld/global")));
  EXPECT_FALSE(fresh.empty()) << "Without a previous summary nothing may be reused";
}

// ---------------------------------------------------------------------------
// Section 2 - whole project second run against an existing summary on disk
// ---------------------------------------------------------------------------

class IncrementalProjectStateTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    silence_logging();

    original_cwd = fs::current_path();
    test_dir     = fs::temp_directory_path() / ("yakka_incremental_" + unique_suffix());
    fs::create_directories(test_dir / ".yakka" / "registries");
    fs::create_directories(test_dir / ".yakka" / "repos");
    fs::current_path(test_dir);

    workspace = std::make_unique<yakka::workspace>();
    workspace->init(test_dir);
  }

  void TearDown() override
  {
    workspace.reset();
    fs::current_path(original_cwd);
    std::error_code error;
    fs::remove_all(test_dir, error);
  }

  void write_component(const std::string &name, const std::string &body)
  {
    const auto dir = test_dir / name;
    fs::create_directories(dir);
    const auto file = dir / (name + ".yakka");
    const bool existed = fs::exists(file);
    {
      std::ofstream stream(file, std::ios::trunc);
      stream << "name: " << name << "\n" << body;
    }
    // Filesystem timestamp granularity can hide an edit made moments after the
    // first run, so push the modification time forward explicitly.
    if (existed)
      fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
  }

  void rescan()
  {
    workspace->local_database.scan_for_components(test_dir);
    (void)workspace->local_database.save();
  }

  // Replays the evaluation half of a yakka run, which is where the summary from
  // the previous run is loaded and diffed.
  std::unique_ptr<yakka::project> run_project(std::vector<std::string> build_string = { "link!", "app" })
  {
    rescan();
    auto project = std::make_unique<yakka::project>(*workspace, "incremental_test");
    project->init_project(build_string);
    project->evaluate_dependencies();
    project->generate_project_summary();
    project->update_project_data();
    project->finalize_diff();
    project->save_summary();
    return project;
  }

  fs::path original_cwd;
  fs::path test_dir;
  std::unique_ptr<yakka::workspace> workspace;
};

TEST_F(IncrementalProjectStateTest, NewlyRequiredFeatureInvalidatesCachedLinkerFlags)
{
  // ldcheck is already part of the build but contributes nothing until the
  // ld_check feature is enabled.
  write_component("ldcheck",
                  "supports:\n"
                  "  features:\n"
                  "    ld_check:\n"
                  "      flags:\n"
                  "        ld:\n"
                  "          global:\n"
                  "            - -Wl,--fatal-warnings\n");
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "requires:\n"
                  "  components:\n"
                  "    - ldcheck\n");

  auto first = run_project();
  ASSERT_TRUE(fs::exists(first->project_summary_file)) << "First run must leave a summary behind";
  EXPECT_FALSE(first->diff.had_previous_state);
  ASSERT_TRUE(first->required_components.contains("ldcheck"));

  // The edit under test: app now requires a feature whose linker flags live in
  // a different component.
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "requires:\n"
                  "  components:\n"
                  "    - ldcheck\n"
                  "  features:\n"
                  "    - ld_check\n");

  auto second = run_project();

  ASSERT_TRUE(second->diff.had_previous_state) << "Second run must load the previous summary";
  EXPECT_TRUE(second->diff.components_modified.contains("app"));
  EXPECT_TRUE(second->required_features.contains("ld_check"));
  EXPECT_TRUE(second->diff.features_added.contains("ld_check"));

  EXPECT_FALSE(second->diff.empty()) << "Blueprints, target database and taskflow must be regenerated";

  // The linker options file depends on this path; it must be reported as changed
  // or the application relinks with the previous flags. No component was added
  // or removed, so this cannot be answered by component churn alone.
  EXPECT_TRUE(second->diff.components_added.empty());
  EXPECT_TRUE(second->diff.has_data_changed(ryml::to_csubstr(":/components/*/flags/ld/global")));
}

TEST_F(IncrementalProjectStateTest, UnchangedProjectReusesCachedState)
{
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "flags:\n"
                  "  ld:\n"
                  "    global:\n"
                  "      - -nostartfiles\n");

  run_project();
  auto second = run_project();

  ASSERT_TRUE(second->diff.had_previous_state);
  EXPECT_TRUE(second->diff.components_modified.empty());
  EXPECT_TRUE(second->diff.components_added.empty());
  EXPECT_TRUE(second->diff.components_removed.empty());
  EXPECT_TRUE(second->diff.empty()) << "An untouched project must reuse the cached blueprints and taskflow";
  EXPECT_FALSE(second->diff.has_data_changed(ryml::to_csubstr(":/components/*/flags/ld/global")));
  EXPECT_FALSE(second->diff.has_data_changed(ryml::to_csubstr(":/components/app/flags/c/local")));
}

TEST_F(IncrementalProjectStateTest, TouchedComponentWithIdenticalContentIsNotModified)
{
  write_component("app",
                  "sources:\n"
                  "  - main.c\n");

  run_project();

  const auto component_file = test_dir / "app" / "app.yakka";
  fs::last_write_time(component_file, fs::last_write_time(component_file) + std::chrono::seconds(5));

  auto second = run_project();

  ASSERT_TRUE(second->diff.had_previous_state);
  EXPECT_TRUE(second->diff.components_modified.empty()) << "Content hashing must absorb a pure timestamp change";
  EXPECT_TRUE(second->diff.empty());
}

TEST_F(IncrementalProjectStateTest, EditedComponentDataInvalidatesItsOwnLocalFlags)
{
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "flags:\n"
                  "  c:\n"
                  "    local:\n"
                  "      - -O2\n");

  run_project();

  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "flags:\n"
                  "  c:\n"
                  "    local:\n"
                  "      - -Os\n");

  auto second = run_project();

  EXPECT_TRUE(second->diff.components_modified.contains("app"));
  EXPECT_TRUE(second->diff.has_data_changed(ryml::to_csubstr(":/components/app/flags/c/local")));
  EXPECT_FALSE(second->diff.has_data_changed(ryml::to_csubstr(":/components/app/sources")));
}

TEST_F(IncrementalProjectStateTest, RemovingARequiredFeatureDropsItFromTheProject)
{
  write_component("ldcheck",
                  "supports:\n"
                  "  features:\n"
                  "    ld_check:\n"
                  "      flags:\n"
                  "        ld:\n"
                  "          global:\n"
                  "            - -Wl,--fatal-warnings\n");
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "requires:\n"
                  "  components:\n"
                  "    - ldcheck\n"
                  "  features:\n"
                  "    - ld_check\n");

  auto first = run_project();
  ASSERT_TRUE(first->required_features.contains("ld_check"));

  // The edit under test: the feature requirement is removed from the component.
  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "requires:\n"
                  "  components:\n"
                  "    - ldcheck\n");

  auto second = run_project();

  ASSERT_TRUE(second->diff.components_modified.contains("app"));
  EXPECT_FALSE(second->required_features.contains("ld_check")) << "A feature no longer required by any component must be dropped";
  EXPECT_TRUE(second->diff.features_removed.contains("ld_check"));
  EXPECT_FALSE(second->diff.empty());

  // The flags the feature contributed must disappear with it.
  EXPECT_TRUE(second->diff.has_data_changed(ryml::to_csubstr(":/components/*/flags/ld/global")));

  // The saved summary must not list the removed feature either.
  const auto summary = yakka::ryml_load_file(second->project_summary_file);
  ASSERT_TRUE(summary.has_value());
  const auto features = summary->crootref()["current"]["features"];
  for (const auto feature: features.children())
    EXPECT_NE(feature.val(), "ld_check") << "The persisted feature list must match the regenerated project";
}

TEST_F(IncrementalProjectStateTest, FeatureFromTheBuildStringSurvivesAComponentEdit)
{
  write_component("app",
                  "sources:\n"
                  "  - main.c\n");

  const std::vector<std::string> build_string{ "link!", "app", "+debug" };

  run_project(build_string);

  write_component("app",
                  "sources:\n"
                  "  - main.c\n"
                  "flags:\n"
                  "  c:\n"
                  "    local:\n"
                  "      - -Os\n");

  auto second = run_project(build_string);

  ASSERT_TRUE(second->diff.components_modified.contains("app"));
  EXPECT_TRUE(second->required_features.contains("debug")) << "Features requested on the command line must be re-added each run";
}

// ---------------------------------------------------------------------------
// Section 3 - reloading a todolist left behind by a previous run
// ---------------------------------------------------------------------------

class TodoListReloadTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    silence_logging();

    original_cwd = fs::current_path();
    test_dir     = fs::temp_directory_path() / ("yakka_todolist_" + unique_suffix());
    fs::create_directories(test_dir);
    fs::current_path(test_dir);

    workspace = std::make_unique<yakka::workspace>();
  }

  void TearDown() override
  {
    workspace.reset();
    fs::current_path(original_cwd);
    std::error_code error;
    fs::remove_all(test_dir, error);
  }

  void write_todolist(const std::string &contents)
  {
    std::ofstream stream(test_dir / "todolist.json", std::ios::trunc);
    stream << contents;
  }

  fs::path original_cwd;
  fs::path test_dir;
  std::unique_ptr<yakka::workspace> workspace;
};

TEST_F(TodoListReloadTest, DataDependenciesAreRebuiltAndDragInTheirDependents)
{
  // A data dependency is persisted with the -1 sentinel because it has no file
  // on disk. It must be re-evaluated on every run, and anything downstream of it
  // must be pulled back into the graph even though its own entry looks current.
  write_todolist(R"({"version":1,"items":{
    ":/components/*/flags/ld/global":{"task_name":":/components/*/flags/ld/global","task_hash":1,"last_modified":-1,"has_match":0,"group_name":"Processing","group_total_count":0,"group_current_count":0,"successors":["project.elf_options"]},
    "project.elf_options":{"task_name":"project.elf_options","task_hash":2,"last_modified":4102444800000000000,"has_match":0,"group_name":"Processing","group_total_count":1,"group_current_count":1,"successors":["project.elf"]},
    "project.elf":{"task_name":"project.elf","task_hash":3,"last_modified":4102444800000000000,"has_match":0,"group_name":"Processing","group_total_count":1,"group_current_count":1,"successors":[]},
    "untouched.o":{"task_name":"untouched.o","task_hash":4,"last_modified":4102444800000000000,"has_match":0,"group_name":"Processing","group_total_count":1,"group_current_count":1,"successors":[]}
  }})");

  yakka::project project(*workspace, "todolist_reload");
  yakka::task_engine engine;

  ASSERT_EQ(engine.load_todolist(test_dir / "todolist.json", project), yakka::yakka_status::SUCCESS);

  EXPECT_EQ(engine.todo_list.count(":/components/*/flags/ld/global"), 1u) << "Data dependencies must always be re-evaluated";
  EXPECT_EQ(engine.todo_list.count("project.elf_options"), 1u) << "A dependent of a re-evaluated data dependency must be rebuilt";
  EXPECT_EQ(engine.todo_list.count("project.elf"), 1u);
  EXPECT_EQ(engine.todo_list.count("untouched.o"), 0u) << "Up to date targets must stay out of the graph";

  // Each target may only appear once, otherwise the graph grows on every run.
  EXPECT_EQ(engine.todo_list.size(), 3u);
}

TEST_F(TodoListReloadTest, SavingCarriesForwardTargetsThatWereSkipped)
{
  write_todolist(R"({"version":1,"items":{
    ":/components/*/flags/ld/global":{"task_name":":/components/*/flags/ld/global","task_hash":1,"last_modified":-1,"has_match":0,"group_name":"Processing","group_total_count":0,"group_current_count":0,"successors":[]},
    "untouched.o":{"task_name":"untouched.o","task_hash":4,"last_modified":4102444800000000000,"has_match":0,"group_name":"Processing","group_total_count":1,"group_current_count":1,"successors":[]}
  }})");

  yakka::project project(*workspace, "todolist_carry_forward");
  yakka::task_engine engine;

  ASSERT_EQ(engine.load_todolist(test_dir / "todolist.json", project), yakka::yakka_status::SUCCESS);
  ASSERT_EQ(engine.todo_list.count("untouched.o"), 0u);

  const auto saved_path = test_dir / "todolist_saved.json";
  engine.save_todolist(saved_path);

  std::ifstream input(saved_path);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string saved = buffer.str();

  EXPECT_NE(saved.find("untouched.o"), std::string::npos) << "Skipped targets must not be dropped from the persisted graph";
  EXPECT_NE(saved.find(":/components/*/flags/ld/global"), std::string::npos);
}

TEST_F(TodoListReloadTest, MissingTodoListIsReported)
{
  yakka::project project(*workspace, "todolist_missing");
  yakka::task_engine engine;

  EXPECT_EQ(engine.load_todolist(test_dir / "no_such_todolist.json", project), yakka::yakka_status::FILE_NOT_FOUND);
}

} // namespace
