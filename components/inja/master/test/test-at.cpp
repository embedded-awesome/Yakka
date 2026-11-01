#include <filesystem>

#ifdef INJA_TEST_SINGLE_HEADER
#include "inja/inja.hpp"
#else
#include "inja/environment.hpp"
#endif
#include "test-common.hpp"

TEST_CASE("at paths") {
  inja::Environment env;
  auto values = ryml::parse_in_arena(R"(
toolchain:
  name: outer
data:
  toolchain:
    name: xcode
    targets: [macos, ios]
  variants: [':/toolchain/name', [stuff, things]]
  a.b: dotted
  a/b: slashed
  /literal: leading-slash
  :literal: leading-colon
path: :/toolchain/name
)");
  const auto root = values.crootref();

  CHECK(env.render(R"({{ at(data, ":/toolchain/name") }})", root) == "xcode");
  CHECK(env.render("{{ at(data, path) }}", root) == "xcode");
  CHECK(env.render("{{ data | at(path) }}", root) == "xcode");
  CHECK(env.render(R"({{ at(data, ":/toolchain/targets/1") }})", root) == "ios");
  CHECK(env.render(R"({{ at(data.toolchain.targets, ":/0") }})", root) == "macos");
  CHECK(env.render(R"({{ at(at(data, ":/"), ":/toolchain/name") }})", root) == "xcode");
  CHECK(env.render(R"({% for variant in data.variants %}{% if variant | startswith(":/") %}{{ at(data, variant) }}{% else %}array{% endif %};{% endfor %})", root) ==
        "xcode;array;");

  CHECK(env.render(R"({{ at(data.toolchain, "name") }})", root) == "xcode");
  CHECK(env.render("{{ at(data.toolchain.targets, 1) }}", root) == "ios");
  CHECK(env.render(R"({{ at(data, "a.b") }}|{{ at(data, "a/b") }}|{{ at(data, "/literal") }}|{{ at(data, ":literal") }})", root) ==
        "dotted|slashed|leading-slash|leading-colon");

  CHECK(env.render(R"({{ default(at(data, ":/missing/name"), "missing") }})", root) == "missing");
  CHECK(env.render(R"({{ default(at(data, ":/toolchain/targets/9"), "missing") }})", root) == "missing");
  CHECK(env.render(R"({{ default(at(data, ":/toolchain/targets/-1"), "missing") }})", root) == "missing");
  CHECK(env.render(R"({{ default(at(data, ":/toolchain/targets/nope"), "missing") }})", root) == "missing");
  CHECK(env.render(R"({{ default(at(data, ":/toolchain/name/child"), "missing") }})", root) == "missing");
  CHECK(env.render(R"({{ at(":/toolchain/name") }}|{{ at("/toolchain/name") }}|{{ at("toolchain/name") }})", root) ==
        "outer|outer|outer");
  CHECK(env.render("{{ at(path) }}|{{ path | at }}", root) == "outer|outer");
  CHECK(env.render(R"({% set path = ":/data/toolchain/targets/1" %}{{ at(path) }})", root) == "ios");
  CHECK(env.render(R"({% set toolchain = {"name": "local"} %}{{ at(":/toolchain/name") }}|{{ toolchain.name }})", root) ==
        "outer|local");
  CHECK(env.render(R"({{ default(at(":/missing"), "missing") }}|{{ default(at(""), "empty") }}|{{ default(at(":/data/toolchain/targets/9"), "missing") }})", root) ==
        "missing|empty|missing");
  CHECK(env.render(R"({% set local = {"nested": {"value": "local"}} %}{{ default(at(":/local/nested/value"), "missing") }})", root) ==
        "missing");
  CHECK(env.render(R"({{ at(at(":/"), ":/toolchain/name") }})", root) == "outer");
  CHECK(env.render(R"({% set local = {"nested": {"value": "local"}} %}{{ at(":/local", ":/nested/value") }}|{{ at(":/local/nested", "value") }})", root) ==
        "local|local");
  CHECK(env.render(R"({% set local = {"nested": {"value": "local"}} %}{{ at("/local/nested", "value") }})", root) == "local");
  CHECK(env.render(R"({% set local = {"items": ["first", "second"]} %}{{ at(local, ":/items/1") }}|{{ at(":/local/items", 0) }})", root) ==
        "second|first");
}
