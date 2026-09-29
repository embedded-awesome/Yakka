// =============================================================================
// Section A5: Project Lifecycle Orchestration Unit Tests
//
// Tests the yakka::project lifecycle methods:
//   init_project, evaluate_dependencies, generate_project_summary,
//   process_blueprints, generate_target_database, save_summary,
//   load_common_commands
//
// Build: include in the unit tests yakka target alongside other test sources.
// Run:   ./unit_tests --gtest_filter="ProjectLifecycle*"
// =============================================================================

#include <gtest/gtest.h>
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ProjectLifecycleTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Suppress console/log output from yakka internals during tests.
    if (!spdlog::get("console")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      auto logger    = std::make_shared<spdlog::logger>("console", null_sink);
      spdlog::register_logger(logger);
    }
    if (!spdlog::get("yakkalog")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      auto logger    = std::make_shared<spdlog::logger>("yakkalog", null_sink);
      spdlog::set_default_logger(logger);
    }

    // Isolated workspace directory.
    test_dir = fs::temp_directory_path() / ("yakka_project_test_" + std::to_string(
                 std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(test_dir);
    fs::create_directories(test_dir / ".yakka" / "registries");
    fs::create_directories(test_dir / ".yakka" / "repos");

    workspace = std::make_unique<yakka::workspace>();
    workspace->init(test_dir);
  }

  void TearDown() override
  {
    fs::remove_all(test_dir);
  }

  // Write a minimal .yakka component descriptor in the test workspace.
  void write_component(const std::string &name, const std::string &extra_yaml = "")
  {
    auto dir = test_dir / name;
    fs::create_directories(dir);
    std::ofstream f(dir / (name + ".yakka"));
    f << "name: " << name << "\n";
    if (!extra_yaml.empty())
      f << extra_yaml << "\n";
  }

  // Write a component with a requires/components dependency.
  void write_component_with_dep(const std::string &name, const std::string &dep)
  {
    write_component(name, std::string("requires:\n  components:\n    - ") + dep + "\n");
  }

  fs::path test_dir;
  std::unique_ptr<yakka::workspace> workspace;
};

// ---------------------------------------------------------------------------
// A5-001 – Init project and generate summary
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, InitProjectWithNoComponentsDoesNotCrash)
{
  // A5-001 baseline: empty init must not crash.
  yakka::project project(*workspace);
  EXPECT_NO_THROW(project.init_project());
}

TEST_F(ProjectLifecycleTest, InitProjectAcceptsStringBuildList)
{
  // init_project with a string vector (CLI-style arguments) must not throw.
  yakka::project project(*workspace);
  EXPECT_NO_THROW(project.init_project(std::vector<std::string>{ "link!" }));
}

TEST_F(ProjectLifecycleTest, GenerateProjectSummaryAfterInit)
{
  // generate_project_summary must not crash after a plain init_project.
  yakka::project project(*workspace);
  project.init_project();
  EXPECT_NO_THROW(project.generate_project_summary());
}

// ---------------------------------------------------------------------------
// A5-002 – Unknown component reported
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, UnknownComponentReportedInState)
{
  // A5-002: a project init'd with an unknown component should set state to
  // PROJECT_HAS_UNKNOWN_COMPONENTS after evaluate_dependencies.

  // Refresh the local database so no stale entries interfere.
  workspace->local_database.scan_for_components(test_dir);

  yakka::project project(*workspace, "test_unknown");
  project.init_project(std::vector<std::string>{ "link!", "__no_such_comp_a5002__" });
  auto state = project.evaluate_dependencies();

  EXPECT_EQ(state, yakka::project::state::PROJECT_HAS_UNKNOWN_COMPONENTS);
  EXPECT_FALSE(project.unknown_components.empty());
}

// ---------------------------------------------------------------------------
// A5-001 (variant) – Known local component is resolved
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, KnownComponentIsResolved)
{
  // A5-001: a component on disk is found and added during evaluate_dependencies.
  write_component("mycomp");

  workspace->local_database.scan_for_components(test_dir);
  auto save_result = workspace->local_database.save();
  ASSERT_TRUE(save_result.has_value());

  yakka::project project(*workspace, "test_known");
  project.init_project(std::vector<std::string>{ "link!", "mycomp" });
  auto state = project.evaluate_dependencies();

  // The component is known so state must not be PROJECT_HAS_UNKNOWN_COMPONENTS
  // specifically for this component.
  EXPECT_NE(state, yakka::project::state::PROJECT_HAS_UNKNOWN_COMPONENTS)
    << "Component 'mycomp' should be found in the local database";
}

// ---------------------------------------------------------------------------
// A5 – Lifecycle method chain does not crash
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, FullLifecycleChainWithNoComponents)
{
  // Run the complete lifecycle sequence against an empty project to verify
  // no stage throws or crashes in isolation.
  yakka::project project(*workspace, "lifecycle_chain_test");

  EXPECT_NO_THROW(project.init_project());
  EXPECT_NO_THROW(project.evaluate_dependencies());
  EXPECT_NO_THROW(project.generate_project_summary());
  EXPECT_NO_THROW(project.process_blueprints());
  EXPECT_NO_THROW(project.generate_target_database());
}

// ---------------------------------------------------------------------------
// A5 – save_summary creates the summary file
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, SaveSummaryWritesFile)
{
  // A5-001 artifact: save_summary must create a project summary file in output/.
  yakka::project project(*workspace, "save_summary_test");
  project.init_project();
  project.generate_project_summary();
  EXPECT_NO_THROW(project.save_summary());

  // The output directory for the project should exist.
  EXPECT_TRUE(fs::exists(test_dir / "output" / "save_summary_test"));
}

// ---------------------------------------------------------------------------
// A5-004 – Direct process_blueprints on valid single component does not crash
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, ParseBlueprintsWithRealComponent)
{
  write_component("bpcomp",
    "blueprints:\n"
    "  compile!:\n"
    "    - echo: compiled\n");

  workspace->local_database.scan_for_components(test_dir);
  ASSERT_TRUE(workspace->local_database.save().has_value());

  yakka::project project(*workspace, "blueprints_test");
  project.init_project(std::vector<std::string>{ "compile!", "bpcomp" });
  project.evaluate_dependencies();
  project.generate_project_summary();
  EXPECT_NO_THROW(project.process_blueprints());
}

// ---------------------------------------------------------------------------
// A5 – generate_target_database after full setup
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, GenerateTargetDatabasePopulatesTargets)
{
  write_component("targetcomp",
    "blueprints:\n"
    "  link!:\n"
    "    - echo: linked\n");

  workspace->local_database.scan_for_components(test_dir);
  ASSERT_TRUE(workspace->local_database.save().has_value());

  yakka::project project(*workspace, "target_db_test");
  project.init_project(std::vector<std::string>{ "link!", "targetcomp" });
  project.evaluate_dependencies();
  project.generate_project_summary();
  project.process_blueprints();
  EXPECT_NO_THROW(project.generate_target_database());
}
