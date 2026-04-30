#include "inja.hpp"
#include "ryml.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class test_mode {
  standard,
  custom_syntax,
};

struct test_case {
  std::string name;
  std::filesystem::path template_path;
  std::filesystem::path data_path;
  std::filesystem::path expected_path;
  test_mode mode {test_mode::standard};
};

std::string read_file(const std::filesystem::path& filename) {
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file: " + filename.string());
  }

  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

std::string normalize_newlines(std::string text) {
  std::string normalized;
  normalized.reserve(text.size());

  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r') {
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }

    normalized.push_back(text[index]);
  }

  return normalized;
}

std::filesystem::path find_suite_root() {
  const auto source_relative = std::filesystem::path(__FILE__).parent_path() / "inja_suite";
  if (std::filesystem::exists(source_relative)) {
    return std::filesystem::weakly_canonical(source_relative);
  }

  auto current = std::filesystem::current_path();
  for (;;) {
    const auto candidate = current / "test" / "inja_suite";
    if (std::filesystem::exists(candidate)) {
      return std::filesystem::weakly_canonical(candidate);
    }

    if (current == current.root_path()) {
      break;
    }
    current = current.parent_path();
  }

  throw std::runtime_error("failed to locate test/inja_suite");
}

void register_callbacks(inja::Environment& env) {
  env.add_callback("render_marker", 0, [](inja::Arguments&, inja::NodeRef additional_data) {
    return inja::ensure_child_seq(additional_data, "values").append_child() << "CALLBACK";
  });
}

inja::Environment make_environment(const std::filesystem::path& suite_root, test_mode mode) {
  inja::Environment env(suite_root);
  register_callbacks(env);

  if (mode == test_mode::custom_syntax) {
    env.set_expression("<<", ">>");
    env.set_line_statement("$$");
    env.set_comment("/*", "*/");
  }

  return env;
}

ryml::Tree load_yaml(const std::filesystem::path& filename) {
  const auto contents = read_file(filename);
  return ryml::parse_in_arena(ryml::to_csubstr(filename.string()), ryml::to_csubstr(contents));
}

bool run_case(const std::filesystem::path& suite_root, const test_case& current_case) {
  try {
    auto env = make_environment(suite_root, current_case.mode);
    auto data_tree = load_yaml(suite_root / current_case.data_path);
    const auto expected = normalize_newlines(read_file(suite_root / current_case.expected_path));
    const auto actual = normalize_newlines(env.render_file(current_case.template_path, data_tree.rootref()));

    if (actual == expected) {
      std::cout << "[PASS] " << current_case.name << '\n';
      return true;
    }

    std::cerr << "[FAIL] " << current_case.name << '\n';
    std::cerr << "Expected:\n" << expected << "\n";
    std::cerr << "Actual:\n" << actual << "\n";
    return false;
  } catch (const std::exception& ex) {
    std::cerr << "[FAIL] " << current_case.name << " threw: " << ex.what() << '\n';
    return false;
  }
}

} // namespace

int main() {
  const auto suite_root = find_suite_root();

  const std::vector<test_case> cases = {
      {"filters-and-types", "templates/basics.inja", "data/basics.yaml", "expected/basics.txt"},
      {"array-loop-metadata", "templates/array_loop_meta.inja", "data/array_loop_meta.yaml", "expected/array_loop_meta.txt"},
      {"object-loop-metadata", "templates/object_loop_meta.inja", "data/object_loop_meta.yaml", "expected/object_loop_meta.txt"},
      {"includes-and-callbacks", "templates/includes/main.inja", "data/includes.yaml", "expected/includes.txt"},
      {"inheritance-and-super", "templates/inheritance/child.inja", "data/empty.yaml", "expected/inheritance.txt"},
      {"macros-and-global-scope", "templates/macros/main.inja", "data/macros.yaml", "expected/macros.txt"},
      {"mutation-helpers", "templates/mutations.inja", "data/mutations.yaml", "expected/mutations.txt"},
      {"missing-loop-is-empty", "templates/missing_loop.inja", "data/empty.yaml", "expected/missing_loop.txt"},
      {"custom-syntax", "templates/custom_syntax.inja", "data/custom_syntax.yaml", "expected/custom_syntax.txt", test_mode::custom_syntax},
  };

  bool all_passed = true;
  for (const auto& current_case : cases) {
    all_passed = run_case(suite_root, current_case) && all_passed;
  }

  return all_passed ? 0 : 1;
}
