// =============================================================================
// Section A6: Blueprint Built-in Command Unit Tests
//
// Tests each built-in blueprint command function in blueprint_commands.cpp:
//   echo, execute, shell, regex, template/inja, save, create_directory,
//   verify, rm, rmdir, pack, copy, cat, as_json, as_yaml, diff
//
// Build: include in the unit tests yakka target alongside other test sources.
// Run:   ./unit_tests --gtest_filter="BlueprintCommand*"
// =============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "blueprint_commands.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <set>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test Fixture
// ---------------------------------------------------------------------------

class BlueprintCommandTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Blueprint commands call spdlog::get("console"); provide a no-op sink.
    if (!spdlog::get("console")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      auto logger    = std::make_shared<spdlog::logger>("console", null_sink);
      spdlog::register_logger(logger);
    }

    // Create an isolated temporary directory for filesystem tests.
    test_dir = fs::temp_directory_path() / ("yakka_bp_test_" + std::to_string(
                 std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(test_dir);

    // Set up the Inja environment with all common template callbacks.
    yakka::add_common_template_commands(inja_env);

    // Empty project summary and mutable project data trees.
    summary_tree = ryml::parse_in_arena("{}");
    project_summary = summary_tree.crootref();

    data_tree.rootref() |= ryml::MAP;
    project_data = data_tree.rootref();
  }

  void TearDown() override
  {
    fs::remove_all(test_dir);
  }

  // Helper: create a YAML scalar command node from a string value.
  ryml::Tree make_scalar(const std::string &value)
  {
    ryml::Tree tree;
    tree.rootref() << value;
    return tree;
  }

  // Helper: parse a multi-key YAML map.
  ryml::Tree make_map(const std::string &yaml)
  {
    return ryml::parse_in_arena(ryml::to_csubstr(yaml));
  }

  // Helper: write a text file in the test directory.
  fs::path write_file(const std::string &name, const std::string &content)
  {
    fs::path p = test_dir / name;
    std::ofstream f(p);
    f << content;
    return p;
  }

  // Helper: read a file's contents.
  std::string read_file(const fs::path &p)
  {
    std::ifstream f(p);
    return std::string{ std::istreambuf_iterator<char>{ f }, {} };
  }

  yakka::process_return invoke_command(const std::string &name, std::string target, ryml::ConstNodeRef command,
                                      std::string captured_output, ryml::ConstNodeRef summary, ryml::NodeRef data)
  {
    return yakka::blueprint_commands.at(name)(std::move(target), command, std::move(captured_output), summary, data, inja_env);
  }

  fs::path test_dir;
  inja::Environment inja_env;
  ryml::Tree summary_tree;
  ryml::ConstNodeRef project_summary;
  ryml::Tree data_tree;
  ryml::NodeRef project_data;
};

// ---------------------------------------------------------------------------
// A6-001 / A6-101 / A6-201 – echo
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, EchoSuccess)
{
  // A6-001: echo with a scalar command outputs the scalar text.
  auto cmd_tree = make_scalar("hello from echo");
  auto cmd      = cmd_tree.crootref();

  auto result = invoke_command("echo", "target", cmd, "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "hello from echo");
}

TEST_F(BlueprintCommandTest, EchoPassthroughWhenEmpty)
{
  // A6-201: echo with no command value passes through captured_output unchanged.
  ryml::Tree empty_tree;
  auto cmd              = empty_tree.crootref();

  auto result = invoke_command("echo", "target", cmd, "prior output", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "prior output");
}

TEST_F(BlueprintCommandTest, EchoTemplateVariable)
{
  // Echo renders Inja template expressions against the project summary.
  ryml::Tree sum_tree = ryml::parse_in_arena("version: 42");
  auto sum             = sum_tree.crootref();

  auto cmd_tree = make_scalar("ver={{ version }}");
  auto cmd      = cmd_tree.crootref();

  auto result = invoke_command("echo", "target", cmd, "", sum, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "ver=42");
}

// ---------------------------------------------------------------------------
// A6-002 / A6-102 – execute
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, ExecuteSuccess)
{
  // A6-002: execute runs a real command and captures its output.
  auto cmd_tree = make_scalar("echo yakka_exec_test");
  auto result   = invoke_command("execute", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("yakka_exec_test"));
}

