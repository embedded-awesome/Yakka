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
#include "build_request.hpp"
#include "build_session.hpp"
#include "task_engine.hpp"
#include "yakka_project.hpp"
#include "yakka_workspace.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>

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

TEST_F(ProjectLifecycleTest, ComponentFeaturesEnableOnlyTrueConditions)
{
  write_component("platform",
    "requires:\n"
    "  data: [/config]\n"
    "config:\n"
    "  enabled: true\n"
    "features:\n"
    "  always: {}\n"
    "  enabled:\n"
    "    condition: 'data.config.enabled == true'\n"
    "  disabled:\n"
    "    condition: 'data.config.enabled == false'\n"
    "supports:\n"
    "  features:\n"
    "    enabled:\n"
    "      requires:\n"
    "        components: [driver]\n"
    "    disabled:\n"
    "      requires:\n"
    "        components: [missing]\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "component_features");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("always"));
  EXPECT_TRUE(project.required_features.contains("enabled"));
  EXPECT_TRUE(project.provided_features.contains("enabled"));
  EXPECT_FALSE(project.required_features.contains("disabled"));
  EXPECT_TRUE(project.required_components.contains("driver"));
  EXPECT_TRUE(project.unknown_components.empty());
  EXPECT_EQ(project.project_summary["features"].num_children(), 0);
  EXPECT_EQ(project.project_summary["data"].num_children(), 0);
  EXPECT_FALSE(project.project_summary.has_child("project_name"));
  project.generate_project_summary();
  EXPECT_EQ(project.project_summary["features"].num_children(), 2);
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsSeeSettledSupportedData)
{
  write_component("platform",
    "requires:\n"
    "  components: [config]\n"
    "  features: [configure]\n"
    "provides:\n"
    "  features: [configure]\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.config.mode == \"configured\" and \"configure\" in features'\n");
  write_component("config",
    "supports:\n"
    "  features:\n"
    "    configure:\n"
    "      requires:\n"
    "        data: [/config]\n"
    "      config:\n"
    "        mode: configured\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "settled_data");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("ready"));
}

TEST_F(ProjectLifecycleTest, ComponentFeaturesRepeatResolutionWithFreshPartialData)
{
  write_component("platform",
    "requires:\n"
    "  data: [/items]\n"
    "items: [base]\n"
    "features:\n"
    "  start: {}\n"
    "  finish:\n"
    "    condition: 'data.config.ready == true'\n"
    "supports:\n"
    "  features:\n"
    "    start:\n"
    "      requires:\n"
    "        components: [config]\n"
    "    finish:\n"
    "      requires:\n"
    "        components: [driver]\n");
  write_component("config",
    "requires:\n"
    "  data: [/config, /items]\n"
    "config:\n"
    "  ready: true\n"
    "items: [extra]\n"
    "features:\n"
    "  from_config:\n"
    "    condition: '\"start\" in features'\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "repeated_conditions");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_components.contains("driver"));
  EXPECT_TRUE(project.required_features.contains("finish"));
  EXPECT_TRUE(project.required_features.contains("from_config"));
  EXPECT_EQ(project.project_summary["data"].num_children(), 0);
  project.generate_project_summary();
  project.update_project_data();
  EXPECT_EQ(project.project_summary["data"]["items"].num_children(), 2);
  EXPECT_EQ(project.project_summary["features"].num_children(), 3);
  EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_EQ(project.project_summary["data"]["items"].num_children(), 2);
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsRunAfterChoiceDefaults)
{
  write_component("platform",
    "requires:\n"
    "  data: [/config]\n"
    "choices:\n"
    "  backend:\n"
    "    description: Select backend\n"
    "    options:\n"
    "      - component: config\n"
    "      - component: alternative\n"
    "    default:\n"
    "      component: config\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.config.ready == true'\n");
  write_component("config", "config:\n  ready: true\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "choice_conditions");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("ready"));
  EXPECT_TRUE(project.required_components.contains("config"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsRunAfterRecommendations)
{
  write_component("platform",
    "requires:\n"
    "  data: [/config]\n"
    "  features:\n"
    "    - name: backend\n"
    "      recommends:\n"
    "        component: config\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.config.ready == true'\n");
  write_component("config", "provides:\n  features: [backend]\nconfig:\n  ready: true\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "recommended_conditions");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("ready"));
  EXPECT_TRUE(project.required_components.contains("config"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsRunAfterReplacements)
{
  write_component("platform",
    "requires:\n"
    "  components: [original, replacement]\n"
    "  data: [/config]\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.config.ready == true'\n");
  write_component("original", "config:\n  ready: false\nfeatures:\n  stale: {}\n");
  write_component("replacement", "replaces:\n  components: original\nconfig:\n  ready: true\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "replacement_conditions");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("ready"));
  EXPECT_FALSE(project.required_features.contains("stale"));
  EXPECT_FALSE(project.provided_features.contains("stale"));
  EXPECT_FALSE(project.required_components.contains("original"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureSatisfiesAnExplicitRequirement)
{
  write_component("platform", "requires:\n  features: [ready]\nfeatures:\n  ready:\n    condition: 'true'\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "required_condition");
  project.init_project(std::vector<std::string>{ "platform" });
  EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.provided_features.contains("ready"));
  EXPECT_TRUE(project.unprovided_features.empty());
}

TEST_F(ProjectLifecycleTest, FalseComponentFeatureDoesNotSatisfyARequirement)
{
  write_component("platform", "requires:\n  features: [ready]\nfeatures:\n  ready:\n    condition: 'false'\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "false_requirement");
  project.init_project(std::vector<std::string>{ "platform" });
  EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_UNRESOLVED_REQUIREMENTS);
  EXPECT_FALSE(project.provided_features.contains("ready"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsRejectInvalidExpressionsAndNonBooleans)
{
  for (const auto &expression: { "data.missing", "true and", "42" }) {
    SCOPED_TRACE(expression);
    write_component("platform", std::string("features:\n  invalid:\n    condition: '") + expression + "'\n");
    workspace->local_database.scan_for_components(test_dir);
    yakka::project project(*workspace, "invalid_condition");
    project.init_project(std::vector<std::string>{ "platform" });
    EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
    EXPECT_EQ(project.current_state, yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
  }
}

TEST_F(ProjectLifecycleTest, ComponentFeatureSchemaRejectsMalformedDeclarations)
{
  for (const auto &declaration: { "[ready]", "{ready: true}", "{ready: {condition: true}}",
                                 "{ready: {condition: 1}}", "{ready: {condition: ''}}" }) {
    SCOPED_TRACE(declaration);
    write_component("platform", std::string("features: ") + declaration + "\n");
    workspace->local_database.scan_for_components(test_dir);
    yakka::project project(*workspace, "invalid_declaration");
    project.init_project(std::vector<std::string>{ "platform" });
    EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
  }
}

TEST_F(ProjectLifecycleTest, ComponentFeaturesKeepEnabledSelectionsWhenDataChanges)
{
  write_component("platform",
    "requires:\n"
    "  data: [/settings]\n"
    "settings:\n"
    "  enabled: true\n"
    "data_schema:\n"
    "  settings:\n"
    "    type: object\n"
    "    properties:\n"
    "      enabled:\n"
    "        type: boolean\n"
    "        merge: overwrite\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.settings.enabled == true'\n"
    "supports:\n"
    "  features:\n"
    "    ready:\n"
    "      requires:\n"
    "        components: [config]\n");
  write_component("config", "settings:\n  enabled: false\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "additive_conditions");
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("ready"));
  project.update_project_data();
  EXPECT_EQ(project.project_summary["data"]["settings"]["enabled"].val(), "false");
}

TEST_F(ProjectLifecycleTest, ComponentFeatureEvaluationDoesNotModifySummary)
{
  write_component("platform",
    "requires:\n"
    "  data: [/config]\n"
    "config:\n"
    "  enabled: true\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.config.enabled == true and components.platform.name == \"platform\" and project_name == \"isolated_conditions\"'\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "isolated_conditions");
  project.init_project();
  ASSERT_TRUE(project.add_component("platform", project.component_flags));
  project.project_summary["features"].append_child() << "summary_only";
  project.project_summary["data"]["sentinel"] << "unchanged";
  const auto before = ryml::emitrs_yaml<std::string>(project.project_summary);
  ASSERT_TRUE(project.evaluate_component_features());
  EXPECT_TRUE(project.provided_features.contains("ready"));
  EXPECT_TRUE(project.unprocessed_features.contains("ready"));
  EXPECT_EQ(ryml::emitrs_yaml<std::string>(project.project_summary), before);
}

TEST_F(ProjectLifecycleTest, ComponentFeaturesUseInjectedDataAndReevaluateWarmBuilds)
{
  write_component("platform",
    "features:\n"
    "  ready:\n"
    "    condition: 'data.enabled == true'\n"
    "supports:\n"
    "  features:\n"
    "    ready:\n"
    "      requires:\n"
    "        components: [driver]\n"
    "blueprints:\n"
    "  build:\n"
    "    process:\n"
    "      - echo: done\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "platform", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_options options;
  options.ignore_slc = true;
  for (const auto enabled: { true, true, false, true }) {
    options.data = enabled ? "enabled: true" : "enabled: false";
    yakka::build_session session(*workspace, options);
    const auto results = session.run(*requests);
    ASSERT_EQ(results.size(), 1);
    ASSERT_TRUE(results.front().success) << results.front().error;
    const auto summary = yakka::ryml_load_file(test_dir / "output" / "platform" / yakka::project_summary_filename);
    ASSERT_TRUE(summary.has_value());
    ASSERT_TRUE(summary->crootref().has_child("current"));
    EXPECT_EQ(summary->crootref()["current"]["components"].has_child("driver"), enabled);
    EXPECT_EQ(summary->crootref()["current"]["features"].num_children(), enabled ? 1 : 0);
  }
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

TEST(BuildRequestTest, PreservesSingleProjectArgumentsAndName)
{
  const std::vector<std::string> arguments{ "app", "platform", "+debug", "gcc_arm", "link!" };
  const auto requests = yakka::expand_build_requests(arguments);
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 1);
  EXPECT_EQ(requests->front().arguments, arguments);
  EXPECT_EQ(requests->front().project_name, "app-platform-gcc_arm+debug");
}

TEST(BuildRequestTest, ExpandsCartesianProductInArgumentOrder)
{
  const auto requests = yakka::expand_build_requests({ "app", "[p1, p2]", "[+debug, +release, +test]", "gcc_arm" });
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 6);
  EXPECT_EQ(requests->front().arguments, (std::vector<std::string>{ "app", "p1", "+debug", "gcc_arm" }));
  EXPECT_EQ(requests->back().arguments, (std::vector<std::string>{ "app", "p2", "+test", "gcc_arm" }));
  EXPECT_EQ((*requests)[3].project_name, "app-p2-gcc_arm+debug");
}

TEST(BuildRequestTest, AcceptsSplitArgumentsAndQuotedScalarAlternatives)
{
  const auto requests = yakka::expand_build_requests({ "app", "['p1',", "\"p2\",", "]", "gcc_arm" });
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 2);
  EXPECT_EQ(requests->back().project_name, "app-p2-gcc_arm");
}

TEST(BuildRequestTest, SupportsMixedComponentAndFeatureAlternatives)
{
  const auto requests = yakka::expand_build_requests({ "app", "[platform, +debug]" });
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 2);
  EXPECT_EQ(requests->front().project_name, "app-platform");
  EXPECT_EQ(requests->back().project_name, "app+debug");
}

TEST(BuildRequestTest, UsesOverrideAsMatrixPrefix)
{
  const auto requests = yakka::expand_build_requests({ "app", "[p1, p2]", "[+debug, +release]" }, "custom");
  ASSERT_TRUE(requests.has_value()) << requests.error();
  EXPECT_EQ(requests->front().project_name, "custom-p1+debug");
  EXPECT_EQ(requests->back().project_name, "custom-p2+release");
  const auto single = yakka::expand_build_requests({ "app", "platform" }, "custom");
  ASSERT_TRUE(single.has_value());
  EXPECT_EQ(single->front().project_name, "custom");
}

TEST(BuildRequestTest, DeduplicatesRepeatedAlternatives)
{
  const auto requests = yakka::expand_build_requests({ "app", "[p1, p1]" }, "custom");
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 1);
  EXPECT_EQ(requests->front().project_name, "custom");
}

TEST(BuildRequestTest, RejectsMalformedAlternatives)
{
  for (const auto &argument: { "[]", "[p1,", "[p1,,p2]", "[[p1],p2]", "[{key: value}]", "[null]", "['']", "[+]", "[link!]", "[two words]", "p1]", "[p1]junk", "[p1][p2]" }) {
    SCOPED_TRACE(argument);
    EXPECT_FALSE(yakka::expand_build_requests({ "app", argument }).has_value());
  }
  EXPECT_FALSE(yakka::expand_build_requests({ "app", "" }).has_value());
}

TEST(BuildRequestTest, DeduplicatesEquivalentMixedAlternativeSelections)
{
  const auto requests = yakka::expand_build_requests({ "app", "[platform, +debug]", "[+debug, platform]", "build!" });
  ASSERT_TRUE(requests.has_value()) << requests.error();
  ASSERT_EQ(requests->size(), 3);
  EXPECT_EQ(requests->front().project_name, "app-platform+debug");
}

TEST(BuildRequestTest, RejectsOutputCollisionsAndUnsafeNames)
{
  EXPECT_FALSE(yakka::expand_build_requests({ "app", "[a-b, a]", "[c, b-c]" }).has_value());
  EXPECT_FALSE(yakka::expand_build_requests({ "app" }, "../outside").has_value());
  EXPECT_FALSE(yakka::expand_build_requests({ "app" }, "..").has_value());
}

TEST(BuildRequestTest, LimitsCartesianExpansionBeforeProjectCreation)
{
  std::vector<std::string> arguments(7, "[a, b, c, d]");
  const auto requests = yakka::expand_build_requests(arguments);
  ASSERT_FALSE(requests.has_value());
  EXPECT_NE(requests.error().find("4096"), std::string::npos);
}

TEST_F(ProjectLifecycleTest, BuildSessionRunsIndependentProjectOutputs)
{
  for (const auto &name: { "first", "second" })
    write_component(name,
      "blueprints:\n"
      "  build:\n"
      "    depends: ['{{project_output}}/result.txt']\n"
      "  '{{project_output}}/result.txt':\n"
      "    process:\n"
      "      - inja: '{{project_name}}'\n"
      "      - save:\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "[first, second]", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_options options;
  options.ignore_slc = true;
  options.jobs = 2;
  yakka::build_session session(*workspace, options);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 2);
  for (const auto &result: results) {
    EXPECT_TRUE(result.success) << result.error;
    std::ifstream artifact(test_dir / "output" / result.project_name / "result.txt");
    std::string contents;
    std::getline(artifact, contents);
    EXPECT_EQ(contents, result.project_name);
  }
}

TEST_F(ProjectLifecycleTest, BuildSessionContinuesAfterPreparationFailure)
{
  write_component("valid", "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "[missing, valid]", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 2);
  EXPECT_FALSE(results.front().success);
  EXPECT_NE(results.front().error.find("Missing components"), std::string::npos);
  EXPECT_TRUE(results.back().success) << results.back().error;
}

TEST_F(ProjectLifecycleTest, BuildSessionRejectsConflictingOutputsBeforePreparation)
{
  yakka::build_session session(*workspace);
  EXPECT_THROW(session.run({ { { "first", "build!" }, "same" }, { { "second", "build!" }, "same" } }), std::invalid_argument);
  EXPECT_FALSE(fs::exists(test_dir / "output" / "same"));
}

TEST_F(ProjectLifecycleTest, BuildSessionRejectsZeroWorkerBudget)
{
  yakka::build_options options;
  options.jobs = 0;
  EXPECT_THROW(yakka::build_session(*workspace, options), std::invalid_argument);
}

TEST_F(ProjectLifecycleTest, BuildSessionReportsTaskFailureAndPreservesIndependentSuccess)
{
  write_component("failed", "blueprints:\n  build:\n    process:\n      - nonexistent_tool: failure\n");
  write_component("valid", "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "[failed, valid]", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 2);
  EXPECT_FALSE(results.front().success);
  EXPECT_TRUE(results.back().success) << results.back().error;
}

TEST(TaskEngineValidationTest, RejectsCycleWithoutTerminalNode)
{
  yakka::task_engine engine;
  auto first = engine.taskflow.emplace([] {});
  auto second = engine.taskflow.emplace([] {});
  first.precede(second);
  second.precede(first);
  EXPECT_FALSE(engine.is_valid());
}

TEST_F(ProjectLifecycleTest, BuildSessionPreservesWarmArtifactsAndRebuildsDeletedOutputs)
{
  write_component("cached",
    "blueprints:\n"
    "  build:\n"
    "    depends: ['{{project_output}}/result.txt']\n"
    "  '{{project_output}}/result.txt':\n"
    "    process:\n"
    "      - inja: '{{project_name}}'\n"
    "      - save:\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "cached", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  ASSERT_TRUE(session.run(*requests).front().success);
  const auto artifact = test_dir / "output" / "cached" / "result.txt";
  ASSERT_TRUE(fs::exists(artifact));
  const auto timestamp = fs::last_write_time(artifact);
  const auto warm = session.run(*requests);
  ASSERT_TRUE(warm.front().success) << warm.front().error;
  EXPECT_EQ(fs::last_write_time(artifact), timestamp);
  fs::remove(artifact);
  const auto rebuilt = session.run(*requests);
  ASSERT_TRUE(rebuilt.front().success) << rebuilt.front().error;
  EXPECT_TRUE(fs::exists(artifact));
}

TEST_F(ProjectLifecycleTest, BuildSessionTracksInjectedDataAcrossWarmBuilds)
{
  write_component("data_build",
    "blueprints:\n"
    "  build:\n"
    "    depends: ['{{project_output}}/result.txt']\n"
    "  '{{project_output}}/result.txt':\n"
    "    depends:\n"
    "      - data: ':/data/value'\n"
    "    process:\n"
    "      - inja: '{{data.value}}'\n"
    "      - save:\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "data_build", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_options options;
  options.data = "value: first";
  yakka::build_session first(*workspace, options);
  ASSERT_TRUE(first.run(*requests).front().success);
  options.data = "value: second";
  yakka::build_session second(*workspace, options);
  const auto rebuilt = second.run(*requests);
  ASSERT_TRUE(rebuilt.front().success) << rebuilt.front().error;
  const auto artifact = test_dir / "output" / "data_build" / "result.txt";
  std::ifstream input(artifact);
  std::string contents;
  std::getline(input, contents);
  EXPECT_EQ(contents, "second");
  const auto timestamp = fs::last_write_time(artifact);
  ASSERT_TRUE(second.run(*requests).front().success);
  EXPECT_EQ(fs::last_write_time(artifact), timestamp);
}

TEST_F(ProjectLifecycleTest, BuildSessionRetriesFailedTasksOnWarmBuild)
{
  write_component("failed", "blueprints:\n  build:\n    process:\n      - nonexistent_tool: failure\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "failed", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  EXPECT_FALSE(session.run(*requests).front().success);
  EXPECT_FALSE(session.run(*requests).front().success);
}

TEST_F(ProjectLifecycleTest, BuildSessionUsesOneSharedWorkerBudget)
{
  struct concurrency_ui : yakka::task_engine_ui {
    std::mutex mutex;
    std::condition_variable condition;
    int active = 0;
    int peak = 0;
    int arrived = 0;
    int completed = 0;
    int budget = 1;
    bool overlap = false;

    void init(yakka::task_engine &engine) override
    {
      engine.taskflow.emplace([this] {
        std::unique_lock lock(mutex);
        ++active;
        ++arrived;
        peak = std::max(peak, active);
        condition.notify_all();
        if (budget > 1)
          overlap = condition.wait_for(lock, std::chrono::seconds(2), [this] { return arrived >= 2; });
        --active;
        ++completed;
      });
    }
    void update(yakka::task_engine &) override {}
    void finish(yakka::task_engine &) override {}
  };

  for (const auto &name: { "first", "second", "third" })
    write_component(name, "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "[first, second, third]", "build!" });
  ASSERT_TRUE(requests.has_value());
  for (const auto budget: { 1, 2 }) {
    concurrency_ui ui;
    ui.budget = budget;
    yakka::build_options options;
    options.jobs = budget;
    yakka::build_session session(*workspace, options);
    const auto results = session.run(*requests, &ui);
    for (const auto &result: results)
      EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(ui.completed, 3);
    EXPECT_EQ(ui.peak, budget);
    if (budget > 1)
      EXPECT_TRUE(ui.overlap);
  }
}

TEST_F(ProjectLifecycleTest, BuildSessionRejectsSharedGeneratedFileClaims)
{
  const auto shared_output = test_dir / "output" / "shared.txt";
  const std::string blueprint =
    "blueprints:\n"
    "  build:\n"
    "    depends: ['{{project_output}}/../shared.txt']\n"
    "  '{{project_output}}/../shared.txt':\n"
    "    process:\n"
    "      - echo: shared\n"
    "      - save:\n";
  write_component("first", blueprint);
  write_component("second", blueprint);
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "[first, second]", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 2);
  for (const auto &result: results) {
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("Conflicting output target"), std::string::npos) << result.error;
  }
  EXPECT_FALSE(fs::exists(shared_output));
}

TEST_F(ProjectLifecycleTest, BuildSessionAcceptsRegexCommandBlueprint)
{
  write_component("regex_root",
    "blueprints:\n"
    "  builder:\n"
    "    regex: 'build_.+'\n"
    "    process:\n"
    "      - echo: matched\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "regex_root", "build_one!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 1);
  EXPECT_TRUE(results.front().success) << results.front().error;
}

TEST_F(ProjectLifecycleTest, BuildSessionReportsUnresolvedFeatureRequirements)
{
  write_component("app",
    "provides:\n  features: [known]\n"
    "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "app", "[+missing, +known]", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 2);
  EXPECT_FALSE(results.front().success);
  EXPECT_TRUE(results.back().success) << results.back().error;
}
