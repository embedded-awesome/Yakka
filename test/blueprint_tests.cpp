#include "blueprint_commands.hpp"
#include "blueprint_database.hpp"
#include "target_database.hpp"
#include "task_engine.hpp"
#include "utilities.hpp"
#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <chrono>
#include <deque>
#include <fstream>

namespace {

std::vector<std::string> strings(const std::vector<c4::csubstr> &values)
{
  std::vector<std::string> result;
  for (const auto value: values)
    result.push_back(yakka::ryml_string(value));
  return result;
}

class BlueprintTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    if (!spdlog::get("console"))
      spdlog::register_logger(std::make_shared<spdlog::logger>("console", std::make_shared<spdlog::sinks::null_sink_st>()));
    directory = fs::temp_directory_path() / ("yakka_blueprints_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(fs::create_directory(directory));
    summary = ryml::parse_in_arena(R"(
components:
  app:
    flags: {first: one}
  driver:
    flags: {second: two}
data:
  flags: {third: three}
name: example
tools: {}
features: []
)");
  }

  void TearDown() override
  {
    std::error_code error;
    fs::remove_all(directory, error);
    EXPECT_FALSE(error) << error.message();
  }

  ryml::ConstNodeRef parse(const std::string &yaml)
  {
    trees.push_back(ryml::parse_in_arena(c4::to_csubstr(yaml)));
    return trees.back().crootref();
  }

  fs::path write(const std::string &name, const std::string &text)
  {
    const auto path = directory / name;
    std::ofstream stream(path);
    stream << text;
    EXPECT_TRUE(stream.good());
    return path;
  }

  void add(const std::string &target, const std::string &yaml)
  {
    database.create_blueprint(target, parse(yaml), "component");
  }

  fs::path directory;
  ryml::Tree summary;
  std::deque<ryml::Tree> trees;
  yakka::blueprint_database database;
};

TEST_F(BlueprintTest, ParsesAllBlueprintFieldsAndDependencyForms)
{
  const auto root = parse(R"(
regex: '(.+)\.o'
requires: [compiler, archiver]
group: Compile
depends:
  - input.cpp
  - :/data/value
  - data: /components/app/flags
  - data: [/data/first, /data/second]
  - dependency_file: input.d
process:
  - echo: hello
)");
  yakka::blueprint blueprint("object", root, "components/compiler");
  EXPECT_EQ(blueprint.target, c4::to_csubstr("object"));
  EXPECT_EQ(blueprint.parent_path, c4::to_csubstr("components/compiler"));
  ASSERT_TRUE(blueprint.regex.has_value());
  EXPECT_EQ(*blueprint.regex, c4::to_csubstr(R"((.+)\.o)"));
  EXPECT_EQ(strings(blueprint.requirements), (std::vector<std::string>{"compiler", "archiver"}));
  EXPECT_EQ(blueprint.task_group, c4::to_csubstr("Compile"));
  ASSERT_EQ(blueprint.dependencies.size(), 6u);
  EXPECT_EQ(blueprint.dependencies[0].type, yakka::blueprint::dependency::DEFAULT_DEPENDENCY);
  for (size_t i = 1; i < 5; ++i)
    EXPECT_EQ(blueprint.dependencies[i].type, yakka::blueprint::dependency::DATA_DEPENDENCY);
  EXPECT_EQ(blueprint.dependencies[5].type, yakka::blueprint::dependency::DEPENDENCY_FILE_DEPENDENCY);
  EXPECT_EQ(blueprint.dependencies[5].name, c4::to_csubstr("input.d"));
  ASSERT_TRUE(blueprint.process.is_seq());
  EXPECT_EQ(blueprint.process[0]["echo"].val(), c4::to_csubstr("hello"));
}

TEST_F(BlueprintTest, OptionalFieldsAndUnknownDependencyMapsDoNotCreateDependencies)
{
  yakka::blueprint blueprint("empty", parse("depends: [{unknown: ignored}]\n"), "");
  EXPECT_FALSE(blueprint.regex.has_value());
  EXPECT_TRUE(blueprint.requirements.empty());
  EXPECT_TRUE(blueprint.dependencies.empty());
  EXPECT_TRUE(blueprint.task_group.empty());
  EXPECT_FALSE(blueprint.process.valid());
}

