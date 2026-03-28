// Copyright (c) 2020 Pantor. All rights reserved.

#include "inja/environment.hpp"

#include "test-common.hpp"

TEST_CASE("source location") {
  std::string content = R""""(Lorem Ipsum
  Dolor
Amid
Set ().$
Try this

)"""";

  CHECK(inja::get_source_location(content, 0).line == 1);
  CHECK(inja::get_source_location(content, 0).column == 1);

  CHECK(inja::get_source_location(content, 10).line == 1);
  CHECK(inja::get_source_location(content, 10).column == 11);

  CHECK(inja::get_source_location(content, 25).line == 4);
  CHECK(inja::get_source_location(content, 25).column == 1);

  CHECK(inja::get_source_location(content, 29).line == 4);
  CHECK(inja::get_source_location(content, 29).column == 5);

  CHECK(inja::get_source_location(content, 43).line == 6);
  CHECK(inja::get_source_location(content, 43).column == 1);
}

TEST_CASE("copy environment") {
  inja::Tree data;
  inja::Environment env(data.rootref());
  env.add_callback("double", 1, [](inja::Arguments& args, inja::NodeRef additional_data) {
    int number = inja::node_to_int(args.at(0)).value();
    return additional_data.append_child() << 2 * number;
  });

  inja::Template t1 = env.parse("{{ double(2) }}");
  env.include_template("tpl", t1);
  std::string test_tpl = "{% include \"tpl\" %}";

  inja::Tree empty_data;
  empty_data.rootref() |= ryml::MAP;

  REQUIRE(env.render(test_tpl, empty_data.rootref()) == "4");

  inja::Environment copy(env);
  CHECK(copy.render(test_tpl, empty_data.rootref()) == "4");

  // overwrite template in source env
  const inja::Template t2 = env.parse("{{ double(4) }}");
  env.include_template("tpl", t2);
  REQUIRE(env.render(test_tpl, empty_data.rootref()) == "8");

  // template is unchanged in copy
  CHECK(copy.render(test_tpl, empty_data.rootref()) == "4");
}

TEST_CASE("loop frame sketch") {
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["outer"] |= ryml::SEQ;
  root["outer"].append_child() << 10;
  root["outer"].append_child() << 20;

  inja::detail::LoopFrameStackSketch frames;

  const auto outer = root["outer"];
  frames.push_array(outer.child(0), 0, outer.num_children());
  REQUIRE(frames.depth() == 1);
  REQUIRE(frames.current() != nullptr);
  CHECK(frames.current()->is_first());
  CHECK(!frames.current()->is_last());
  CHECK(inja::node_to_int(frames.current()->value).value() == 10);

  frames.push_array(outer.child(1), 1, outer.num_children());
  REQUIRE(frames.depth() == 2);
  REQUIRE(frames.current() != nullptr);
  CHECK(!frames.current()->is_first());
  CHECK(frames.current()->is_last());
  CHECK(inja::node_to_int(frames.current()->value).value() == 20);

  const auto* parent = frames.parent();
  REQUIRE(parent != nullptr);
  CHECK(parent->index == 0);
  CHECK(inja::node_to_int(parent->value).value() == 10);

  frames.pop();
  REQUIRE(frames.depth() == 1);
  CHECK(frames.parent() == nullptr);
}

TEST_CASE("loop parent integration") {
  inja::Environment env;
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["outer"] |= ryml::SEQ;

  auto first_row = root["outer"].append_child();
  first_row |= ryml::SEQ;
  first_row.append_child() << "a";
  first_row.append_child() << "b";

  auto second_row = root["outer"].append_child();
  second_row |= ryml::SEQ;
  second_row.append_child() << "c";

  const auto result = env.render(
      "{% for row in outer %}{% for cell in row %}{{ loop.parent.index }}-{{ loop.index }}:{{ cell }};{% endfor %}{% endfor %}",
      data.rootref());

  CHECK(result == "0-0:a;0-1:b;1-0:c;");
}
