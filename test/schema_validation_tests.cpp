// =============================================================================
// Schema validation unit tests (yakka::schema / valijson + RapidYAML adapter)
//
// Run: ./unit_tests --gtest_filter="Schema*"
// =============================================================================

#include <gtest/gtest.h>
#include "yakka_schema.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <filesystem>
#include <fstream>
#include <deque>

namespace {

class SchemaValidationTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    if (!spdlog::get("console")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      spdlog::register_logger(std::make_shared<spdlog::logger>("console", null_sink));
    }
    if (!spdlog::get("yakkalog")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      spdlog::set_default_logger(std::make_shared<spdlog::logger>("yakkalog", null_sink));
    }
    spdlog::set_level(spdlog::level::off);
  }

  static bool check(yakka::schema &s, const std::string &yaml)
  {
    trees.push_back(ryml::parse_in_arena(c4::to_csubstr(yaml)));
    return s.validate(trees.back().crootref(), "test");
  }

  // Trees must outlive the validate() call; a deque keeps existing entries stable.
  static inline std::deque<ryml::Tree> trees;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, EmptySchemaAcceptsAnything)
{
  yakka::schema s{ std::string{} };
  EXPECT_TRUE(check(s, "anything: 1"));
}

TEST_F(SchemaValidationTest, ConstructFromFile)
{
  const auto path = std::filesystem::temp_directory_path() / "yakka_schema_test.yaml";
  {
    std::ofstream out(path);
    out << "type: object\nproperties:\n  name:\n    type: string\nrequired:\n  - name\n";
  }

  yakka::schema s{ path };
  EXPECT_TRUE(check(s, "name: hello"));
  EXPECT_FALSE(check(s, "other: hello"));

  std::filesystem::remove(path);
}

TEST_F(SchemaValidationTest, NonMapSchemaRootAcceptsAnything)
{
  // Note: a *malformed* schema string cannot be tested here - ryml's default
  // error callback aborts the process rather than throwing.
  yakka::schema s{ std::string("- not\n- a\n- schema\n") };
  EXPECT_TRUE(check(s, "name: hello"));
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, StringType)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  value:\n    type: string\n") };
  EXPECT_TRUE(check(s, "value: hello"));
  EXPECT_TRUE(check(s, "value: \"123\""));
  EXPECT_FALSE(check(s, "value: 123"));
  EXPECT_FALSE(check(s, "value: [1, 2]"));
}

TEST_F(SchemaValidationTest, IntegerType)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  value:\n    type: integer\n") };
  EXPECT_TRUE(check(s, "value: 123"));
  EXPECT_TRUE(check(s, "value: -7"));
  EXPECT_FALSE(check(s, "value: 1.5"));
  EXPECT_FALSE(check(s, "value: \"123\""));
  EXPECT_FALSE(check(s, "value: hello"));
}

TEST_F(SchemaValidationTest, NumberType)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  value:\n    type: number\n") };
  EXPECT_TRUE(check(s, "value: 1.5"));
  EXPECT_TRUE(check(s, "value: 123"));
  EXPECT_FALSE(check(s, "value: hello"));
}

TEST_F(SchemaValidationTest, BooleanType)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  value:\n    type: boolean\n") };
  EXPECT_TRUE(check(s, "value: true"));
  EXPECT_TRUE(check(s, "value: false"));
  EXPECT_FALSE(check(s, "value: \"true\""));
  EXPECT_FALSE(check(s, "value: 1"));
}

TEST_F(SchemaValidationTest, ArrayAndObjectTypes)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  list:\n    type: array\n  map:\n    type: object\n") };
  EXPECT_TRUE(check(s, "list: [1, 2]\nmap: {a: 1}"));
  EXPECT_FALSE(check(s, "list: {a: 1}"));
  EXPECT_FALSE(check(s, "map: [1, 2]"));
}

TEST_F(SchemaValidationTest, NullableType)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  value:\n    type: [array, 'null']\n") };
  EXPECT_TRUE(check(s, "value: [1, 2]"));
  EXPECT_TRUE(check(s, "value:"));
  EXPECT_FALSE(check(s, "value: hello"));
}