TEST_F(BlueprintTest, ExactMatchingReturnsAllProvidersButNotPartialMatches)
{
  add("build", "process: [{echo: first}]");
  add("build", "process: [{echo: second}]");
  EXPECT_EQ(database.find_match("build", summary.crootref()).size(), 2u);
  EXPECT_TRUE(database.find_match("build_extra", summary.crootref()).empty());
  EXPECT_TRUE(database.find_match(":/data/missing", summary.crootref()).empty());
}

TEST_F(BlueprintTest, RegexCapturesRenderDependenciesAndRequireFullMatch)
{
  add(R"(out/([^/]+)/(.*)\.o)", R"(
regex: 'out/([^/]+)/(.*)\.o'
depends:
  - '{{ curdir() }}/{{ $(1) }}/{{ $(2) }}'
  - '{{ $(0) }}.options'
  - '{{ render("{{ name }}") }}'
)");
  const auto matches = database.find_match("out/app/main.cpp.o", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(strings(matches[0]->regex_matches),
            (std::vector<std::string>{"out/app/main.cpp.o", "app", "main.cpp"}));
  EXPECT_EQ(strings(matches[0]->dependencies),
            (std::vector<std::string>{"component/app/main.cpp", "out/app/main.cpp.o.options", "example"}));
  EXPECT_TRUE(database.find_match("prefix/out/app/main.cpp.o", summary.crootref()).empty());
}

TEST_F(BlueprintTest, DependenciesExpandListsNormalizeRelativePathsAndDataPrefixes)
{
  add("build", R"(
depends:
  - '[./first.cpp, .//second.cpp, third.cpp,]'
  - ./plain.cpp
  - data: /components/{{ name }}/flags
  - :/data/value
)");
  const auto matches = database.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(strings(matches[0]->dependencies),
            (std::vector<std::string>{"first.cpp", "second.cpp", "third.cpp", "plain.cpp",
                                     ":/components/example/flags", ":/data/value"}));
}

TEST_F(BlueprintTest, EmptyDependencyExpansionContributesNoFile)
{
  add("build", "depends: ['{{ \"\" }}', '[]']\n");
  const auto matches = database.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_TRUE(matches[0]->dependencies.empty());
}

TEST_F(BlueprintTest, InvalidDependencyTemplateDoesNotProducePartialMatch)
{
  add("build", "depends: [first, '{{ name']\n");
  EXPECT_TRUE(database.find_match("build", summary.crootref()).empty());
}

TEST_F(BlueprintTest, AggregateMapCombinesComponentsAndProjectData)
{
  add("build", R"(
depends:
  - '{{ aggregate("/flags").first }}'
  - '{{ aggregate("/flags").second }}'
  - '{{ aggregate("/flags").third }}'
)");
  const auto matches = database.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(strings(matches[0]->dependencies), (std::vector<std::string>{"one", "two", "three"}));
}

TEST_F(BlueprintTest, AggregateListsRenderScalarsAndPreserveNestedItems)
{
  summary = ryml::parse_in_arena(R"(
components:
  app: {inputs: ['{{ name }}.cpp', {nested: [one, two]}]}
  driver: {inputs: driver.cpp}
data: {inputs: [project.cpp]}
name: example
)");
  inja::Environment env;
  ryml::Tree output;
  auto values = yakka::aggregate_blueprint_data(env, summary.crootref(), "/inputs", output.rootref());
  ASSERT_TRUE(values.is_seq());
  ASSERT_EQ(values.num_children(), 4u);
  EXPECT_EQ(values[0].val(), c4::to_csubstr("example.cpp"));
  EXPECT_EQ(values[1]["nested"][1].val(), c4::to_csubstr("two"));
  EXPECT_EQ(values[2].val(), c4::to_csubstr("driver.cpp"));
  EXPECT_EQ(values[3].val(), c4::to_csubstr("project.cpp"));
}

TEST_F(BlueprintTest, AggregateMapsPreserveSubtreesAndProjectDataOverrides)
{
  summary = ryml::parse_in_arena(R"(
components:
  app: {flags: {nested: {first: one}, shared: old}}
  driver: {flags: {nested: {second: two}}}
data: {flags: {shared: new}}
)");
  inja::Environment env;
  ryml::Tree output;
  auto values = yakka::aggregate_blueprint_data(env, summary.crootref(), "/flags", output.rootref());
  ASSERT_TRUE(values.is_map());
  EXPECT_EQ(values["nested"]["first"].val(), c4::to_csubstr("one"));
  EXPECT_EQ(values["nested"]["second"].val(), c4::to_csubstr("two"));
  EXPECT_EQ(values["shared"].val(), c4::to_csubstr("new"));
}

TEST_F(BlueprintTest, AggregateMissingPathsAreEmptyAndMixedShapesAreRejected)
{
  inja::Environment env;
  ryml::Tree output;
  EXPECT_EQ(yakka::aggregate_blueprint_data(env, summary.crootref(), "/missing", output.rootref()).num_children(), 0u);
  summary = ryml::parse_in_arena("components: {app: {flags: [one]}}\ndata: {flags: {second: two}}\n");
  ryml::Tree mixed;
  EXPECT_THROW(yakka::aggregate_blueprint_data(env, summary.crootref(), "/flags", mixed.rootref()), std::invalid_argument);
}

TEST_F(BlueprintTest, SelectCallbackIsCurrentlyUnsupported)
{
  add("build", "depends: ['{{ select(\"feature\") }}']\n");
  const auto matches = database.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_TRUE(matches[0]->dependencies.empty());
}

TEST_F(BlueprintTest, ParsesCompilerDependencyFilesIncludingSameLineAndContinuations)
{
  const auto path = write("compile.d", "output.o: ./source.cpp first.hpp \\\n second.hpp\r\n");
  EXPECT_EQ(strings(database.parse_gcc_dependency_file(path.string())),
            (std::vector<std::string>{"source.cpp", "first.hpp", "second.hpp"}));
  EXPECT_TRUE(database.parse_gcc_dependency_file((directory / "missing.d").string()).empty());
}

TEST_F(BlueprintTest, CompilerDependencyFilesHandleEscapedSpacesAndEmptyFiles)
{
  const auto path = write("spaces.d", "output.o: include\\ path/header.hpp\tplain.hpp\n");
  EXPECT_EQ(strings(database.parse_gcc_dependency_file(path.string())),
            (std::vector<std::string>{"include path/header.hpp", "plain.hpp"}));
  EXPECT_TRUE(database.parse_gcc_dependency_file(write("empty.d", "").string()).empty());
  EXPECT_TRUE(database.parse_gcc_dependency_file(write("invalid.d", "not a dependency rule").string()).empty());
}

TEST_F(BlueprintTest, CompilerDependencyFilesIgnoreAdditionalPhonyRules)
{
  const auto path = write("phony.d", "output.o: source.cpp header.hpp\n\nheader.hpp:\n");
  EXPECT_EQ(strings(database.parse_gcc_dependency_file(path.string())),
            (std::vector<std::string>{"source.cpp", "header.hpp"}));
}

TEST_F(BlueprintTest, EmptyTargetAndEmptyDataDependencyAreHandledSafely)
{
  EXPECT_TRUE(database.find_match("", summary.crootref()).empty());
  add("build", "depends: [{data: '{{ \"\" }}'}]\n");
  EXPECT_TRUE(database.find_match("build", summary.crootref()).empty());
}

TEST_F(BlueprintTest, LoadingMissingBlueprintFilePreservesExistingEntries)
{
  add("build", "process: [{echo: existing}]\n");
  EXPECT_NO_THROW(database.load(directory / "missing.json"));
  EXPECT_EQ(database.find_match("build", summary.crootref()).size(), 1u);
}

TEST_F(BlueprintTest, DependencyFilesAreRenderedAndAddedToMatches)
{
  write("input.d", "output.o: \\\n ./source.cpp header.hpp\n");
  add("build", "depends:\n  - dependency_file: '" + directory.generic_string() + "/input.d'\n");
  const auto matches = database.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(strings(matches[0]->dependencies), (std::vector<std::string>{"source.cpp", "header.hpp"}));
}

TEST_F(BlueprintTest, BlueprintPersistencePreservesProcessRequirementsGroupAndMatching)
{
  add("build", "group: Build\nrequires: [compiler]\ndepends: [source.cpp]\nprocess: [{echo: hello}]\n");
  add("build", "process: [{echo: second}]\n");
  add(R"((.+)\.o)", "regex: '(.+)\\.o'\ndepends: ['{{ $(1) }}.cpp']\nprocess: [{echo: object}]\n");
  const auto path = directory / "blueprints.json";
  database.save(path);
  yakka::blueprint_database restored;
  restored.load(path);
  const auto matches = restored.find_match("build", summary.crootref());
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(strings(matches[0]->dependencies), (std::vector<std::string>{"source.cpp"}));
  EXPECT_EQ(strings(matches[0]->blueprint->requirements), (std::vector<std::string>{"compiler"}));
  EXPECT_EQ(matches[0]->blueprint->task_group, c4::to_csubstr("Build"));
  ASSERT_TRUE(matches[0]->blueprint->process.valid());
  EXPECT_EQ(matches[0]->blueprint->process[0]["echo"].val(), c4::to_csubstr("hello"));
  const auto regex_matches = restored.find_match("main.o", summary.crootref());
  ASSERT_EQ(regex_matches.size(), 1u);
  EXPECT_EQ(strings(regex_matches[0]->dependencies), (std::vector<std::string>{"main.cpp"}));
}

TEST_F(BlueprintTest, TargetCachePreservesMissesAndAvoidsDuplicateMatches)
{
  add("build", "depends: [source.cpp]\n");
  yakka::target_database targets;
  EXPECT_EQ(targets.add_target("build", database, summary.crootref()).size(), 1u);
  EXPECT_EQ(targets.add_target("build", database, summary.crootref()).size(), 1u);
  EXPECT_TRUE(targets.add_target("missing", database, summary.crootref()).empty());
  EXPECT_EQ(targets.targets.size(), 2u);
}

TEST_F(BlueprintTest, TargetPersistencePreservesExecutableBlueprintAndOptionalRegex)
{
  add("build", "group: Build\nrequires: [compiler]\ndepends: [source.cpp]\nprocess: [{echo: hello}]\n");
  yakka::target_database targets;
  targets.add_target("build", database, summary.crootref());
  targets.add_target("missing", database, summary.crootref());
  const auto path = directory / "targets.json";
  ASSERT_EQ(targets.save(path), yakka::yakka_status::SUCCESS);
  yakka::target_database restored;
  ASSERT_EQ(restored.load(path), yakka::yakka_status::SUCCESS);
  ASSERT_EQ(restored.get_target("build").size(), 1u);
  const auto &match = restored.get_target("build")[0];
  EXPECT_EQ(strings(match->dependencies), (std::vector<std::string>{"source.cpp"}));
  EXPECT_EQ(strings(match->regex_matches), (std::vector<std::string>{"build"}));
  ASSERT_NE(match->blueprint, nullptr);
  EXPECT_FALSE(match->blueprint->regex.has_value());
  EXPECT_EQ(strings(match->blueprint->requirements), (std::vector<std::string>{"compiler"}));
  EXPECT_EQ(match->blueprint->task_group, c4::to_csubstr("Build"));
  EXPECT_EQ(match->blueprint->process[0]["echo"].val(), c4::to_csubstr("hello"));
  EXPECT_TRUE(restored.get_target("missing").empty());
}

TEST_F(BlueprintTest, TargetLoadingReportsMissingAndInvalidDocumentShape)
{
  yakka::target_database targets;
  EXPECT_EQ(targets.load(directory / "missing.json"), yakka::yakka_status::FILE_NOT_FOUND);
  EXPECT_EQ(targets.load(write("invalid.json", R"({"targets":{}})")), yakka::yakka_status::LOAD_FAILED);
}

class BlueprintExecutionTest : public BlueprintTest {
protected:
  std::pair<std::string, int> run(const std::string &yaml, const std::string &tools = "{}")
  {
    yakka::workspace workspace;
    workspace.workspace_path = directory;
    yakka::project project(workspace, "blueprint_test");
    project.init_project();
    yakka::merge_nodes(project.project_summary["tools"], parse(tools));
    yakka::merge_nodes(project.project_summary["components"], summary.crootref()["components"]);
    yakka::merge_nodes(project.project_summary["data"], summary.crootref()["data"]);
    workspace.local_database.scan_for_components(directory);
    auto root = parse(yaml);
    auto match = std::make_shared<yakka::blueprint_match>();
    match->blueprint = std::make_shared<yakka::blueprint>("build", root, "component");
    match->regex_matches = {"build", "capture"};
    yakka::task_engine engine;
    ryml::Tree data;
    data.rootref() |= ryml::MAP;
    return engine.run_command("build", match, project, data.rootref());
  }
};

TEST_F(BlueprintExecutionTest, CommandsPipeCapturedOutputInOrder)
{
  const auto result = run("process:\n  - echo: first\n  - regex: {search: first, replace: second}\n  - echo:\n");
  EXPECT_EQ(result.second, 0);
  EXPECT_EQ(result.first, "second");
}

TEST_F(BlueprintExecutionTest, FailureStopsSubsequentCommands)
{
  const auto output = directory / "should_not_exist";
  const auto result = run("process:\n  - verify: '" + (directory / "missing").generic_string() +
                         "'\n  - echo: wrong\n  - save: '" + output.generic_string() + "'\n");
  EXPECT_NE(result.second, 0);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(BlueprintExecutionTest, TemplateErrorAbortsProcessAndPreventsSavingEmptyFile)
{
  const auto output = directory / "template_err_output.txt";
  const auto result = run("process:\n  - template: '{{ unclosed'\n  - save: '" + output.generic_string() + "'\n");
  EXPECT_NE(result.second, 0);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(BlueprintExecutionTest, TemplateFileMissingAbortsProcessAndPreventsSavingEmptyFile)
{
  const auto output = directory / "missing_tpl_output.txt";
  const auto result = run("process:\n  - template:\n      template_file: '" + (directory / "nonexistent.tpl").generic_string() + "'\n  - save: '" + output.generic_string() + "'\n");
  EXPECT_NE(result.second, 0);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(BlueprintExecutionTest, TemplateIncludeErrorAbortsProcessAndPreventsSavingEmptyFile)
{
  const auto tpl_path = write("broken_include.tpl", "Hello {% include \"missing_sub.tpl\" %}\n");
  const auto output   = directory / "include_err_output.txt";
  const auto result   = run("process:\n  - template:\n      template_file: '" + tpl_path.generic_string() + "'\n  - save: '" + output.generic_string() + "'\n");
  EXPECT_NE(result.second, 0);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(BlueprintExecutionTest, RejectsUnknownCommandsAndMalformedProcessEntries)
{
  for (const auto &yaml: {"process: [{unknown_command: value}]", "process: [scalar]",
                         "process: [{echo: one, save: two}]"})
    EXPECT_NE(run(yaml).second, 0) << yaml;
}

TEST_F(BlueprintExecutionTest, DependencyOnlyBlueprintSucceedsWithoutProcess)
{
  const auto result = run("depends: [source.cpp]");
  EXPECT_EQ(result.second, 0);
  EXPECT_TRUE(result.first.empty());
}

TEST_F(BlueprintExecutionTest, ScalarToolCommandsCaptureOutput)
{
  const auto result = run("process: [{printer: message}]", "printer: echo");
  EXPECT_EQ(result.second, 0);
  EXPECT_NE(result.first.find("message"), std::string::npos);
}

TEST_F(BlueprintExecutionTest, ToolSuccessCodesAcceptConfiguredNonzeroAndRejectOtherCodes)
{
  const std::string tools = "finish:\n  command: exit\n  tool_return_success: [7]\n";
  EXPECT_EQ(run("process: [{finish: '7'}]", tools).second, 0);
  EXPECT_NE(run("process: [{finish: '8'}]", tools).second, 0);
  EXPECT_NE(run("process: [{finish: '7'}]", "finish: exit").second, 0);
}

TEST_F(BlueprintExecutionTest, MalformedProcessContainerFailsRatherThanSilentlySkipping)
{
  EXPECT_NE(run("process: {echo: ignored}").second, 0);
}

TEST_F(BlueprintExecutionTest, CallbacksExposeCaptureCurrentDirectoryAndNestedRendering)
{
  const auto result = run(R"(
process:
  - echo: '{{ $(1) }}:{{ curdir() }}:{{ render("{{ curdir() }}", "other") }}:{{ curdir() }}'
)");
  EXPECT_EQ(result.second, 0);
  EXPECT_EQ(result.first, "capture:component:other:component");
}

TEST_F(BlueprintExecutionTest, AggregateCallbackUsesComponentAndProjectData)
{
  const auto result = run("process: [{inja: '{{ aggregate(\"/flags\").first }}:{{ aggregate(\"/flags\").second }}:{{ aggregate(\"/flags\").third }}'}]");
  EXPECT_EQ(result.second, 0);
  EXPECT_EQ(result.first, "one:two:three");
}

TEST_F(BlueprintExecutionTest, LoadComponentCallbackReturnsOwnedNestedData)
{
  write("example.yakka", "name: example\nnested: {values: [one, two]}\n");
  const auto result = run("process: [{inja: '{% set loaded = load_component(\"example\") %}{{ at(loaded.nested.values, 1) }}'}]");
  EXPECT_EQ(result.second, 0);
  EXPECT_EQ(result.first, "two");
}

TEST_F(BlueprintExecutionTest, IncludeCallbackParsesTemplateExpressions)
{
  const auto path = write("included.inja", "{{ curdir() }}:{{ $(1) }}");
  const auto result = run("process:\n  - inja: '{% include \"" + path.generic_string() + "\" %}'\n");
  EXPECT_EQ(result.second, 0);
  EXPECT_EQ(result.first, "component:capture");
}

TEST_F(BlueprintExecutionTest, InvalidCaptureIndexDoesNotReadPastMatches)
{
  const auto result = run("process: [{inja: '{{ $(99) }}'}]");
  EXPECT_TRUE(result.first.empty());
}

class BlueprintProjectTest : public BlueprintTest {
protected:
  void SetUp() override
  {
    BlueprintTest::SetUp();
    workspace.init(directory);
    project = std::make_unique<yakka::project>(workspace, "blueprint_project");
    project->init_project();
    project->generate_project_summary();
  }

  std::shared_ptr<yakka::component> component(const std::string &name, const std::string &body)
  {
    auto result = std::make_shared<yakka::component>();
    if (result->parse_file(write(name + ".yakka", "name: " + name + "\n" + body)) != yakka::yakka_status::SUCCESS)
      throw std::runtime_error("Failed to parse test component " + name);
    return result;
  }

  yakka::workspace workspace;
  std::unique_ptr<yakka::project> project;
};

TEST_F(BlueprintProjectTest, RegistersTemplatedRegexAndDataTargetsAndRejectsInvalidTargets)
{
  auto definitions = component("app", R"(
blueprints:
  '{{project_name}}.txt':
    process: [{echo: generated}]
  objects:
    regex: '{{ project_name }}/(.+)\.o'
    depends: ['{{ $(1) }}.cpp']
  ':/data/result':
    process: [{echo: value}]
  ':/components/app/result':
    process: [{echo: forbidden}]
  empty:
    regex: '{{ "" }}'
    process: [{echo: empty}]
)");
  project->process_blueprints(definitions);
  EXPECT_EQ(project->blueprint_database.blueprints.size(), 3u);
  EXPECT_EQ(project->blueprint_database.find_match("blueprint_project.txt", project->project_summary).size(), 1u);
  const auto objects = project->blueprint_database.find_match("blueprint_project/main.o", project->project_summary);
  ASSERT_EQ(objects.size(), 1u);
  EXPECT_EQ(strings(objects[0]->dependencies), (std::vector<std::string>{"main.cpp"}));
  EXPECT_EQ(project->blueprint_database.find_match(":/data/result", project->project_summary).size(), 1u);
}

TEST_F(BlueprintProjectTest, TraversesDependencyGraphWithSharedNodesAndCyclesWithoutDuplicatingTargets)
{
  auto definitions = component("app", R"(
blueprints:
  build: {depends: [first, second]}
  first: {depends: [shared]}
  second: {depends: [shared]}
  shared: {depends: [first, ':/data/value']}
)");
  project->process_blueprints(definitions);
  project->commands.insert("build");
  project->generate_target_database();
  EXPECT_EQ(project->target_database.targets.size(), 5u);
  EXPECT_EQ(project->target_database.get_target("shared").size(), 1u);
  EXPECT_TRUE(project->target_database.get_target(":/data/value").empty());
}

TEST_F(BlueprintProjectTest, ProcessesScalarMapAndConfiguredSuccessTools)
{
  auto definitions = component("compiler", R"(
tools:
  plain: echo
  mapped: {command: echo}
  scalar_success: {command: exit, return_value_success: 7}
  list_success: {command: exit, return_value_success: [0, 7]}
)");
  project->process_tools(definitions);
  const auto tools = project->project_summary["tools"];
  EXPECT_EQ(tools["plain"].val(), c4::to_csubstr("echo"));
  EXPECT_EQ(tools["mapped"].val(), c4::to_csubstr("echo"));
  EXPECT_EQ(tools["scalar_success"]["command"].val(), c4::to_csubstr("exit"));
  EXPECT_EQ(tools["scalar_success"]["tool_return_success"][0].val<int>().value(), 7);
  ASSERT_EQ(tools["list_success"]["tool_return_success"].num_children(), 2u);
  EXPECT_EQ(tools["list_success"]["tool_return_success"][1].val<int>().value(), 7);
}

TEST_F(BlueprintProjectTest, BlueprintRequirementsLoadAdditionalTools)
{
  component("compiler", "tools: {printer: echo}\n");
  auto definitions = component("app", "blueprints:\n  build:\n    requires: [compiler]\n    process: [{printer: hello}]\n");
  workspace.local_database.scan_for_components(directory);
  project->process_blueprints(definitions);
  project->commands.insert("build");
  project->generate_target_database();
  EXPECT_TRUE(project->additional_tools.contains("compiler"));
  ASSERT_TRUE(project->project_summary["tools"].has_child("printer"));
  EXPECT_EQ(project->project_summary["tools"]["printer"].val(), c4::to_csubstr("echo"));
}

class BlueprintSchemaTest : public BlueprintTest {};

TEST_F(BlueprintSchemaTest, AcceptsEverySupportedBlueprintFieldAndDependencyForm)
{
  yakka::schema schema(yakka::yakka_component_schema_yaml);
  EXPECT_TRUE(schema.validate(parse(R"(
name: example
blueprints:
  build:
    regex: '.+'
    group: Build
    requires: [compiler]
    depends:
      - input.cpp
      - data: [/components/*/flags, /data/value]
      - dependency_file: input.d
    process:
      - echo: hello
)"), "example"));
}

TEST_F(BlueprintSchemaTest, RejectsInvalidFieldTypesAndUnknownFields)
{
  for (const auto &body: {"unknown: value", "regex: []", "group: {}", "requires: compiler",
                         "depends: input.cpp", "process: echo", "process: [echo]"})
  {
    SCOPED_TRACE(body);
    yakka::schema schema(yakka::yakka_component_schema_yaml);
    EXPECT_FALSE(schema.validate(parse(std::string("name: example\nblueprints:\n  build:\n    ") + body + "\n"), "example"));
  }
}

} // namespace
