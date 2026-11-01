#include "inja/inja.hpp"
#include <doctest/doctest.h>

TEST_CASE("keys") {
  inja::Environment env;
  auto data = ryml::parse_in_arena(R"({
    "components": {"uart": {"name": "UART"}, "spi": {}, "gpio": null},
    "wanted": ["spi", "missing", "uart", "spi"],
    "special": {"123": 1, "true": 2, "null": 3, "": 4, "a.b": 5, "a/b": 6}
  })");
  const auto root = data.crootref();

  CHECK(env.render("{{ keys(components) }}", root) == R"(["uart","spi","gpio"])");
  CHECK(env.render("{{ components | keys }}", root) == R"(["uart","spi","gpio"])");
  CHECK(env.render("{{ keys({}) }}", root) == "[]");
  CHECK(env.render("{{ isArray(keys({})) }}", root) == "true");
  CHECK(env.render(R"({{ keys({"only": [1, 2]}) }})", root) == R"(["only"])");
  CHECK(env.render("{{ keys(special) }}", root) == R"(["123","true","null","","a.b","a/b"])");
  CHECK(env.render("{{ intersect(wanted, keys(components)) }}", root) == R"(["spi","uart"])");
  CHECK(env.render("{{ wanted | intersect(components | keys) | join(\",\") }}", root) == "spi,uart");
  CHECK(env.render(R"({% set m = {"b": 2, "a": 1} %}{% set k = keys(m) %}{{ k }}|{{ m }})", root) ==
        R"(["b","a"]|{"b": 2,"a": 1})");
  CHECK(env.render("{% for key in keys(components) %}{{ key }};{% endfor %}", root) == "uart;spi;gpio;");
  CHECK(root["components"].num_children() == 3);
  CHECK(root["components"]["uart"]["name"].val() == "UART");

  CHECK_THROWS_WITH(env.render("{{ keys([]) }}", root),
                    "[inja.exception.render_error] (at 1:4) keys() expects a map");
  CHECK_THROWS_WITH(env.render("{{ keys(123) }}", root),
                    "[inja.exception.render_error] (at 1:4) keys() expects a map");
  CHECK_THROWS_WITH(env.render("{{ keys(\"abc\") }}", root),
                    "[inja.exception.render_error] (at 1:4) keys() expects a map");
  CHECK_THROWS_WITH(env.render("{{ keys(null) }}", root),
                    "[inja.exception.render_error] (at 1:4) keys() expects a map");
  CHECK_THROWS_WITH(env.render("{{ keys(true) }}", root),
                    "[inja.exception.render_error] (at 1:4) keys() expects a map");
  CHECK_THROWS(env.render("{{ keys() }}", root));
  CHECK_THROWS(env.render("{{ keys({}, {}) }}", root));
}
