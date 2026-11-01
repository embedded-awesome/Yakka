#include <filesystem>

#ifdef INJA_TEST_SINGLE_HEADER
#include "inja/inja.hpp"
#else
#include "inja/environment.hpp"
#endif
#include "test-common.hpp"

TEST_CASE("startswith") {
  inja::Environment env;
  auto values = ryml::parse_in_arena(R"(
text: hello world
empty: ""
quoted_number: "123"
quoted_bool: "true"
quoted_null: "null"
whitespace: "  hello"
number: 123
decimal: 1.25
boolean: true
nothing: null
sequence: [hello]
mapping: {hello: world}
empty_sequence: []
empty_mapping: {}
)");
  CHECK(env.render(R"({{ startswith(text, "hello") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ text | startswith("hello") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith(text, "world") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith(text, "Hello") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith(text, empty) }})", values.rootref()) == "true");
  CHECK(env.render("{{ startswith(empty, empty) }}", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith(empty, "hello") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith(quoted_number, "12") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith(quoted_bool, "tr") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith(quoted_null, "nu") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith(whitespace, "  ") }})", values.rootref()) == "true");
  CHECK(env.render(R"({{ startswith("123", "12") }})", values.rootref()) == "true");

  for (const auto* value : {"number", "decimal", "boolean", "nothing", "sequence", "mapping",
                            "empty_sequence", "empty_mapping"}) {
    CAPTURE(value);
    CHECK(env.render(std::string("{{ startswith(") + value + ", empty) }}", values.rootref()) == "false");
    CHECK(env.render(std::string("{{ startswith(text, ") + value + ") }}", values.rootref()) == "false");
    CHECK(env.render(std::string("{{ ") + value + " | startswith(empty) }}", values.rootref()) == "false");
  }
  CHECK(env.render(R"({{ startswith(123, "12") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith(true, "tr") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith(null, "") }})", values.rootref()) == "false");
  CHECK(env.render(R"({{ startswith("123", 12) }})", values.rootref()) == "false");
}
