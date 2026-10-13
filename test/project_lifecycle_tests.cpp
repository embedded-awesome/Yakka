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
  EXPECT_FALSE(project.required_features.contains("always"));
  EXPECT_FALSE(project.provided_features.contains("always"));
  EXPECT_TRUE(project.required_features.contains("enabled"));
  EXPECT_TRUE(project.provided_features.contains("enabled"));
  EXPECT_FALSE(project.required_features.contains("disabled"));
  EXPECT_TRUE(project.required_components.contains("driver"));
  EXPECT_TRUE(project.unknown_components.empty());
  EXPECT_EQ(project.project_summary["features"].num_children(), 0);
  EXPECT_EQ(project.project_summary["data"].num_children(), 0);
  EXPECT_FALSE(project.project_summary.has_child("project_name"));
  project.generate_project_summary();
  EXPECT_EQ(project.project_summary["features"].num_children(), 1);
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

TEST_F(ProjectLifecycleTest, OptionalFeatureContributesNothingUntilSelected)
{
  write_component("platform",
    "features:\n"
    "  optional:\n"
    "    sources: [optional.cpp]\n"
    "    includes:\n      global: [optional_include]\n"
    "    flags:\n      c:\n        global: [-DOPTIONAL]\n"
    "    requires:\n      components: [driver]\n      features: [driver_ready]\n");
  write_component("driver", "features:\n  driver_ready: {}\n");
  workspace->local_database.scan_for_components(test_dir);
  for (const bool selected: { false, true }) {
    SCOPED_TRACE(selected);
    yakka::project project(*workspace);
    project.init_project(selected ? std::vector<std::string>{ "platform", "+optional" } : std::vector<std::string>{ "platform" });
    ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
    EXPECT_EQ(project.required_features.contains("optional"), selected);
    EXPECT_EQ(project.provided_features.contains("optional"), selected);
    EXPECT_EQ(project.required_components.contains("driver"), selected);
    EXPECT_EQ(project.provided_features.contains("driver_ready"), selected);
    const auto platform = project.project_summary["components"]["platform"];
    EXPECT_EQ(platform.has_child("sources"), selected);
    EXPECT_EQ(platform.has_child("includes"), selected);
    EXPECT_EQ(platform.has_child("flags"), selected);
    if (selected) {
      EXPECT_EQ(platform["sources"].num_children(), 1);
      EXPECT_EQ(platform["includes"]["global"][0].val(), "optional_include");
      EXPECT_EQ(platform["flags"]["c"]["global"][0].val(), "-DOPTIONAL");
      EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
      EXPECT_EQ(platform["sources"].num_children(), 1);
    }
  }
}