// ---------------------------------------------------------------------------
// Object constraints
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, RequiredProperties)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  name:\n    type: string\nrequired:\n  - name\n") };
  EXPECT_TRUE(check(s, "name: hello"));
  EXPECT_FALSE(check(s, "other: hello"));
}

TEST_F(SchemaValidationTest, AdditionalPropertiesFalse)
{
  yakka::schema s{ std::string("type: object\nadditionalProperties: false\nproperties:\n  name:\n    type: string\n") };
  EXPECT_TRUE(check(s, "name: hello"));
  EXPECT_FALSE(check(s, "name: hello\nextra: 1"));
}

TEST_F(SchemaValidationTest, PatternProperties)
{
  yakka::schema s{ std::string("type: object\npatternProperties:\n  '^x_':\n    type: integer\n") };
  EXPECT_TRUE(check(s, "x_a: 1"));
  EXPECT_FALSE(check(s, "x_a: hello"));
  EXPECT_TRUE(check(s, "y_a: hello"));
}

TEST_F(SchemaValidationTest, PropertyNamesPattern)
{
  yakka::schema s{ std::string("type: object\npropertyNames:\n  pattern: '^[a-z]+$'\n") };
  EXPECT_TRUE(check(s, "abc: 1"));
  EXPECT_FALSE(check(s, "ABC: 1"));
}

TEST_F(SchemaValidationTest, MinProperties)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  inner:\n    type: object\n    minProperties: 1\n") };
  EXPECT_TRUE(check(s, "inner: {a: 1}"));
  EXPECT_FALSE(check(s, "inner: {}"));
}

// ---------------------------------------------------------------------------
// Array constraints
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, ItemsAndUniqueItems)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  list:\n    type: array\n    uniqueItems: true\n    items:\n      type: string\n") };
  EXPECT_TRUE(check(s, "list: [a, b]"));
  EXPECT_FALSE(check(s, "list: [a, a]"));
  EXPECT_FALSE(check(s, "list: [1, 2]"));
}

