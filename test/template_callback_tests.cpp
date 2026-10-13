// =============================================================================
// Section A8: Template Callback Integration Unit Tests
//
// Tests each callback registered by add_common_template_commands() in
// yakka/utilities.cpp, exercised through inja::Environment::render().
//
// Build: include in the unit tests yakka target alongside other test sources.
// Run:   ./unit_tests --gtest_filter="TemplateCallback*"
// =============================================================================

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/null_sink.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class TemplateCallbackTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    if (!spdlog::get("console")) {
      auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
      auto logger    = std::make_shared<spdlog::logger>("console", null_sink);
      spdlog::register_logger(logger);
    }

    yakka::add_common_template_commands(env);

    // Stable empty data node for renders that don't need data.
    empty_tree = ryml::parse_in_arena("{}");
    empty_data = empty_tree.crootref();

    // Create temp dir for filesystem callbacks.
    test_dir = fs::temp_directory_path() / ("yakka_tpl_test_" + std::to_string(
                 std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(test_dir);
  }

  void TearDown() override
  {
    fs::remove_all(test_dir);
  }

  // Render a template string against the given data and return the result.
  std::string render(const std::string &tpl, ryml::ConstNodeRef data)
  {
    return yakka::try_render(env, tpl, data);
  }

  std::string render(const std::string &tpl)
  {
    return render(tpl, empty_data);
  }

  // Write a file in the test directory and return its absolute path string.
  std::string write_test_file(const std::string &name, const std::string &content)
  {
    auto p = test_dir / name;
    std::ofstream f(p);
    f << content;
    return p.generic_string();
  }

  inja::Environment env;
  ryml::Tree empty_tree;
  ryml::ConstNodeRef empty_data;
  fs::path test_dir;
};

// ---------------------------------------------------------------------------
// A8-001 – Path Operations
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, DirExtractsParent)
{
  // dir() returns the parent directory of a file path.
  auto result = render(R"({{ dir("/some/path/file.txt") }})");
  EXPECT_EQ(result, "/some/path");
}

TEST_F(TemplateCallbackTest, DirOnDirectoryReturnsSelf)
{
  // dir() on a path without a filename returns the path itself.
  auto result = render(R"({{ dir("/some/path/") }})");
  EXPECT_THAT(result, ::testing::HasSubstr("some/path"));
}

TEST_F(TemplateCallbackTest, NotDirExtractsFilename)
{
  auto result = render(R"({{ not_dir("/some/path/file.txt") }})");
  EXPECT_EQ(result, "file.txt");
}

TEST_F(TemplateCallbackTest, ParentPathReturnsParent)
{
  auto result = render(R"({{ parent_path("/some/path/file.txt") }})");
  EXPECT_EQ(result, "/some/path");
}

TEST_F(TemplateCallbackTest, ExtensionWithoutDot)
{
  auto result = render(R"({{ extension("main.cpp") }})");
  EXPECT_EQ(result, "cpp");
}

TEST_F(TemplateCallbackTest, AbsolutePathIsAbsolute)
{
  // absolute_path on a relative path should return an absolute path.
  auto result = render(R"({{ absolute_path("relative.txt") }})");
  EXPECT_TRUE(fs::path(result).is_absolute());
}

TEST_F(TemplateCallbackTest, RelativePathTwoArgs)
{
  // relative_path(path1, path2) makes path1 relative to path2.
  auto result = render(R"({{ relative_path("/a/b/c.txt", "/a/b") }})");
  EXPECT_EQ(result, "c.txt");
}

// ---------------------------------------------------------------------------
// A8-001 – File System Queries
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, FileExistsTrue)
{
  auto p   = write_test_file("exists.txt", "data");
  auto tpl = R"({{ file_exists(")" + p + R"(") }})";
  auto result = render(tpl);
  EXPECT_EQ(result, "1");
}

TEST_F(TemplateCallbackTest, FileExistsFalse)
{
  auto missing = (test_dir / "no_such_file.txt").generic_string();
  auto tpl     = R"({{ file_exists(")" + missing + R"(") }})";
  auto result  = render(tpl);
  EXPECT_EQ(result, "0");
}