TEST_F(ProjectLifecycleTest, OtherComponentRequirementSelectsOnlyOneArchitecture)
{
  write_component("toolchain",
    "features:\n"
    "  ARMv7-M:\n    flags:\n      c:\n        global: [-march=armv7-m]\n"
    "  ARMv8-M:\n    flags:\n      c:\n        global: [-march=armv8-m.main]\n"
    "choices:\n  architecture:\n    description: Architecture\n"
    "    options:\n      - feature: ARMv7-M\n      - feature: ARMv8-M\n");
  write_component("device", "requires:\n  features: [ARMv8-M]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "toolchain", "device" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  EXPECT_TRUE(project.multiple_answer_choices.empty());
  EXPECT_FALSE(project.required_features.contains("ARMv7-M"));
  EXPECT_FALSE(project.provided_features.contains("ARMv7-M"));
  EXPECT_TRUE(project.provided_features.contains("ARMv8-M"));
  const auto flags = project.project_summary["components"]["toolchain"]["flags"]["c"]["global"];
  ASSERT_EQ(flags.num_children(), 1);
  EXPECT_EQ(flags[0].val(), "-march=armv8-m.main");
}

TEST_F(ProjectLifecycleTest, FeatureChoiceDefaultActivatesOptionalDeclaration)
{
  write_component("platform",
    "features:\n  first:\n    sources: [first.cpp]\n  second: {}\n"
    "choices:\n  mode:\n    description: Mode\n"
    "    options:\n      - feature: first\n      - feature: second\n"
    "    default:\n      feature: first\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.provided_features.contains("first"));
  EXPECT_FALSE(project.required_features.contains("second"));
  EXPECT_EQ(project.project_summary["components"]["platform"]["sources"][0].val(), "first.cpp");
}

TEST_F(ProjectLifecycleTest, TrueConditionAppliesFeaturePayloadAndLateRequirements)
{
  write_component("platform",
    "features:\n"
    "  automatic:\n"
    "    condition: 'true'\n"
    "    requires:\n      components: [driver]\n      features: [optional]\n"
    "    sources: [automatic.cpp]\n"
    "  optional:\n    sources: [optional.cpp]\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_components.contains("driver"));
  EXPECT_TRUE(project.provided_features.contains("automatic"));
  EXPECT_TRUE(project.provided_features.contains("optional"));
  EXPECT_EQ(project.project_summary["components"]["platform"]["sources"].num_children(), 2);
}

TEST_F(ProjectLifecycleTest, ExplicitSelectionDoesNotNeedConditionInputs)
{
  write_component("platform",
    "features:\n  optional:\n    condition: 'data.missing == true'\n    sources: [optional.cpp]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform", "+optional" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.provided_features.contains("optional"));
  EXPECT_EQ(project.project_summary["components"]["platform"]["sources"][0].val(), "optional.cpp");
}

TEST_F(ProjectLifecycleTest, WarmBuildDoesNotRetainPreviouslySelectedOptionalFeature)
{
  write_component("platform",
    "features:\n  optional:\n    requires:\n      components: [driver]\n"
    "blueprints:\n  build:\n    process:\n      - echo: success\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  yakka::build_options options;
  options.ignore_slc = true;
  yakka::build_session session(*workspace, options);
  const auto first = session.run({ { { "platform", "+optional", "build!" }, "optional_warm" } });
  ASSERT_TRUE(first.front().success) << first.front().error;
  const auto second = session.run({ { { "platform", "build!" }, "optional_warm" } });
  ASSERT_TRUE(second.front().success) << second.front().error;
  const auto summary = yakka::ryml_load_file(test_dir / "output/optional_warm" / yakka::project_summary_filename);
  ASSERT_TRUE(summary.has_value());
  EXPECT_EQ(summary->crootref()["current"]["features"].num_children(), 0);
  EXPECT_FALSE(summary->crootref()["current"]["components"].has_child("driver"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsCacheTemplatesAndInputVariables)
{
  write_component("platform",
    "features:\n"
    "  ready:\n"
    "    condition: 'data.device.clock == 20 and data.device.clock > 0 and components.platform.name == \"platform\"'\n");
  write_component("config", "device:\n  clock: 10\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "cached_conditions");
  project.init_project();
  ASSERT_TRUE(project.add_component("platform", project.component_flags));
  ASSERT_TRUE(project.add_component("config", project.component_flags));

  EXPECT_FALSE(project.evaluate_component_features());
  ASSERT_EQ(project.current_state, yakka::project::state::PROJECT_VALID);
  const auto key = std::make_pair(std::string("platform"), std::string("ready"));
  ASSERT_TRUE(project.feature_conditions.contains(key));
  const auto &condition = project.feature_conditions.at(key);
  const auto *parsed = condition.parsed_template.get();
  EXPECT_EQ(condition.input_variables, (std::set<std::string> { "data.device.clock", "components.platform.name" }));

  for (const auto &component: project.components)
    if (component->id == "config")
      component->root["device"]["clock"] << 20;
  ASSERT_TRUE(project.evaluate_component_features());
  EXPECT_EQ(project.feature_conditions.at(key).parsed_template.get(), parsed);
  EXPECT_TRUE(project.provided_features.contains("ready"));
  EXPECT_EQ(project.project_summary["data"].num_children(), 0);
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsRefreshInferredSubtreesWithoutDuplicates)
{
  for (const auto &requirements: { "", "requires:\n  data: [/device]\n", "requires:\n  data: [device]\n" }) {
    SCOPED_TRACE(requirements);
    write_component("platform",
      std::string(requirements) + "device:\n"
      "  clocks: [10]\n"
      "features:\n"
      "  ready:\n"
      "    condition: 'length(data.device.clocks) == 2 and at(data.device.clocks, 1) == 20'\n");
    write_component("config", "device:\n  clocks: [20]\n");
    workspace->local_database.scan_for_components(test_dir);
    yakka::project project(*workspace, "inferred_subtrees");
    project.init_project();
    ASSERT_TRUE(project.add_component("platform", project.component_flags));
    EXPECT_FALSE(project.evaluate_component_features());
    ASSERT_EQ(project.current_state, yakka::project::state::PROJECT_VALID);
    EXPECT_FALSE(project.evaluate_component_features());
    ASSERT_TRUE(project.add_component("config", project.component_flags));
    ASSERT_TRUE(project.evaluate_component_features());
    EXPECT_TRUE(project.provided_features.contains("ready"));
  }
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsReuseTemplatesWhenMissingDataArrives)
{
  write_component("platform",
    "features:\n"
    "  start:\n"
    "    condition: 'true'\n"
    "  ready:\n"
    "    condition: 'data.device.clock == 20'\n");
  write_component("config", "device:\n  clock: 20\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "missing_condition_input");
  project.init_project();
  ASSERT_TRUE(project.add_component("platform", project.component_flags));
  ASSERT_TRUE(project.evaluate_component_features());
  EXPECT_FALSE(project.provided_features.contains("ready"));
  ASSERT_EQ(project.current_state, yakka::project::state::PROJECT_VALID);
  const auto key = std::make_pair(std::string("platform"), std::string("ready"));
  ASSERT_TRUE(project.feature_conditions.contains(key));
  const auto *parsed = project.feature_conditions.at(key).parsed_template.get();

  ASSERT_TRUE(project.add_component("config", project.component_flags));
  ASSERT_TRUE(project.evaluate_component_features());
  EXPECT_EQ(project.feature_conditions.at(key).parsed_template.get(), parsed);
  EXPECT_TRUE(project.provided_features.contains("ready"));
}

TEST_F(ProjectLifecycleTest, ComponentFeatureConditionsMergeInferredLeavesUsingSchema)
{
  write_component("platform",
    "requires:\n"
    "  components: [config]\n"
    "device:\n"
    "  clock: 10\n"
    "data_schema:\n"
    "  device:\n"
    "    type: object\n"
    "    properties:\n"
    "      clock:\n"
    "        type: integer\n"
    "        merge: max\n"
    "features:\n"
    "  ready:\n"
    "    condition: 'data.device.clock == 20'\n");
  write_component("config", "device:\n  clock: 20\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "inferred_leaf_merge");
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
    "  start:\n"
    "    condition: 'true'\n"
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
    "  data: [/items]\n"
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

TEST_F(ProjectLifecycleTest, ExplicitRequirementEnablesFeatureDespiteFalseCondition)
{
  write_component("platform", "requires:\n  features: [ready]\nfeatures:\n  ready:\n    condition: 'false'\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace, "false_requirement");
  project.init_project(std::vector<std::string>{ "platform" });
  EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.provided_features.contains("ready"));
  EXPECT_TRUE(project.unprovided_features.empty());
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

  EXPECT_TRUE(fs::exists(project.project_summary_file));
}

// ---------------------------------------------------------------------------
// A5-004 – Direct process_blueprints on valid single component does not crash
// ---------------------------------------------------------------------------

TEST_F(ProjectLifecycleTest, ParseBlueprintsWithRealComponent)
{
  write_component("bpcomp",
    "blueprints:\n"
    "  compile!:\n"
    "    process:\n"
    "      - echo: compiled\n");

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
    "    process:\n"
    "      - echo: linked\n");

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

TEST_F(ProjectLifecycleTest, BuildSessionReportsUnresolvedFeatureAndRequirementSource)
{
  write_component("device", "requires:\n  features: [ARMv8-M]\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "device", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 1);
  EXPECT_FALSE(results.front().success);
  const auto &error = results.front().error;
  EXPECT_NE(error.find("Unresolved feature 'ARMv8-M'"), std::string::npos) << error;
  EXPECT_NE(error.find((test_dir / "device/device.yakka").generic_string()), std::string::npos) << error;
  EXPECT_NE(error.find("/requires/features/0"), std::string::npos) << error;
  EXPECT_NE(error.find("provides.features"), std::string::npos) << error;
  EXPECT_EQ(error.find("state 6"), std::string::npos);
}

TEST_F(ProjectLifecycleTest, UnresolvedFeatureReportsSupportedRequirementAndCommandLine)
{
  write_component("device",
    "provides:\n  features: [enable]\n"
    "supports:\n  features:\n    enable:\n"
    "      requires:\n        features: [{name: missing}]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "device", "+enable", "+missing" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_UNRESOLVED_REQUIREMENTS);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("/supports/features/enable/requires/features/0"), std::string::npos) << error;
  EXPECT_NE(error.find("selected on the command line"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, BuildSessionReportsExclusiveFeatureChoiceAndAllSources)
{
  write_component("toolchain",
    "provides:\n  features: [ARMv7-M, ARMv8-M]\n"
    "choices:\n  architecture:\n    description: MCU architecture\n"
    "    options:\n      - feature: ARMv7-M\n      - feature: ARMv8-M\n");
  write_component("device", "requires:\n  features: [ARMv8-M]\n");
  workspace->local_database.scan_for_components(test_dir);
  const auto requests = yakka::expand_build_requests({ "toolchain", "device", "+ARMv7-M", "build!" });
  ASSERT_TRUE(requests.has_value());
  yakka::build_session session(*workspace);
  const auto results = session.run(*requests);
  ASSERT_EQ(results.size(), 1);
  EXPECT_FALSE(results.front().success);
  const auto &error = results.front().error;
  EXPECT_NE(error.find("Choice 'architecture' (MCU architecture)"), std::string::npos) << error;
  EXPECT_NE(error.find("is exclusive but has multiple selections"), std::string::npos) << error;
  EXPECT_NE(error.find((test_dir / "toolchain/toolchain.yakka").generic_string()), std::string::npos) << error;
  EXPECT_NE(error.find("/choices/architecture"), std::string::npos) << error;
  EXPECT_NE(error.find("feature 'ARMv7-M'"), std::string::npos) << error;
  EXPECT_NE(error.find("feature 'ARMv8-M'"), std::string::npos) << error;
  EXPECT_NE(error.find((test_dir / "device/device.yakka").generic_string()), std::string::npos) << error;
  EXPECT_NE(error.find("selected on the command line"), std::string::npos) << error;
  EXPECT_NE(error.find("Keep exactly one option"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, ChoiceConflictReportsEnabledDeclarationsAndDefault)
{
  write_component("platform",
    "features:\n  first:\n    condition: '\"second\" in features'\n"
    "provides:\n  features: [second]\n"
    "choices:\n  mode:\n    description: Mode\n"
    "    options:\n      - feature: first\n      - feature: second\n"
    "    default:\n      feature: second\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.multiple_answer_choices.size(), 1);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("/features/first"), std::string::npos) << error;
  EXPECT_NE(error.find("at /default"), std::string::npos) << error;
  EXPECT_NE(error.find("/choices/mode"), std::string::npos) << error;
  project.evaluate_choices();
  EXPECT_EQ(project.multiple_answer_choices.size(), 1);
}

TEST_F(ProjectLifecycleTest, IncompleteChoiceReportsMixedOptionsAndFix)
{
  write_component("platform",
    "choices:\n  mode:\n    description: Mode\n"
    "    options:\n      - feature: first\n      - component: driver\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.incomplete_choices.size(), 1);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("has no selection"), std::string::npos) << error;
  EXPECT_NE(error.find("feature 'first'"), std::string::npos) << error;
  EXPECT_NE(error.find("component 'driver'"), std::string::npos) << error;
  EXPECT_NE(error.find("use +feature"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, MixedChoiceConflictReportsComponentRequirement)
{
  write_component("platform",
    "requires:\n  components: [driver]\n  features: [first]\n"
    "provides:\n  features: [first]\n"
    "choices:\n  mode:\n    description: Mode\n    exclusive: true\n"
    "    options:\n      - feature: first\n      - component: driver\n");
  write_component("driver");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.multiple_answer_choices.size(), 1);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("component 'driver'"), std::string::npos) << error;
  EXPECT_NE(error.find("/requires/components/0"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, NonExclusiveChoiceAllowsMultipleSelectedOptions)
{
  write_component("platform",
    "requires:\n  features: [first, second]\n"
    "provides:\n  features: [first, second]\n"
    "choices:\n  mode:\n    description: Mode\n    exclusive: false\n"
    "    options:\n      - feature: first\n      - feature: second\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  EXPECT_TRUE(project.multiple_answer_choices.empty());
}

TEST_F(ProjectLifecycleTest, DataChoiceCollectsComponentValueWithoutExplicitRequirement)
{
  write_component("platform",
    "logging:\n  level: debug\n"
    "choices:\n  log_level:\n    description: Logging level\n"
    "    data: /logging/level\n    options: [debug, info]\n    default: info\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  EXPECT_TRUE(project.multiple_answer_choices.empty());
  project.generate_project_summary();
  project.update_project_data();
  EXPECT_EQ(project.project_summary["data"]["logging"]["level"].val(), "debug");
  project.validate_schema();
  EXPECT_EQ(project.current_state, yakka::project::state::PROJECT_VALID);
}

TEST_F(ProjectLifecycleTest, DataChoiceDefaultFeedsFeatureConditionsAndSurvivesRecollection)
{
  write_component("platform",
    "choices:\n  log_level:\n    description: Logging level\n"
    "    data: /logging/level\n    options: [debug, info]\n    default: info\n"
    "features:\n  logging_enabled:\n    condition: 'data.logging.level == \"info\"'\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  EXPECT_TRUE(project.required_features.contains("logging_enabled"));
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  for (int i = 0; i < 2; ++i) {
    project.generate_project_summary();
    project.update_project_data();
    EXPECT_EQ(project.project_summary["data"]["logging"]["level"].val(), "info");
  }
}

TEST_F(ProjectLifecycleTest, DataChoiceRequestOverridesDefaultAndIsPersistedAcrossWarmBuilds)
{
  write_component("platform",
    "choices:\n  log_level:\n    description: Logging level\n"
    "    data: /logging/level\n    options: [debug, info]\n    default: info\n"
    "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::build_options options;
  options.ignore_slc = true;
  options.data = "logging: {level: debug}";
  yakka::build_session session(*workspace, options);
  const std::vector<yakka::build_request> requests = { { { "platform", "build!" }, "data_choice" } };
  for (int i = 0; i < 2; ++i) {
    const auto results = session.run(requests);
    ASSERT_TRUE(results.front().success) << results.front().error;
    auto saved = yakka::ryml_load_file(test_dir / "output" / "data_choice" / yakka::project_summary_filename);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->crootref()["current"]["data"]["logging"]["level"].val(), "debug");
  }
}

TEST_F(ProjectLifecycleTest, DataChoiceMissingValueReportsPathEvenForOneOption)
{
  write_component("platform",
    "choices:\n  mode:\n    description: Mode\n"
    "    data: /settings/mode\n    options: [only]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.incomplete_choices.size(), 1);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("/settings/mode"), std::string::npos) << error;
  EXPECT_NE(error.find("/choices/mode"), std::string::npos) << error;
  EXPECT_NE(error.find("--data"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, DataChoiceRejectsUnknownValueInsteadOfApplyingDefault)
{
  write_component("platform",
    "choices:\n  mode:\n    description: Mode\n"
    "    data: /settings/mode\n    options: [first, second]\n    default: first\n"
    "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::build_options options;
  options.ignore_slc = true;
  options.data = "settings: {mode: unknown}";
  yakka::build_session session(*workspace, options);
  const auto results = session.run({ { { "platform", "build!" }, "invalid_data_choice" } });
  ASSERT_FALSE(results.front().success);
  EXPECT_NE(results.front().error.find("invalid value"), std::string::npos) << results.front().error;
  EXPECT_NE(results.front().error.find("/settings/mode"), std::string::npos) << results.front().error;
}

TEST_F(ProjectLifecycleTest, DataChoiceSupportsNonExclusiveArraysAndRejectsExclusiveConflicts)
{
  write_component("platform",
    "settings:\n  mode: [first, second]\n"
    "choices:\n  mode:\n    description: Mode\n"
    "    data: /settings/mode\n    options: [first, second]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.multiple_answer_choices.size(), 1);
  EXPECT_NE(project.evaluation_error().find("/settings/mode"), std::string::npos);
  project.project_summary["choices"]["mode"]["exclusive"] << false;
  project.evaluate_choices();
  EXPECT_TRUE(project.multiple_answer_choices.empty());
  EXPECT_TRUE(project.incomplete_choices.empty());
}

TEST_F(ProjectLifecycleTest, DataChoiceNonExclusiveDefaultArrayIsNotDuplicated)
{
  write_component("platform",
    "choices:\n  mode:\n    description: Mode\n    exclusive: false\n"
    "    data: /settings/mode\n    options: [first, second]\n    default: [first, second]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  for (int i = 0; i < 2; ++i) {
    project.evaluate_choices();
    EXPECT_TRUE(project.incomplete_choices.empty());
    EXPECT_TRUE(project.multiple_answer_choices.empty());
    EXPECT_EQ(project.project_summary["data"]["settings"]["mode"].num_children(), 2);
  }
}

TEST_F(ProjectLifecycleTest, DataChoiceRejectsInvalidDefault)
{
  write_component("platform",
    "choices:\n  mode:\n    description: Mode\n"
    "    data: /settings/mode\n    options: [first, second]\n    default: unknown\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
}

TEST_F(ProjectLifecycleTest, DataChoiceRejectsMalformedDeclarations)
{
  for (const auto declaration: {
         "data: {path: /mode}\n    options: [first]",
         "data: mode\n    options: [first]",
         "data: /mode\n    options: []",
         "data: /mode",
         "data: /mode\n    options: {first: true}" }) {
    write_component("platform",
      std::string("choices:\n  mode:\n    description: Mode\n    ") + declaration + "\n");
    workspace->local_database.scan_for_components(test_dir);
    yakka::project project(*workspace);
    project.init_project(std::vector<std::string>{ "platform" });
    EXPECT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
  }
}

TEST_F(ProjectLifecycleTest, SupportedDataChoiceDefaultsTypedValues)
{
  write_component("platform",
    "requires:\n  features: [enable]\nprovides:\n  features: [enable]\n"
    "supports:\n  features:\n    enable:\n      choices:\n"
    "        number:\n          description: Number\n"
    "          data: /settings/number\n          options: [2]\n          default: 2\n"
    "        nothing:\n          description: Null\n"
    "          data: /settings/nothing\n          options: [null]\n          default: null\n"
    "        array:\n          description: Array\n"
    "          data: /settings/array\n          options: [[a, b]]\n          default: [a, b]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  EXPECT_TRUE(project.multiple_answer_choices.empty());
  const auto settings = project.project_summary["data"]["settings"];
  EXPECT_EQ(settings["number"].val<int>().value(), 2);
  EXPECT_TRUE(settings["nothing"].val_is_null());
  EXPECT_EQ(settings["array"].num_children(), 2);
}

TEST_F(ProjectLifecycleTest, DataChoiceRequestArrayReplacesComponentSelections)
{
  write_component("platform",
    "settings:\n  mode: [first, second]\n"
    "choices:\n  mode:\n    description: Mode\n    exclusive: false\n"
    "    data: /settings/mode\n    options: [first, second]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  auto request_data = project.project_data["request_data"];
  request_data |= ryml::MAP;
  ryml::parse_in_arena("settings: {mode: [second]}", request_data);
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.multiple_answer_choices.empty());
  const auto selection = project.project_summary["data"]["settings"]["mode"];
  ASSERT_EQ(selection.num_children(), 1);
  EXPECT_EQ(selection[0].val(), "second");
}

TEST_F(ProjectLifecycleTest, DataChoicePreservesScalarTypesAndMatchesObjectValues)
{
  write_component("platform",
    "settings:\n  flag: true\n  object: {b: 2, a: 1}\n"
    "choices:\n  flag:\n    description: Flag\n"
    "    data: /settings/flag\n    options: [true, false]\n"
    "  object:\n    description: Object\n"
    "    data: /settings/object\n    options: [{a: 1, b: 2}]\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  EXPECT_TRUE(project.incomplete_choices.empty());
  EXPECT_EQ(project.current_state, yakka::project::state::PROJECT_VALID);
  auto request_data = project.project_data["request_data"];
  request_data |= ryml::MAP;
  ryml::parse_in_arena("settings: {flag: 'true'}", request_data);
  project.evaluate_choices();
  EXPECT_EQ(project.current_state, yakka::project::state::PROJECT_HAS_INVALID_COMPONENT);
  EXPECT_NE(project.evaluation_error().find("invalid value"), std::string::npos);
}

TEST_F(ProjectLifecycleTest, SupportedChoiceReportsOriginalYamlPath)
{
  write_component("platform",
    "requires:\n  features: [enable, first, second]\n"
    "provides:\n  features: [enable, first, second]\n"
    "supports:\n  features:\n    enable:\n"
    "      choices:\n        mode:\n          description: Mode\n"
    "          options:\n            - feature: first\n            - feature: second\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::project project(*workspace);
  project.init_project(std::vector<std::string>{ "platform" });
  ASSERT_EQ(project.evaluate_dependencies(), yakka::project::state::PROJECT_VALID);
  project.evaluate_choices();
  ASSERT_EQ(project.multiple_answer_choices.size(), 1);
  const auto error = project.evaluation_error();
  EXPECT_NE(error.find("/supports/features/enable/choices/mode"), std::string::npos) << error;
}

TEST_F(ProjectLifecycleTest, WarmBuildReportsChoiceSourcesFromFreshDefinitions)
{
  const std::string declarations =
    "provides:\n  features: [first, second]\n"
    "choices:\n  mode:\n    description: Mode\n"
    "    options:\n      - feature: first\n      - feature: second\n";
  write_component("platform", declarations + "blueprints:\n  build:\n    process:\n      - echo: success\n");
  workspace->local_database.scan_for_components(test_dir);
  yakka::build_options options;
  options.ignore_slc = true;
  yakka::build_session session(*workspace, options);
  const std::vector<yakka::build_request> requests = { { { "platform", "+first", "build!" }, "warm_choices" } };
  const auto first = session.run(requests);
  ASSERT_TRUE(first.front().success) << first.front().error;
  write_component("platform", declarations + "requires:\n  features: [second]\n");
  const auto second = session.run(requests);
  ASSERT_FALSE(second.front().success);
  EXPECT_NE(second.front().error.find("is exclusive but has multiple selections"), std::string::npos) << second.front().error;
  EXPECT_NE(second.front().error.find("/requires/features/0"), std::string::npos) << second.front().error;
  EXPECT_NE(second.front().error.find("/choices/mode"), std::string::npos) << second.front().error;
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
    std::vector<bool> include_project_names;

    void init(yakka::task_engine &engine) override
    {
      include_project_names.push_back(engine.include_project_name_in_progress);
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
    EXPECT_EQ(ui.include_project_names, std::vector<bool>(3, true));
    if (budget > 1)
      EXPECT_TRUE(ui.overlap);
  }

  const auto single_request = yakka::expand_build_requests({ "first", "build!" });
  ASSERT_TRUE(single_request.has_value());
  concurrency_ui ui;
  yakka::build_session session(*workspace);
  const auto single_result = session.run(*single_request, &ui);
  ASSERT_EQ(single_result.size(), 1);
  EXPECT_TRUE(single_result.front().success) << single_result.front().error;
  EXPECT_EQ(ui.include_project_names, std::vector<bool>{ false });
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
