#include "inja/inja.hpp"
#include <doctest/doctest.h>

TEST_CASE("intersect") {
  inja::Environment env;
  auto data = ryml::parse_in_arena(R"({"names": ["Jeff", "Seb", "Peter", "Tom"], "name": "Jeff"})");
  const auto root = data.crootref();

  CHECK(env.render("{{ intersect([3, 1, 2], [2, 3, 4]) }}", root) == "[3,2]");
  CHECK(env.render("{{ intersect([3, 1, 3, 2, 2], [2, 3, 3]) }}", root) == "[3,2]");
  CHECK(env.render(R"({{ intersect(names, ["Tom", "Jeff", "Nobody"]) }})", root) == R"(["Jeff","Tom"])");
  CHECK(env.render("{{ intersect([1, 2], [3, 4]) }}", root) == "[]");
  CHECK(env.render("{{ intersect([], [1]) }}", root) == "[]");
  CHECK(env.render("{{ intersect([1], []) }}", root) == "[]");
  CHECK(env.render("{{ intersect([], []) }}", root) == "[]");
  CHECK(env.render("{{ isArray(intersect([], [])) }}", root) == "true");
  CHECK(env.render("{{ [3, 1, 2] | intersect([2, 3]) | join(\",\") }}", root) == "3,2");
  CHECK(env.render("{{ intersect([2.0, 1, 2], [1.0, 2]) }}", root) == "[2.0,1]");
  CHECK(env.render("{{ intersect([true, false, null], [null, true]) }}", root) == "[true,null]");
  CHECK(env.render(R"({{ intersect([[1, 2], {"a": 1}, [2, 1]], [{"a": 1}, [1, 2]]) }})", root) ==
        R"([[1,2],{"a": 1}])");
  CHECK(env.render("{% set shared = intersect(names, names) %}{{ shared }}|{{ names }}", root) ==
        R"(["Jeff","Seb","Peter","Tom"]|["Jeff","Seb","Peter","Tom"])");
  CHECK(env.render(R"({% set left = [3, 1, 3, 2] %}{% set right = [2, 3, 2] %}{% set shared = intersect(left, right) %}{{ shared }}|{{ left }}|{{ right }})", root) ==
        "[3,2]|[3,1,3,2]|[2,3,2]");
  CHECK(env.render("{{ intersect(intersect([3, 1, 2], [2, 3]), [2]) }}", root) == "[2]");
  CHECK(env.render("{{ intersect(1, []) }}", root) == "[]");
  CHECK(env.render("{{ intersect(2, [1, 2, 2]) }}", root) == "[2]");
  CHECK(env.render("{{ intersect(3, [1, 2]) }}", root) == "[]");
  CHECK(env.render("{{ intersect(2.5, [1, 2.5]) }}", root) == "[2.5]");
  CHECK(env.render("{{ intersect(true, [false, true]) }}", root) == "[true]");
  CHECK(env.render("{{ intersect(false, [true]) }}", root) == "[]");
  CHECK(env.render("{{ intersect(null, [null]) }}", root) == "[null]");
  CHECK(env.render("{{ intersect(null, []) }}", root) == "[]");
  CHECK(env.render(R"({{ intersect("Jeff", names) }})", root) == R"(["Jeff"])");
  CHECK(env.render(R"({{ intersect("Je", names) }})", root) == "[]");
  CHECK(env.render("{{ intersect(name, names) }}", root) == R"(["Jeff"])");
  CHECK(env.render("{{ name | intersect(names) | join(\",\") }}", root) == "Jeff");
  CHECK(env.render("{{ isArray(intersect(name, names)) }}", root) == "true");
  CHECK(env.render(R"({% set shared = intersect(name, names) %}{{ shared }}|{{ name }}|{{ names }})", root) ==
        R"(["Jeff"]|Jeff|["Jeff","Seb","Peter","Tom"])");
  CHECK_THROWS_WITH(env.render("{{ intersect([], \"abc\") }}", root),
                    "[inja.exception.render_error] (at 1:4) intersect() expects an array or scalar as the first argument and an array as the second");
  CHECK_THROWS_WITH(env.render("{{ intersect({}, []) }}", root),
                    "[inja.exception.render_error] (at 1:4) intersect() expects an array or scalar as the first argument and an array as the second");
  CHECK_THROWS_WITH(env.render("{{ intersect([], null) }}", root),
                    "[inja.exception.render_error] (at 1:4) intersect() expects an array or scalar as the first argument and an array as the second");
  CHECK_THROWS(env.render("{{ intersect(1, 1) }}", root));
  CHECK_THROWS(env.render("{{ intersect(missing, []) }}", root));
  CHECK_THROWS(env.render("{{ intersect([]) }}", root));
  CHECK_THROWS(env.render("{{ intersect([], [], []) }}", root));
}