TEST_F(TemplateCallbackTest, FilesizeMatchesActualSize)
{
  auto p      = write_test_file("sized.txt", "1234567890");
  auto tpl    = R"({{ filesize(")" + p + R"(") }})";
  auto result = render(tpl);
  EXPECT_EQ(result, "10");
}

// ---------------------------------------------------------------------------
// A8-002 – File Content Operations
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, ReadFileReturnsContents)
{
  auto p      = write_test_file("read_me.txt", "read file content");
  auto tpl    = R"({{ read_file(")" + p + R"(") }})";
  auto result = render(tpl);
  EXPECT_EQ(result, "read file content");
}

TEST_F(TemplateCallbackTest, LoadYamlReturnsParsedNode)
{
  auto p      = write_test_file("data.yaml", "key: hello\n");
  auto tpl    = R"({% set d = load_yaml(")" + p + R"(") %}{{ d.key }})";
  auto result = render(tpl);
  EXPECT_EQ(result, "hello");
}

TEST_F(TemplateCallbackTest, LoadJsonReturnsParsedNode)
{
  auto p      = write_test_file("data.json", R"({"key":"world"})");
  auto tpl    = R"({% set d = load_json(")" + p + R"(") %}{{ d.key }})";
  auto result = render(tpl);
  EXPECT_EQ(result, "world");
}

// ---------------------------------------------------------------------------
// A8-003 – Malformed Data Graceful Failure
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, ReadFileMissingReturnsEmpty)
{
  // read_file on a missing path should not crash; returns empty string.
  auto missing = (test_dir / "missing_read.txt").generic_string();
  auto tpl     = R"({{ read_file(")" + missing + R"(") }})";
  // Should not throw; may return empty
  EXPECT_NO_THROW(render(tpl));
}

// ---------------------------------------------------------------------------
// A8-004 – String Operations
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, QuoteWrapsInDoubleQuotes)
{
  auto result = render(R"({{ quote("hello") }})");
  EXPECT_EQ(result, "\"hello\"");
}

TEST_F(TemplateCallbackTest, ReplaceSubstitutesPattern)
{
  auto result = render(R"({{ replace("foo bar foo", "foo", "baz") }})");
  EXPECT_EQ(result, "baz bar baz");
}

TEST_F(TemplateCallbackTest, RegexEscapeEscapesMetacharacters)
{
  auto result = render(R"({{ regex_escape("a.b+c") }})");
  EXPECT_THAT(result, ::testing::HasSubstr("\\"));
}

TEST_F(TemplateCallbackTest, SplitProducesArray)
{
  auto result = render(R"({% for x in split("a,b,c", ",") %}{{ x }};{% endfor %})");
  EXPECT_EQ(result, "a;b;c;");
}

TEST_F(TemplateCallbackTest, StartsWithTrue)
{
  auto result = render(R"({{ startswith("hello world", "hello") }})");
  EXPECT_EQ(result, "true");
}

TEST_F(TemplateCallbackTest, StartsWithFalse)
{
  auto result = render(R"({{ startswith("world hello", "hello") }})");
  EXPECT_EQ(result, "false");
}

TEST_F(TemplateCallbackTest, LegacyStartsWithIsNotRegistered)
{
  EXPECT_THROW(env.parse(R"({{ starts_with("hello world", "hello") }})"), inja::ParserError);
}

TEST_F(TemplateCallbackTest, BuiltinStartsWithReturnsBoolean)
{
  inja::Environment builtin_env;
  EXPECT_EQ(builtin_env.render(R"({{ startswith("hello world", "hello") }})", empty_data), "true");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("world hello", "hello") }})", empty_data), "false");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("Hello", "hello") }})", empty_data), "false");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("hello", "hello") }})", empty_data), "true");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("hi", "hello") }})", empty_data), "false");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("hello", "") }})", empty_data), "true");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("", "") }})", empty_data), "true");
  EXPECT_EQ(builtin_env.render(R"({{ startswith("", "hello") }})", empty_data), "false");
}

TEST_F(TemplateCallbackTest, BuiltinStartsWithSupportsDataAndBooleanExpressions)
{
  inja::Environment builtin_env;
  auto data = ryml::parse_in_arena("input: hello world\nprefix: hello\n");
  EXPECT_EQ(builtin_env.render(
    R"({% if startswith(input, prefix) and not startswith(input, "world") %}matched{% endif %})",
    data.crootref()), "matched");
  EXPECT_EQ(builtin_env.render(R"({{ startswith(input, prefix) == true }})", data.crootref()), "true");
}