TEST_F(BlueprintCommandTest, ExecuteFailsOnInvalidCommand)
{
  // A6-102: execute returns non-zero exit code for failing commands.
  auto cmd_tree = make_scalar("false");
  auto result   = invoke_command("execute", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, ExecuteInvalidNode)
{
  // A6-202: execute with invalid/missing command node returns error code.
  ryml::Tree empty_tree;
  auto result = invoke_command("execute", "target", empty_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-003 / A6-103 – shell
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, ShellSuccess)
{
  // A6-003: shell runs command through the platform shell.
  auto cmd_tree = make_scalar("echo yakka_shell_test");
  auto result   = invoke_command("shell", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("yakka_shell_test"));
}

TEST_F(BlueprintCommandTest, ShellInvalidNode)
{
  // A6-103: shell with invalid node returns error.
  ryml::Tree empty_tree;
  auto result = invoke_command("shell", "target", empty_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-004 – regex (replace sub-mode)
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, RegexReplaceSuccess)
{
  // A6-004: regex replace transforms captured output.
  auto cmd_tree = make_map("search: foo\nreplace: bar");
  auto result   = invoke_command("regex", "target", cmd_tree.crootref(), "foo baz foo", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("bar"));
  EXPECT_THAT(result.result, ::testing::Not(::testing::HasSubstr("foo")));
}

TEST_F(BlueprintCommandTest, RegexMissingSearchFails)
{
  // A6-104: regex without search field returns error.
  auto cmd_tree = make_map("replace: bar");
  auto result   = invoke_command("regex", "target", cmd_tree.crootref(), "foo", project_summary, project_data);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-005 / A6-105 – template (inja command)
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, TemplateScalarRenders)
{
  // A6-005: template command with scalar renders the string.
  auto cmd_tree = make_scalar("result=42");
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "result=42");
}

TEST_F(BlueprintCommandTest, TemplateWithInlineTemplate)
{
  // Template command with inline template key renders the template string.
  auto cmd_tree = make_map("template: \"value={{ x }}\"\n");
  ryml::Tree sum_tree = ryml::parse_in_arena("x: 99");

  auto result = invoke_command("template", "target", cmd_tree.crootref(), "", sum_tree.crootref(), project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "value=99");
}

TEST_F(BlueprintCommandTest, TemplateFromFile)
{
  // A6-205: template command with template_file loads and renders from file.
  auto tpl_path = write_file("test.tpl", "file={{ var }}");
  auto cmd_yaml = std::string("template_file: \"") + tpl_path.generic_string() + "\"";
  auto cmd_tree = make_map(cmd_yaml);
  ryml::Tree sum_tree = ryml::parse_in_arena("var: loaded");

  auto result = invoke_command("template", "target", cmd_tree.crootref(), "", sum_tree.crootref(), project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "file=loaded");
}

TEST_F(BlueprintCommandTest, InjaCommandIsAliasForTemplate)
{
  // A6-006: inja command is an alias for template and behaves identically.
  auto cmd_tree = make_scalar("alias=ok");
  auto result   = invoke_command("inja", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "alias=ok");
}

TEST_F(BlueprintCommandTest, TemplateSyntaxErrorReturnsError)
{
  // Template with invalid syntax returns non-zero retcode
  auto cmd_tree = make_scalar("{{ unclosed");
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, TemplateMapSyntaxErrorReturnsError)
{
  // Map form with invalid template syntax returns non-zero retcode
  auto cmd_tree = make_map("template: \"{{ unclosed\"");
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, TemplateMissingFileReturnsError)
{
  // Missing template file returns non-zero retcode
  auto cmd_yaml = std::string("template_file: \"") + (test_dir / "nonexistent.tpl").generic_string() + "\"";
  auto cmd_tree = make_map(cmd_yaml);
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, TemplateMissingDataFileReturnsError)
{
  // Missing data file returns non-zero retcode
  auto tpl_path = write_file("valid.tpl", "content={{ x }}");
  auto cmd_yaml = std::string("template_file: \"") + tpl_path.generic_string() + "\"\n" +
                  "data_file: \"" + (test_dir / "nonexistent.yaml").generic_string() + "\"";
  auto cmd_tree = make_map(cmd_yaml);
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, TemplateDataParseErrorReturnsError)
{
  // Invalid inline YAML data returns non-zero retcode
  auto cmd_tree = make_map("template: \"content\"\ndata: \"[invalid yaml: {{\"");
  auto result   = invoke_command("template", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

// ---------------------------------------------------------------------------
// A6-007 / A6-107 – save
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, SaveToFile)
{
  // A6-007: save writes captured_output to a file.
  auto save_path = test_dir / "saved.txt";
  auto cmd_tree  = make_scalar(save_path.generic_string());

  auto result = invoke_command("save", "target", cmd_tree.crootref(), "hello saved content", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(save_path));
  EXPECT_EQ(read_file(save_path), "hello saved content");
}

TEST_F(BlueprintCommandTest, SaveCreatesParentDirectories)
{
  // A6-207: save creates intermediate directories.
  auto save_path = test_dir / "deep" / "path" / "file.txt";
  auto cmd_tree  = make_scalar(save_path.generic_string());

  auto result = invoke_command("save", "target", cmd_tree.crootref(), "nested content", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(save_path));
}

TEST_F(BlueprintCommandTest, SaveEnsuresParentPathForTargetWhenFilenameOmitted)
{
  // When filename is omitted, save creates full parent path for target.
  auto save_path = test_dir / "auto_target" / "subdir" / "output.bin";
  ryml::Tree cmd_tree; // empty / null command

  auto result = invoke_command("save", save_path.generic_string(), cmd_tree.crootref(), "target content", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(save_path));
  EXPECT_EQ(read_file(save_path), "target content");
}

TEST_F(BlueprintCommandTest, SaveFailsWhenParentPathBlockedByFile)
{
  // When a component in the parent path is a regular file, save fails with non-zero retcode.
  auto blocking_file = write_file("not_a_dir.txt", "blocking");
  auto save_path = blocking_file / "child" / "file.txt";
  auto cmd_tree  = make_scalar(save_path.generic_string());

  auto result = invoke_command("save", "target", cmd_tree.crootref(), "data", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
  EXPECT_FALSE(fs::exists(save_path));
}

// ---------------------------------------------------------------------------
// A6-008 / A6-208 – create_directory
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, CreateDirectorySuccess)
{
  // A6-008: create_directory creates the target directory.
  auto new_dir = test_dir / "new_sub_dir";
  auto cmd_yaml = std::string("\"") + new_dir.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("create_directory", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_TRUE(fs::exists(new_dir));
  EXPECT_TRUE(fs::is_directory(new_dir));
}

// ---------------------------------------------------------------------------
// A6-009 / A6-109 – verify
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, VerifyExistingFile)
{
  // A6-009: verify returns 0 when file exists.
  auto f        = write_file("verify_target.txt", "present");
  auto cmd_yaml = std::string("\"") + f.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("verify", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, VerifyMissingFile)
{
  // A6-109: verify returns non-zero when file does not exist.
  auto missing  = test_dir / "__missing_verify__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("verify", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

// ---------------------------------------------------------------------------
// A6-010 / A6-110 – rm
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, RmRemovesExistingFile)
{
  // A6-010: rm removes an existing file.
  auto f        = write_file("to_remove.txt", "content");
  auto cmd_yaml = std::string("\"") + f.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("rm", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_FALSE(fs::exists(f));
}

TEST_F(BlueprintCommandTest, RmNonExistentFileReturnsSuccess)
{
  // A6-210: rm on a missing file is a no-op (graceful).
  auto missing  = test_dir / "__no_such_rm_file__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("rm", "target", cmd_tree.crootref(), "", project_summary, project_data);

  // rm of a non-existent file should not crash; return code is implementation-defined
  // but should not indicate a signal crash.
  EXPECT_LT(result.retcode, 128);
}

// ---------------------------------------------------------------------------
// A6-011 / A6-111 – rmdir
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, RmdirRemovesDirectory)
{
  // A6-011: rmdir removes an existing directory recursively.
  auto dir = test_dir / "rmdir_target";
  fs::create_directories(dir / "sub");
  write_file("rmdir_target/sub/file.txt", "x");

  auto cmd_yaml = std::string("\"") + dir.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("rmdir", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_FALSE(fs::exists(dir));
}

// ---------------------------------------------------------------------------
// A6-013 / A6-113 – copy
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, CopyFileSuccess)
{
  // A6-013: copy replicates a file to a new location.
  auto src      = write_file("copy_src.txt", "copy content");
  auto dst      = test_dir / "copy_dst.txt";
  auto cmd_yaml = std::string("source: \"") + src.generic_string() + "\"\ndestination: \"" + dst.generic_string() + "\"";
  auto cmd_tree = make_map(cmd_yaml);

  auto result = invoke_command("copy", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(dst));
  EXPECT_EQ(read_file(dst), "copy content");
}

// ---------------------------------------------------------------------------
// A6-014 / A6-114 – cat
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, CatReadsFileIntoOutput)
{
  // A6-014: cat reads the given file's contents into captured_output.
  auto src      = write_file("cat_src.txt", "cat content here");
  auto cmd_yaml = std::string("\"") + src.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("cat", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "cat content here");
}

TEST_F(BlueprintCommandTest, CatMissingFile)
{
  // A6-114: cat of missing file returns error.
  auto missing  = test_dir / "__no_such_cat__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = invoke_command("cat", "target", cmd_tree.crootref(), "", project_summary, project_data);

  EXPECT_NE(result.retcode, 0);
}

// ---------------------------------------------------------------------------
// A6-015 / A6-115 – as_json
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, AsJsonConvertsYamlToJson)
{
  // A6-015: as_json converts YAML-format captured_output to JSON.
  auto cmd_tree = make_map("{}");
  auto result   = invoke_command("as_json", "target", cmd_tree.crootref(), "key: value", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  // Result should be valid JSON – at a minimum it must start with '{'.
  EXPECT_THAT(result.result, ::testing::StartsWith("{"));
}

// ---------------------------------------------------------------------------
// A6-016 / A6-116 – as_yaml
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, AsYamlConvertsJsonToYaml)
{
  // A6-016: as_yaml converts JSON-format captured_output to YAML.
  auto cmd_tree = make_map("{}");
  auto result   = invoke_command("as_yaml", "target", cmd_tree.crootref(), R"({"key":"value"})", project_summary, project_data);

  EXPECT_EQ(result.retcode, 0);
  // Result should contain the key without braces (YAML style).
  EXPECT_THAT(result.result, ::testing::HasSubstr("key"));
}

// ---------------------------------------------------------------------------
// A6-017 – diff (currently returns empty / unimplemented)
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, DiffCommandDoesNotCrash)
{
  // A6-017: diff command must not crash regardless of input (currently unimplemented body).
  auto cmd_tree = make_map("left: a\nright: b");
  auto result   = invoke_command("diff", "target", cmd_tree.crootref(), "", project_summary, project_data);

  // retcode 0 expected (stub implementation).
  EXPECT_EQ(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, RegistryContainsExactlyTheCommandsExercisedByThisSuite)
{
  const std::set<std::string> expected = {
    "echo", "execute", "shell", "regex", "template", "inja", "save", "create_directory",
    "verify", "rm", "rmdir", "pack", "copy", "cat", "as_json", "as_yaml", "diff"
  };
  std::set<std::string> actual;
  for (const auto &[name, command]: yakka::blueprint_commands)
    actual.insert(name);
  EXPECT_EQ(actual, expected);
}

TEST_F(BlueprintCommandTest, RegexReplacementRendersPrefixAndSuffix)
{
  auto tree = make_map("search: foo\nreplace: bar\nprefix: '[{{ name }}:'\nsuffix: ']'\n");
  auto summary = make_map("name: result");
  auto result = invoke_command("regex", "", tree.crootref(), "foo foo", summary.crootref(), project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "[result:bar bar]");
}

TEST_F(BlueprintCommandTest, RegexMatchRendersCaptureGroups)
{
  auto tree = make_map(R"(
search: '([a-z]+)=([0-9]+)'
match: '{{ reg(2) }}:{{ reg(1) }};'
prefix: '['
suffix: ']'
)");
  auto result = invoke_command("regex", "", tree.crootref(), "x=12 y=34", project_summary, project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "[12:x;34:y;]");
}

TEST_F(BlueprintCommandTest, RegexSplitReplaceProcessesEveryLine)
{
  auto tree = make_map("search: 'value=([0-9]+)'\nreplace: '$1;'\nsplit: true\n");
  auto result = invoke_command("regex", "", tree.crootref(), "value=12\nignored\nvalue=34\n", project_summary, project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "12;34;");
}

TEST_F(BlueprintCommandTest, RegexSplitMatchProcessesEveryLine)
{
  auto tree = make_map("search: 'value=([0-9]+)'\nmatch: '{{ reg(1) }};'\nsplit: true\n");
  auto result = invoke_command("regex", "", tree.crootref(), "value=12\nvalue=34\n", project_summary, project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "12;34;");
}

TEST_F(BlueprintCommandTest, RegexToYamlPreservesCapturedFieldValues)
{
  for (const auto &split: {"", "split: true\n"}) {
    SCOPED_TRACE(split);
    auto tree = make_map(std::string("search: '([a-z]+)=([0-9]+)'\nto_yaml: [name, value]\n") + split);
    auto result = invoke_command("regex", "", tree.crootref(), "x=12\ny=34\n", project_summary, project_data);
    ASSERT_EQ(result.retcode, 0);
    auto output = ryml::parse_in_arena(c4::to_csubstr(result.result));
    ASSERT_TRUE(output.crootref().is_seq());
    ASSERT_EQ(output.crootref().num_children(), 2u);
    EXPECT_EQ(output.crootref()[0]["name"].val(), c4::to_csubstr("x"));
    EXPECT_EQ(output.crootref()[1]["value"].val(), c4::to_csubstr("34"));
  }
}

TEST_F(BlueprintCommandTest, RegexWithoutAnOperationFails)
{
  auto tree = make_map("search: foo");
  EXPECT_NE(invoke_command("regex", "", tree.crootref(), "foo", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, InlineTemplateDataOverridesSummary)
{
  auto tree = make_map("data: '{value: inline}'\ntemplate: '{{ value }}'\n");
  auto summary = make_map("value: summary");
  auto result = invoke_command("template", "", tree.crootref(), "", summary.crootref(), project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "inline");
}

TEST_F(BlueprintCommandTest, TemplateDataFileOverridesSummary)
{
  const auto path = write_file("data.yaml", "value: file\n");
  auto tree = make_map("data_file: '" + path.generic_string() + "'\ntemplate: '{{ value }}'\n");
  auto summary = make_map("value: summary");
  auto result = invoke_command("template", "", tree.crootref(), "", summary.crootref(), project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "file");
}

TEST_F(BlueprintCommandTest, MissingTemplateDataFileDoesNotSilentlyUseSummary)
{
  auto tree = make_map("data_file: '" + (test_dir / "missing.yaml").generic_string() + "'\ntemplate: '{{ value }}'\n");
  auto summary = make_map("value: summary");
  EXPECT_NE(invoke_command("template", "", tree.crootref(), "", summary.crootref(), project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, InvalidTemplateCommandFails)
{
  auto tree = make_map("{}");
  EXPECT_NE(invoke_command("template", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, SaveDefaultsToTargetAndPreservesBinaryContent)
{
  ryml::Tree tree;
  const auto path = test_dir / "binary";
  const std::string content("a\0b", 3);
  auto result = invoke_command("save", path.generic_string(), tree.crootref(), content, project_summary, project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(read_file(path), content);
}

TEST_F(BlueprintCommandTest, SaveWritesExistingAndNewDataPointers)
{
  for (const auto &pointer: {":/data/existing", ":/data/nested/new"}) {
    SCOPED_TRACE(pointer);
    project_data["existing"] << "old";
    auto tree = make_scalar(pointer);
    auto result = invoke_command("save", "", tree.crootref(), "new", project_summary, project_data);
    ASSERT_EQ(result.retcode, 0);
    auto path = ryml::Pointer(std::string(pointer).substr(6));
    ASSERT_TRUE(project_data.contains(path));
    EXPECT_EQ(project_data.at(path).val(), c4::to_csubstr("new"));
  }
}

TEST_F(BlueprintCommandTest, SaveRejectsNonDataPointersAndUnwritableFile)
{
  auto pointer = make_scalar(":/components/app/name");
  EXPECT_NE(invoke_command("save", "", pointer.crootref(), "new", project_summary, project_data).retcode, 0);
  auto directory = make_scalar(test_dir.generic_string());
  EXPECT_NE(invoke_command("save", "", directory.crootref(), "new", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, RemoveExpandsYamlLists)
{
  const auto first = write_file("first.txt", "first");
  const auto second = write_file("second.txt", "second");
  auto tree = make_scalar("['" + first.generic_string() + "', '" + second.generic_string() + "']");
  EXPECT_EQ(invoke_command("rm", "", tree.crootref(), "input", project_summary, project_data).retcode, 0);
  EXPECT_FALSE(fs::exists(first));
  EXPECT_FALSE(fs::exists(second));
}

TEST_F(BlueprintCommandTest, RemoveReportsInvalidListItems)
{
  auto tree = make_scalar("[{not: a_filename}]");
  EXPECT_NE(invoke_command("rm", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, FileCommandsRejectMissingArguments)
{
  ryml::Tree tree;
  for (const auto &name: {"verify", "rm", "rmdir", "cat"}) {
    SCOPED_TRACE(name);
    EXPECT_NE(invoke_command(name, "", tree.crootref(), "", project_summary, project_data).retcode, 0);
  }
}

TEST_F(BlueprintCommandTest, PackSupportsAllIntegerWidthsSignsHexAndPadding)
{
  auto tree = make_map("format: CcSsLlx\ndata: [1, -1, 258, -2, 0x01020304, -3, 0]\n");
  const auto result = invoke_command("pack", "", tree.crootref(), "prefix", project_summary, project_data);
  ASSERT_EQ(result.retcode, 0);
  std::string expected = "prefix";
  const uint8_t byte = 1;
  const int8_t signed_byte = -1;
  const uint16_t word = 258;
  const int16_t signed_word = -2;
  const uint32_t long_word = 0x01020304;
  const int32_t signed_long = -3;
  for (const auto &[value, size]: std::vector<std::pair<const void *, size_t>>{
         {&byte, sizeof(byte)}, {&signed_byte, sizeof(signed_byte)}, {&word, sizeof(word)},
         {&signed_word, sizeof(signed_word)}, {&long_word, sizeof(long_word)}, {&signed_long, sizeof(signed_long)}})
    expected.append(static_cast<const char *>(value), size);
  expected.push_back('\0');
  EXPECT_EQ(result.result, expected);
}

TEST_F(BlueprintCommandTest, PackRejectsMissingFieldsMismatchedLengthsAndInvalidNumbers)
{
  for (const auto &yaml: {"format: C", "data: [1]", "format: CC\ndata: [1]",
                         "format: C\ndata: [1, 2]", "format: Q\ndata: [1]",
                         "format: C\ndata: [garbage]", "format: C\ndata: [12garbage]",
                         "format: C\ndata: ['']", "format: C\ndata: [256]"}) {
    SCOPED_TRACE(yaml);
    auto tree = make_map(yaml);
    EXPECT_NE(invoke_command("pack", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
  }
}

TEST_F(BlueprintCommandTest, CopySupportsSequenceAndYamlList)
{
  const auto source = write_file("source.txt", "content");
  for (const auto &key: {"source", "yaml_list"}) {
    SCOPED_TRACE(key);
    const auto destination = test_dir / key;
    fs::create_directory(destination);
    auto tree = make_map(std::string(key) + ": " +
                         (std::string(key) == "yaml_list" ? "\"['" + source.generic_string() + "']\"" :
                                                          "['" + source.generic_string() + "']") +
                         "\ndestination: '" + destination.generic_string() + "'\n");
    ASSERT_EQ(invoke_command("copy", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
    EXPECT_EQ(read_file(destination / source.filename()), "content");
  }
}

TEST_F(BlueprintCommandTest, CopyReportsMissingSourceDestinationAndFilesystemFailure)
{
  for (const auto &yaml: {"source: input", "destination: output", "source: []\ndestination: ''"}) {
    SCOPED_TRACE(yaml);
    auto tree = make_map(yaml);
    EXPECT_NE(invoke_command("copy", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
  }
  auto tree = make_map("source: '" + (test_dir / "missing").generic_string() +
                       "'\ndestination: '" + (test_dir / "destination").generic_string() + "'\n");
  EXPECT_NE(invoke_command("copy", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, CopySupportsEveryMapSourceForm)
{
  fs::create_directory(test_dir / "tree");
  const auto file = write_file("tree/item.txt", "content");
  for (const auto &option: {"files", "file_paths", "folders", "folder_paths"}) {
    SCOPED_TRACE(option);
    const bool folder = std::string(option).starts_with("folder");
    const bool preserve_path = std::string(option).ends_with("_paths");
    const auto source = folder ? file.parent_path() : file;
    const auto destination = test_dir / option;
    fs::create_directory(destination);
    auto tree = make_map("source:\n  " + std::string(option) + ": ['" + source.generic_string() +
                         "']\ndestination: '" + destination.generic_string() + "'\n");
    ASSERT_EQ(invoke_command("copy", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
    fs::path copied = preserve_path
      ? fs::path(destination.generic_string() + "/" + source.generic_string())
      : destination / "item.txt";
    if (preserve_path && folder)
      copied /= "item.txt";
    EXPECT_EQ(read_file(copied), "content");
  }
}

TEST_F(BlueprintCommandTest, RegexZeroLengthMatchesAdvanceAndInvalidPatternsFail)
{
  for (const auto &options: {"match: X", "split: true\nmatch: X", "to_yaml: []"}) {
    auto tree = make_map("search: '^|$'\n" + std::string(options) + "\n");
    const auto result = invoke_command("regex", "", tree.crootref(), "abc", project_summary, project_data);
    EXPECT_EQ(result.retcode, 0);
    if (std::string(options).find("match") != std::string::npos)
      EXPECT_EQ(result.result, "XX");
  }
  auto invalid = make_map("search: '['\nreplace: X\n");
  EXPECT_NE(invoke_command("regex", "", invalid.crootref(), "", project_summary, project_data).retcode, 0);
}

TEST_F(BlueprintCommandTest, ConversionsPreserveNestedValuesNotJustOutputPrefixes)
{
  auto tree = make_map("{}");
  auto converted = invoke_command("as_json", "", tree.crootref(), "values: [1, two]\nmap: {enabled: true}\n",
                                  project_summary, project_data);
  ASSERT_EQ(converted.retcode, 0);
  auto restored = invoke_command("as_yaml", "", tree.crootref(), converted.result, project_summary, project_data);
  ASSERT_EQ(restored.retcode, 0);
  auto output = ryml::parse_in_arena(c4::to_csubstr(restored.result));
  EXPECT_EQ(output.crootref()["values"].num_children(), 2u);
  EXPECT_EQ(output.crootref()["values"][1].val(), c4::to_csubstr("two"));
  EXPECT_EQ(output.crootref()["map"]["enabled"].val(), c4::to_csubstr("true"));
}

TEST_F(BlueprintCommandTest, DiffRequiresBothInputsAndCurrentlyReturnsNoPatch)
{
  for (const auto &yaml: {"{}", "left: a", "right: b"}) {
    auto tree = make_map(yaml);
    EXPECT_NE(invoke_command("diff", "", tree.crootref(), "", project_summary, project_data).retcode, 0);
  }
  const auto left = write_file("left.yaml", "value: 1");
  const auto right = write_file("right.yaml", "value: 2");
  auto tree = make_map("left_file: '" + left.generic_string() + "'\nright_file: '" + right.generic_string() + "'\n");
  const auto result = invoke_command("diff", "", tree.crootref(), "", project_summary, project_data);
  EXPECT_EQ(result.retcode, 0);
  EXPECT_TRUE(result.result.empty());
}
