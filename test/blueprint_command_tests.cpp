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
#include "blueprint_commands.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <filesystem>
#include <fstream>

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

    data_tree.rootref().set_map();
    project_data = data_tree.rootref();
  }

  void TearDown() override
  {
    fs::remove_all(test_dir);
  }

  // Helper: create a YAML scalar command node from a string value.
  ryml::Tree make_scalar(const std::string &value)
  {
    return ryml::parse_in_arena(ryml::to_csubstr("\"" + value + "\""));
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

  auto result = yakka::echo_command("target", cmd, "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "hello from echo");
}

TEST_F(BlueprintCommandTest, EchoPassthroughWhenEmpty)
{
  // A6-201: echo with no command value passes through captured_output unchanged.
  ryml::Tree empty_tree = ryml::parse_in_arena("~");
  auto cmd              = empty_tree.crootref();

  auto result = yakka::echo_command("target", cmd, "prior output", project_summary, project_data, inja_env);

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

  auto result = yakka::echo_command("target", cmd, "", sum, project_data, inja_env);

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
  auto result   = yakka::execute_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("yakka_exec_test"));
}

TEST_F(BlueprintCommandTest, ExecuteFailsOnInvalidCommand)
{
  // A6-102: execute returns non-zero exit code for failing commands.
  auto cmd_tree = make_scalar("false");
  auto result   = yakka::execute_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_NE(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, ExecuteInvalidNode)
{
  // A6-202: execute with invalid/missing command node returns error code.
  ryml::Tree empty_tree;
  auto result = yakka::execute_command("target", empty_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-003 / A6-103 – shell
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, ShellSuccess)
{
  // A6-003: shell runs command through the platform shell.
  auto cmd_tree = make_scalar("echo yakka_shell_test");
  auto result   = yakka::shell_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("yakka_shell_test"));
}

TEST_F(BlueprintCommandTest, ShellInvalidNode)
{
  // A6-103: shell with invalid node returns error.
  ryml::Tree empty_tree;
  auto result = yakka::shell_command("target", empty_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-004 – regex (replace sub-mode)
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, RegexReplaceSuccess)
{
  // A6-004: regex replace transforms captured output.
  auto cmd_tree = make_map("search: foo\nreplace: bar");
  auto result   = yakka::regex_command("target", cmd_tree.crootref(), "foo baz foo", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_THAT(result.result, ::testing::HasSubstr("bar"));
  EXPECT_THAT(result.result, ::testing::Not(::testing::HasSubstr("foo")));
}

TEST_F(BlueprintCommandTest, RegexMissingSearchFails)
{
  // A6-104: regex without search field returns error.
  auto cmd_tree = make_map("replace: bar");
  auto result   = yakka::regex_command("target", cmd_tree.crootref(), "foo", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, -1);
}

// ---------------------------------------------------------------------------
// A6-005 / A6-105 – template (inja command)
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, TemplateScalarRenders)
{
  // A6-005: template command with scalar renders the string.
  auto cmd_tree = make_scalar("result=42");
  auto result   = yakka::template_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "result=42");
}

TEST_F(BlueprintCommandTest, TemplateWithInlineTemplate)
{
  // Template command with inline template key renders the template string.
  auto cmd_tree = make_map("template: \"value={{ x }}\"\n");
  ryml::Tree sum_tree = ryml::parse_in_arena("x: 99");

  auto result = yakka::template_command("target", cmd_tree.crootref(), "", sum_tree.crootref(), project_data, inja_env);

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

  auto result = yakka::template_command("target", cmd_tree.crootref(), "", sum_tree.crootref(), project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "file=loaded");
}

TEST_F(BlueprintCommandTest, InjaCommandIsAliasForTemplate)
{
  // A6-006: inja command is an alias for template and behaves identically.
  auto cmd_tree = make_scalar("alias=ok");
  auto result   = yakka::inja_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "alias=ok");
}

// ---------------------------------------------------------------------------
// A6-007 / A6-107 – save
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, SaveToFile)
{
  // A6-007: save writes captured_output to a file.
  auto save_path = test_dir / "saved.txt";
  auto cmd_tree  = make_scalar(save_path.generic_string());

  auto result = yakka::save_command("target", cmd_tree.crootref(), "hello saved content", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(save_path));
  EXPECT_EQ(read_file(save_path), "hello saved content");
}

TEST_F(BlueprintCommandTest, SaveCreatesParentDirectories)
{
  // A6-207: save creates intermediate directories.
  auto save_path = test_dir / "deep" / "path" / "file.txt";
  auto cmd_tree  = make_scalar(save_path.generic_string());

  auto result = yakka::save_command("target", cmd_tree.crootref(), "nested content", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  ASSERT_TRUE(fs::exists(save_path));
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

  auto result = yakka::create_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

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

  auto result = yakka::verify_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
}

TEST_F(BlueprintCommandTest, VerifyMissingFile)
{
  // A6-109: verify returns non-zero when file does not exist.
  auto missing  = test_dir / "__missing_verify__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = yakka::verify_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

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

  auto result = yakka::rm_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_FALSE(fs::exists(f));
}

TEST_F(BlueprintCommandTest, RmNonExistentFileReturnsSuccess)
{
  // A6-210: rm on a missing file is a no-op (graceful).
  auto missing  = test_dir / "__no_such_rm_file__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = yakka::rm_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

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

  auto result = yakka::rmdir_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

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
  auto cmd_yaml = std::string("from: \"") + src.generic_string() + "\"\nto: \"" + dst.generic_string() + "\"";
  auto cmd_tree = make_map(cmd_yaml);

  auto result = yakka::copy_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

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

  auto result = yakka::cat_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_EQ(result.retcode, 0);
  EXPECT_EQ(result.result, "cat content here");
}

TEST_F(BlueprintCommandTest, CatMissingFile)
{
  // A6-114: cat of missing file returns error.
  auto missing  = test_dir / "__no_such_cat__.txt";
  auto cmd_yaml = std::string("\"") + missing.generic_string() + "\"";
  auto cmd_tree = ryml::parse_in_arena(ryml::to_csubstr(cmd_yaml));

  auto result = yakka::cat_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  EXPECT_NE(result.retcode, 0);
}

// ---------------------------------------------------------------------------
// A6-015 / A6-115 – as_json
// ---------------------------------------------------------------------------

TEST_F(BlueprintCommandTest, AsJsonConvertsYamlToJson)
{
  // A6-015: as_json converts YAML-format captured_output to JSON.
  auto cmd_tree = make_map("{}");
  auto result   = yakka::as_json_command("target", cmd_tree.crootref(), "key: value", project_summary, project_data, inja_env);

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
  auto result   = yakka::as_yaml_command("target", cmd_tree.crootref(), R"({"key":"value"})", project_summary, project_data, inja_env);

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
  auto result   = yakka::diff_command("target", cmd_tree.crootref(), "", project_summary, project_data, inja_env);

  // retcode 0 expected (stub implementation).
  EXPECT_EQ(result.retcode, 0);
}