TEST_F(TemplateCallbackTest, BuiltinStartsWithRequiresTwoArguments)
{
  inja::Environment builtin_env;
  EXPECT_THROW(builtin_env.render(R"({{ startswith("hello") }})", empty_data), inja::ParserError);
  EXPECT_THROW(builtin_env.render(R"({{ startswith("hello", "he", "x") }})", empty_data), inja::ParserError);
}

TEST_F(TemplateCallbackTest, SubstringFromIndex)
{
  auto result = render(R"({{ substring("abcdef", 2) }})");
  EXPECT_EQ(result, "cdef");
}

TEST_F(TemplateCallbackTest, TrimRemovesWhitespace)
{
  auto result = render(R"({{ trim("  hello  ") }})");
  EXPECT_EQ(result, "hello");
}

// ---------------------------------------------------------------------------
// A8-004 – Collection Operations
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, JoinArrayWithDelimiter)
{
  ryml::Tree data_tree = ryml::parse_in_arena("items:\n  - a\n  - b\n  - c\n");
  auto data            = data_tree.crootref();
  auto result          = render(R"({{ join(items, "-") }})", data);
  EXPECT_EQ(result, "a-b-c");
}

TEST_F(TemplateCallbackTest, ConcatenateStrings)
{
  auto result = render(R"({{ concatenate("hello", " ", "world") }})");
  EXPECT_EQ(result, "hello world");
}

TEST_F(TemplateCallbackTest, FilterMatchingItems)
{
  ryml::Tree data_tree = ryml::parse_in_arena("names:\n  - foo.cpp\n  - bar.h\n  - baz.cpp\n");
  auto data            = data_tree.crootref();
  auto result          = render(R"({% for x in filter(names, ".*\\.cpp") %}{{ x }};{% endfor %})", data);
  EXPECT_EQ(result, "foo.cpp;baz.cpp;");
}

// ---------------------------------------------------------------------------
// A8-005 – Numeric/Hash Operations
// ---------------------------------------------------------------------------

TEST_F(TemplateCallbackTest, Hex2DecConvertsHexToDecimal)
{
  auto result = render(R"({{ hex2dec("FF") }})");
  EXPECT_EQ(result, "255");
}

TEST_F(TemplateCallbackTest, Hex2DecZero)
{
  auto result = render(R"({{ hex2dec("0") }})");
  EXPECT_EQ(result, "0");
}

TEST_F(TemplateCallbackTest, Xxh64sumProducesHexString)
{
  // xxh64sum should produce a hex string for any input.
  auto result = render(R"({{ xxh64sum("test_input") }})");
  EXPECT_FALSE(result.empty());
  // Valid hex chars only.
  for (char c : result)
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
}

TEST_F(TemplateCallbackTest, Sha256sumProducesHexString)
{
  auto result = render(R"({{ sha256sum("test_input") }})");
  EXPECT_FALSE(result.empty());
  for (char c : result)
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
}

TEST_F(TemplateCallbackTest, ArgumentOnlyBuiltinsWorkWithoutYakkaCallbacks)
{
  inja::Environment builtin_env;
  const std::vector<std::pair<std::string, std::string>> cases = {
    { R"({{ dir("/some/path/file.txt") }})", "/some/path" },
    { R"({{ dir("/some/path/") }})", "/some/path/" },
    { R"({{ not_dir("/some/path/file.txt") }})", "file.txt" },
    { R"({{ parent_path("/some/path/file.txt") }})", "/some/path" },
    { R"({{ extension("file.txt") }})", "txt" },
    { R"({{ extension("file") }})", "" },
    { R"({{ extension(".hidden") }})", "" },
    { R"({{ quote("hello") }})", "\"hello\"" },
    { R"({{ quote("a\"b\\c") }})", R"("a\"b\\c")" },
    { R"({{ regex_escape("a.b+c") }})", R"(a\.b\+c)" },
    { R"({{ hex2dec("FF") }})", "255" },
    { R"({{ hex2dec("0x10") }})", "16" },
    { R"({{ concatenate("hello", " ", "world") }})", "hello world" },
    { R"({{ concatenate("only") }})", "only" },
    { R"({{ join(["a", "b", "c"], "-") }})", "a-b-c" },
    { R"({{ join([], "-") }})", "" },
    { R"({{ join(["01", "1.00"], "-") }})", "01-1.00" },
    { R"({{ sha256sum("abc") }})", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
    { R"({{ sha256sum("") }})", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
    { R"({{ xxh64sum("") }})", "ef46db3751d8e999" },
    { R"({{ xxh64sum("abc") }})", "44bc2cf5ad770999" },
    { R"({{ fnv1a_32("") }})", "811c9dc5" },
    { R"({{ fnv1a_32("hello") }})", "4f9f2cab" },
  };
  for (const auto &[expression, expected]: cases) {
    SCOPED_TRACE(expression);
    EXPECT_EQ(builtin_env.render(expression, empty_data), expected);
    EXPECT_EQ(env.render(expression, empty_data), expected);
  }
}