// ---------------------------------------------------------------------------
// Combinators
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, OneOf)
{
  yakka::schema s{ std::string(R"(
type: object
properties:
  value:
    oneOf:
      - type: object
        required: [feature]
        properties:
          feature:
            type: string
      - type: object
        required: [component]
        properties:
          component:
            type: string
)") };
  EXPECT_TRUE(check(s, "value: {feature: a}"));
  EXPECT_TRUE(check(s, "value: {component: a}"));
  EXPECT_FALSE(check(s, "value: {other: a}"));
  EXPECT_FALSE(check(s, "value: {feature: a, component: b}"));
}

TEST_F(SchemaValidationTest, EnumAndConst)
{
  yakka::schema s{ std::string("type: object\nproperties:\n  colour:\n    enum: [red, green]\n  fixed:\n    const: only\n") };
  EXPECT_TRUE(check(s, "colour: red\nfixed: only"));
  EXPECT_FALSE(check(s, "colour: blue"));
  EXPECT_FALSE(check(s, "fixed: other"));
}

// ---------------------------------------------------------------------------
// add_schema_data
// ---------------------------------------------------------------------------

TEST_F(SchemaValidationTest, AddSchemaDataMergesFragments)
{
  yakka::schema s;

  ryml::Tree first  = ryml::parse_in_arena(ryml::to_csubstr("alpha:\n  type: string\n"));
  ryml::Tree second = ryml::parse_in_arena(ryml::to_csubstr("beta:\n  type: integer\n"));

  s.add_schema_data(first.crootref());
  s.add_schema_data(second.crootref());

  EXPECT_TRUE(check(s, "alpha: hello\nbeta: 1"));
  EXPECT_FALSE(check(s, "alpha: 1"));
  EXPECT_FALSE(check(s, "beta: hello"));
}

TEST_F(SchemaValidationTest, AddSchemaDataInvalidatesCachedSchema)
{
  yakka::schema s;

  ryml::Tree first = ryml::parse_in_arena(ryml::to_csubstr("alpha:\n  type: string\n"));
  s.add_schema_data(first.crootref());
  EXPECT_TRUE(check(s, "alpha: hello"));

  ryml::Tree second = ryml::parse_in_arena(ryml::to_csubstr("beta:\n  type: integer\n"));
  s.add_schema_data(second.crootref());
  EXPECT_FALSE(check(s, "alpha: hello\nbeta: nope"));
}

// ---------------------------------------------------------------------------
// The built-in Yakka component schema
// ---------------------------------------------------------------------------

class YakkaComponentSchemaTest : public SchemaValidationTest {
protected:
  yakka::schema component_schema{ yakka::yakka_component_schema_yaml };
};

TEST_F(YakkaComponentSchemaTest, AcceptsValidComponent)
{
  EXPECT_TRUE(check(component_schema, R"(
name: Example
requires:
  features:
    - cpp20
  components:
    - rapidyaml
blueprints:
  '{{project_output}}/app.elf':
    depends:
      - a
    process:
      - shell: echo hi
)"));
}

TEST_F(YakkaComponentSchemaTest, RejectsMissingName)
{
  EXPECT_FALSE(check(component_schema, "requires:\n  features:\n    - cpp20\n"));
}

TEST_F(YakkaComponentSchemaTest, RejectsUnknownBlueprintProperty)
{
  EXPECT_FALSE(check(component_schema, "name: Example\nblueprints:\n  target:\n    bogus: 1\n"));
}

TEST_F(YakkaComponentSchemaTest, RejectsBlueprintNameViolatingPropertyNames)
{
  EXPECT_FALSE(check(component_schema, "name: Example\nblueprints:\n  '!bad':\n    regex: x\n"));
}

TEST_F(YakkaComponentSchemaTest, RejectsChoiceWithoutDescription)
{
  EXPECT_FALSE(check(component_schema, "name: Example\nchoices:\n  my_choice:\n    exclusive: true\n"));
}

TEST_F(YakkaComponentSchemaTest, AcceptsEveryComponentInWorkspace)
{
  // Walk up from the working directory to find the Yakka workspace root.
  std::filesystem::path components_dir;
  for (auto dir = std::filesystem::current_path(); !dir.empty(); dir = dir.parent_path()) {
    if (std::filesystem::exists(dir / "yakka-components.json") && std::filesystem::is_directory(dir / "components")) {
      components_dir = dir / "components";
      break;
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) {
      break;
    }
  }

  if (components_dir.empty()) {
    GTEST_SKIP() << "workspace components directory not found";
  }

  for (const auto &entry: std::filesystem::recursive_directory_iterator(components_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".yakka") {
      continue;
    }

    std::ifstream in(entry.path());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.empty()) {
      continue;
    }

    EXPECT_TRUE(check(component_schema, content)) << "failed: " << entry.path().generic_string();
  }
}

// ---------------------------------------------------------------------------
// Merge strategies
// ---------------------------------------------------------------------------

class MergeStrategyTest : public SchemaValidationTest {
protected:
  // Builds a schema from a `<property>: {...}` fragment, exactly as project data schemas are assembled.
  yakka::schema make_schema(const std::string &fragment)
  {
    trees.push_back(ryml::parse_in_arena(c4::to_csubstr(fragment)));
    yakka::schema s;
    s.add_schema_data(trees.back().crootref());
    return s;
  }

  static std::string merge(yakka::schema &s, const std::string &target_yaml, const std::string &source_yaml)
  {
    trees.push_back(ryml::parse_in_arena(c4::to_csubstr(target_yaml)));
    ryml::Tree &target = trees.back();
    trees.push_back(ryml::parse_in_arena(c4::to_csubstr(source_yaml)));
    ryml::Tree &source = trees.back();

    yakka::json_node_merge(ryml::Pointer{}, target.rootref(), source.crootref(), &s);
    return ryml::emitrs_yaml<std::string>(target);
  }

  static std::vector<std::string> values_of(const std::string &yaml, const char *key)
  {
    ryml::Tree tree = ryml::parse_in_arena(c4::to_csubstr(yaml));
    std::vector<std::string> out;
    for (const auto &child: tree.crootref().find_child(c4::to_csubstr(key)).children()) {
      out.push_back(std::string(child.val().str, child.val().len));
    }
    return out;
  }
};

TEST_F(MergeStrategyTest, DefaultConcatenatesSequences)
{
  auto s = make_schema("list:\n  type: array\n");
  EXPECT_EQ(values_of(merge(s, "list: [a, b]", "list: [b, c]"), "list"), (std::vector<std::string>{ "a", "b", "b", "c" }));
}

TEST_F(MergeStrategyTest, Concatenate)
{
  auto s = make_schema("list:\n  type: array\n  merge: concatenate\n");
  EXPECT_EQ(values_of(merge(s, "list: [a, b]", "list: [b, c]"), "list"), (std::vector<std::string>{ "a", "b", "b", "c" }));
}

TEST_F(MergeStrategyTest, Unique)
{
  auto s = make_schema("list:\n  type: array\n  merge: unique\n");
  EXPECT_EQ(values_of(merge(s, "list: [a, b]", "list: [b, c]"), "list"), (std::vector<std::string>{ "a", "b", "c" }));
}

TEST_F(MergeStrategyTest, Sort)
{
  auto s = make_schema("list:\n  type: array\n  merge: sort\n");
  EXPECT_EQ(values_of(merge(s, "list: [c, a]", "list: [b]"), "list"), (std::vector<std::string>{ "a", "b", "c" }));
}

TEST_F(MergeStrategyTest, AppendPromotesScalarToSequence)
{
  auto s = make_schema("list:\n  merge: concatenate\n");
  EXPECT_EQ(values_of(merge(s, "list: a", "list: b"), "list"), (std::vector<std::string>{ "a", "b" }));
}

TEST_F(MergeStrategyTest, Overwrite)
{
  auto s = make_schema("list:\n  type: array\n  merge: overwrite\n");
  EXPECT_EQ(values_of(merge(s, "list: [a, b]", "list: [c]"), "list"), (std::vector<std::string>{ "c" }));
}

TEST_F(MergeStrategyTest, Max)
{
  auto s = make_schema("value:\n  type: integer\n  merge: max\n");
  EXPECT_NE(merge(s, "value: 5", "value: 9").find("value: 9"), std::string::npos);
  EXPECT_NE(merge(s, "value: 9", "value: 5").find("value: 9"), std::string::npos);
}

TEST_F(MergeStrategyTest, Min)
{
  auto s = make_schema("value:\n  type: integer\n  merge: min\n");
  EXPECT_NE(merge(s, "value: 5", "value: 9").find("value: 5"), std::string::npos);
  EXPECT_NE(merge(s, "value: 9", "value: 5").find("value: 5"), std::string::npos);
}

TEST_F(MergeStrategyTest, AbortLeavesTargetUnchanged)
{
  auto s = make_schema("value:\n  merge: abort\n");
  EXPECT_NE(merge(s, "value: 5", "value: 9").find("value: 5"), std::string::npos);
}

TEST_F(MergeStrategyTest, StrategyAppliesToNestedPaths)
{
  auto s = make_schema("outer:\n  type: object\n  properties:\n    list:\n      type: array\n      merge: unique\n");
  const auto result = merge(s, "outer:\n  list: [a, b]\n", "outer:\n  list: [b, c]\n");
  ryml::Tree tree   = ryml::parse_in_arena(c4::to_csubstr(result));
  std::vector<std::string> out;
  for (const auto &child: tree.crootref()["outer"]["list"].children()) {
    out.push_back(std::string(child.val().str, child.val().len));
  }
  EXPECT_EQ(out, (std::vector<std::string>{ "a", "b", "c" }));
}

} // namespace