TEST_F(TemplateCallbackTest, ArgumentOnlyBuiltinsFindNestedPathsAndHashContainers)
{
  inja::Environment builtin_env;
  auto data = ryml::parse_in_arena(
    "name: root\nnested:\n  name: child\n"
    "items:\n  - name: first\n  - nested:\n      name: second\n"
    "payload: [a, b]\n");
  const auto expression = R"({{ join(find_json(input, "name"), ";") }})";
  auto wrapper = ryml::parse_in_arena("{}");
  wrapper.rootref()["input"] |= ryml::MAP;
  wrapper.duplicate_contents(&data, data.root_id(), wrapper.rootref()["input"].id());
  EXPECT_EQ(builtin_env.render(expression, wrapper.crootref()),
            "name;nested.name;items[0].name;items[1].nested.name");
  EXPECT_EQ(builtin_env.render(R"({{ join(find_json(input, "absent"), ";") }})", wrapper.crootref()), "");
  EXPECT_EQ(builtin_env.render(R"({{ length(find_json("scalar", "name")) }})", empty_data), "0");
  for (const auto function: { "sha256sum", "xxh64sum", "fnv1a_32" }) {
    const auto expression = std::string("{{ ") + function + "(payload) }}";
    const auto serialized = ryml::emitrs_json<std::string>(data.crootref()["payload"]);
    auto hash_data = ryml::parse_in_arena("{}");
    hash_data.rootref()["text"] << serialized;
    const auto expected = builtin_env.render(std::string("{{ ") + function + "(text) }}", hash_data.crootref());
    EXPECT_EQ(builtin_env.render(expression, data.crootref()), expected);
    EXPECT_EQ(env.render(expression, data.crootref()), expected);
  }
}

TEST_F(TemplateCallbackTest, ArgumentOnlyBuiltinsReportInvalidInputsAndArity)
{
  inja::Environment builtin_env;
  EXPECT_THROW(builtin_env.render(R"({{ join("text", ",") }})", empty_data), inja::RenderError);
  EXPECT_THROW(builtin_env.render(R"({{ hex2dec("invalid") }})", empty_data), std::invalid_argument);
  EXPECT_THROW(builtin_env.parse(R"({{ concatenate() }})"), inja::ParserError);
  EXPECT_THROW(builtin_env.parse(R"({{ find_json({}) }})"), inja::ParserError);
  for (const auto function: { "dir", "not_dir", "parent_path", "extension", "quote", "regex_escape",
                             "hex2dec", "sha256sum", "xxh64sum", "fnv1a_32" }) {
    SCOPED_TRACE(function);
    EXPECT_THROW(builtin_env.parse(std::string("{{ ") + function + "() }}"), inja::ParserError);
    EXPECT_THROW(builtin_env.parse(std::string("{{ ") + function + "(\"a\", \"b\") }}"), inja::ParserError);
  }
}

TEST_F(TemplateCallbackTest, GlobReturnsMatchingPaths)
{
  // Create a few files and glob them.
  write_test_file("file1.txt", "a");
  write_test_file("file2.txt", "b");
  auto pattern = (test_dir / "*.txt").generic_string();
  auto tpl     = R"({% set files = glob(")" + pattern + R"(") %}{{ files | length }})";
  auto result  = render(tpl);
  EXPECT_EQ(result, "2");
}
