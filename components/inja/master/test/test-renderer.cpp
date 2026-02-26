// Copyright (c) 2020 Pantor. All rights reserved.

#include "inja/environment.hpp"

#include "test-common.hpp"

TEST_CASE("types") {
  inja::Environment env;
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["name"] << "Peter";
  root["city"] << "Brunswick";
  root["age"] << "29";
  root["names"] |= ryml::SEQ;
  root["names"].append_child() << "Jeff";
  root["names"].append_child() << "Seb";
  root["brother"] |= ryml::MAP;
  root["brother"]["name"] << "Chris";
  root["brother"]["daughters"] |= ryml::SEQ;
  root["brother"]["daughters"].append_child() << "Maria";
  root["brother"]["daughters"].append_child() << "Helen";
  root["brother"]["daughter0"] |= ryml::MAP;
  root["brother"]["daughter0"]["name"] << "Maria";
  root["is_happy"] << "true";
  root["is_sad"] << "false";
  root["@name"] << "@name";
  root["$name"] << "$name";
  root["relatives"] |= ryml::MAP;
  root["relatives"]["mother"] << "Maria";
  root["relatives"]["brother"] << "Chris";
  root["relatives"]["sister"] << "Jenny";
  root["vars"] |= ryml::SEQ;
  root["vars"].append_child() << "2";
  root["vars"].append_child() << "3";
  root["vars"].append_child() << "4";
  root["vars"].append_child() << "0";
  root["vars"].append_child() << "-1";
  root["vars"].append_child() << "-2";
  root["vars"].append_child() << "-3";
  root["max_value"] << "18446744073709551615";

  SUBCASE("basic") {
    CHECK(env.render("", data.rootref()) == "");
    CHECK(env.render("Hello World!", data.rootref()) == "Hello World!");
    CHECK_THROWS_WITH(env.render("{{ }}", data.rootref()), "[inja.exception.render_error] (at 1:4) empty expression");
    CHECK_THROWS_WITH(env.render("{{" , data.rootref()), "[inja.exception.parser_error] (at 1:3) expected expression close, got '<eof>'");
  }

  SUBCASE("variables") {
    CHECK(env.render("Hello {{ name }}!", data.rootref()) == "Hello Peter!");
    CHECK(env.render("{{ name }}", data.rootref()) == "Peter");
    CHECK(env.render("{{name}}", data.rootref()) == "Peter");
    CHECK(env.render("{{ name }} is {{ age }} years old.", data.rootref()) == "Peter is 29 years old.");
    CHECK(env.render("Hello {{ name }}! I come from {{ city }}.", data.rootref()) == "Hello Peter! I come from Brunswick.");
    CHECK(env.render("Hello {{ names.1 }}!", data.rootref()) == "Hello Seb!");
    CHECK(env.render("Hello {{ brother.name }}!", data.rootref()) == "Hello Chris!");
    CHECK(env.render("Hello {{ brother.daughter0.name }}!", data.rootref()) == "Hello Maria!");
    CHECK(env.render("{{ \"{{ no_value }}\" }}", data.rootref()) == "{{ no_value }}");
    CHECK(env.render("{{ @name }}", data.rootref()) == "@name");
    CHECK(env.render("{{ $name }}", data.rootref()) == "$name");
    CHECK(env.render("{{max_value}}", data.rootref()) == "18446744073709551615");

    CHECK_THROWS_WITH(env.render("{{unknown}}", data.rootref()), "[inja.exception.render_error] (at 1:3) variable 'unknown' not found");
  }

  SUBCASE("comments") {
    CHECK(env.render("Hello{# This is a comment #}!", data.rootref()) == "Hello!");
    CHECK(env.render("{# --- #Todo --- #}", data.rootref()) == "");
  }

  SUBCASE("loops") {
    CHECK(env.render("{% for name in names %}a{% endfor %}", data.rootref()) == "aa");
    CHECK(env.render("Hello {% for name in names %}{{ name }} {% endfor %}!", data.rootref()) == "Hello Jeff Seb !");
    CHECK(env.render("Hello {% for name in names %}{{ loop.index }}: {{ name }}, {% endfor %}!", data.rootref()) == "Hello 0: Jeff, 1: Seb, !");
    CHECK(env.render("{% for type, name in relatives %}{{ loop.index1 }}: {{ type }}: {{ name }}{% if loop.is_last == "
                     "false %}, {% endif %}{% endfor %}",
                     data.rootref()) == "1: brother: Chris, 2: mother: Maria, 3: sister: Jenny");
    CHECK(env.render("{% for v in vars %}{% if v > 0 %}+{% endif %}{% endfor %}", data.rootref()) == "+++");
    CHECK(env.render("{% for name in names %}{{ loop.index }}: {{ name }}{% if not loop.is_last %}, {% endif %}{% endfor %}!", data.rootref()) == "0: Jeff, 1: Seb!");
    CHECK(env.render("{% for name in names %}{{ loop.index }}: {{ name }}{% if loop.is_last == false %}, {% endif %}{% "
                     "endfor %}!",
                     data.rootref()) == "0: Jeff, 1: Seb!");

    CHECK(env.render("{% for name in [] %}a{% endfor %}", data.rootref()) == "");

    CHECK_THROWS_WITH(env.render("{% for name ins names %}a{% endfor %}", data.rootref()), "[inja.exception.parser_error] (at 1:13) expected 'in', got 'ins'");
    CHECK_THROWS_WITH(env.render("{% for name in empty_loop %}a{% endfor %}", data.rootref()), "[inja.exception.render_error] (at 1:16) variable 'empty_loop' not found");
    // CHECK_THROWS_WITH( env.render("{% for name in relatives %}{{ name }}{% endfor %}", data),
    // "[inja.exception.json_error] [json.exception.type_error.302] type must be array, but is object" );
  }

  SUBCASE("nested loops") {
    inja::Tree ldata = env.load_json(R""""(
{ "outer" : [
    { "inner" : [
        { "in2" : [ 1, 2 ] },
        { "in2" : []},
        { "in2" : []}
        ]
    },
    { "inner" : [] },
    { "inner" : [
        { "in2" : [ 3, 4 ] },
        { "in2" : [ 5, 6 ] }
        ]
    }
    ]
}
)"""");

    CHECK(env.render(R""""(
{% for o in outer %}{% for i in o.inner %}{{loop.parent.index}}:{{loop.index}}::{{loop.parent.is_last}}
{% for ii in i.in2%}{{ii}},{%endfor%}
{%endfor%}{%endfor%}
)"""",
                     ldata.rootref()) == "\n0:0::false\n1,2,\n0:1::false\n\n0:2::false\n\n2:0::true\n3,4,\n2:1::true\n5,6,\n\n");
  }

  SUBCASE("conditionals") {
    CHECK(env.render("{% if is_happy %}{% endif %}", data.rootref()) == "");
    CHECK(env.render("{% if is_happy %}Yeah!{% endif %}", data.rootref()) == "Yeah!");
    CHECK(env.render("{% if is_sad %}Yeah!{% endif %}", data.rootref()) == "");
    CHECK(env.render("{% if is_sad %}Yeah!{% else %}Nooo...{% endif %}", data.rootref()) == "Nooo...");
    CHECK(env.render("{% if age == 29 %}Right{% else %}Wrong{% endif %}", data.rootref()) == "Right");
    CHECK(env.render("{% if age > 29 %}Right{% else %}Wrong{% endif %}", data.rootref()) == "Wrong");
    CHECK(env.render("{% if age <= 29 %}Right{% else %}Wrong{% endif %}", data.rootref()) == "Right");
    CHECK(env.render("{% if age != 28 %}Right{% else %}Wrong{% endif %}", data.rootref()) == "Right");
    CHECK(env.render("{% if age >= 30 %}Right{% else %}Wrong{% endif %}", data.rootref()) == "Wrong");
    CHECK(env.render("{% if age in [28, 29, 30] %}True{% endif %}", data.rootref()) == "True");
    CHECK(env.render("{% if age == 28 %}28{% else if age == 29 %}29{% endif %}", data.rootref()) == "29");
    CHECK(env.render("{% if age == 26 %}26{% else if age == 27 %}27{% else if age == 28 %}28{% else %}29{% endif %}", data.rootref()) == "29");
    CHECK(env.render("{% if age == 25 %}+{% endif %}{% if age == 29 %}+{% else %}-{% endif %}", data.rootref()) == "+");

    CHECK_THROWS_WITH(env.render("{% if is_happy %}{% if is_happy %}{% endif %}", data.rootref()), "[inja.exception.parser_error] (at 1:46) unmatched if");
    CHECK_THROWS_WITH(env.render("{% if is_happy %}{% else if is_happy %}{% end if %}", data.rootref()),
                      "[inja.exception.parser_error] (at 1:43) expected statement, got 'end'");
  }

  SUBCASE("set statements") {
    CHECK(env.render("{% set predefined=true %}{% if predefined %}a{% endif %}", data.rootref()) == "a");
    CHECK(env.render("{% set predefined=false %}{% if predefined %}a{% endif %}", data.rootref()) == "");
    CHECK(env.render("{% set age=30 %}{{age}}", data.rootref()) == "30");
    CHECK(env.render("{% set age=2+3 %}{{age}}", data.rootref()) == "5");
    CHECK(env.render("{% set predefined.value=1 %}{% if existsIn(predefined, \"value\") %}{{predefined.value}}{% endif %}", data.rootref()) == "1");
    CHECK(env.render("{% set brother.name=\"Bob\" %}{{brother.name}}", data.rootref()) == "Bob");
    CHECK_THROWS_WITH(env.render("{% if predefined %}{% endif %}", data.rootref()), "[inja.exception.render_error] (at 1:7) variable 'predefined' not found");
    CHECK(env.render("{{age}}", data.rootref()) == "29");
    CHECK(env.render("{{brother.name}}", data.rootref()) == "Chris");
  }

  SUBCASE("short circuit evaluation") {
    CHECK(env.render("{% if 0 and undefined %}do{% else %}nothing{% endif %}", data.rootref()) == "nothing");
    CHECK_THROWS_WITH(env.render("{% if 1 and undefined %}do{% else %}nothing{% endif %}", data.rootref()),
                      "[inja.exception.render_error] (at 1:13) variable 'undefined' not found");
  }

  SUBCASE("line statements") {
    CHECK(env.render(R""""(## if is_happy
Yeah!
## endif)"""",
                     data.rootref()) == R""""(Yeah!
)"""");

    CHECK(env.render(R""""(## if is_happy
## if is_happy
Yeah!
## endif
## endif    )"""",
                     data.rootref()) == R""""(Yeah!
)"""");
  }

  SUBCASE("pipe syntax") {
    CHECK(env.render("{{ brother.name | upper }}", data.rootref()) == "CHRIS");
    CHECK(env.render("{{ brother.name | upper | lower }}", data.rootref()) == "chris");
    CHECK(env.render("{{ [\"C\", \"A\", \"B\"] | sort | join(\",\") }}", data.rootref()) == "A,B,C");
  }
}

TEST_CASE("templates") {
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["name"] << "Peter";
  root["city"] << "Brunswick";
  root["is_happy"] << "true";

  SUBCASE("reuse") {
    inja::Environment env;
    const inja::Template temp = env.parse("{% if is_happy %}{{ name }}{% else %}{{ city }}{% endif %}");

    CHECK(env.render(temp, data.rootref()) == "Peter");

    data.rootref()["is_happy"] << "false";

    CHECK(env.render(temp, data.rootref()) == "Brunswick");
  }

  SUBCASE("include") {
    inja::Environment env;
    const inja::Template t1 = env.parse("Hello {{ name }}");
    env.include_template("greeting", t1);

    const inja::Template t2 = env.parse("{% include \"greeting\" %}!");
    CHECK(env.render(t2, data.rootref()) == "Hello Peter!");
    CHECK_THROWS_WITH(env.parse("{% include \"does-not-exist\" %}!"), "[inja.exception.file_error] failed accessing file at 'does-not-exist'");

    CHECK_THROWS_WITH(env.parse("{% include does-not-exist %}!"), "[inja.exception.parser_error] (at 1:12) expected string, got 'does-not-exist'");
  }

  SUBCASE("include-callback") {
    inja::Environment env;

    CHECK_THROWS_WITH(env.parse("{% include \"does-not-exist\" %}!"), "[inja.exception.file_error] failed accessing file at 'does-not-exist'");

    env.set_search_included_templates_in_files(false);
    env.set_include_callback([&env](const std::filesystem::path&, const std::string&) { return env.parse("Hello {{ name }}"); });

    const inja::Template t1 = env.parse("{% include \"greeting\" %}!");
    CHECK(env.render(t1, data.rootref()) == "Hello Peter!");

    env.set_search_included_templates_in_files(true);
    env.set_include_callback([&env](const std::filesystem::path&, const std::string& name) { return env.parse("Bye " + name); });

    const inja::Template t2 = env.parse("{% include \"Jeff\" %}!");
    CHECK(env.render(t2, data.rootref()) == "Bye Jeff!");
  }

  SUBCASE("include-in-loop") {
    inja::Tree loop_data;
    auto lroot = loop_data.rootref();
    lroot |= ryml::MAP;
    lroot["cities"] |= ryml::SEQ;
    auto city0 = lroot["cities"].append_child();
    city0 |= ryml::MAP;
    city0["name"] << "Munich";
    auto city1 = lroot["cities"].append_child();
    city1 |= ryml::MAP;
    city1["name"] << "New York";

    inja::Environment env;
    env.include_template("city.tpl", env.parse("{{ loop.index }}:{{ city.name }};"));

    CHECK(env.render("{% for city in cities %}{% include \"city.tpl\" %}{% endfor %}", loop_data.rootref()) == "0:Munich;1:New York;");
  }

  SUBCASE("count variables") {
    inja::Environment env;
    const inja::Template t1 = env.parse("Hello {{ name }}");
    const inja::Template t2 = env.parse("{% if is_happy %}{{ name }}{% else %}{{ city }}{% endif %}");
    const inja::Template t3 = env.parse("{% if at(name, test) %}{{ name }}{% else %}{{ city }}{{ upper(city) }}{% endif %}");

    CHECK(t1.count_variables() == 1);
    CHECK(t2.count_variables() == 3);
    CHECK(t3.count_variables() == 5);
  }

  SUBCASE("whitespace control") {
    inja::Environment env;
    CHECK(env.render("{% if is_happy %}{{ name }}{% endif %}", data.rootref()) == "Peter");
    CHECK(env.render("   {% if is_happy %}{{ name }}{% endif %}   ", data.rootref()) == "   Peter   ");
    CHECK(env.render("   {% if is_happy %}{{ name }}{% endif %}\n ", data.rootref()) == "   Peter\n ");
    CHECK(env.render("Test\n   {%- if is_happy %}{{ name }}{% endif %}   ", data.rootref()) == "Test\nPeter   ");
    CHECK(env.render("   {%+ if is_happy %}{{ name }}{% endif %}", data.rootref()) == "   Peter");
    CHECK(env.render("   {%- if is_happy %}{{ name }}{% endif -%}   \n   ", data.rootref()) == "Peter");

    CHECK(env.render("   {{- name -}}   \n   ", data.rootref()) == "Peter");
    CHECK(env.render("Test\n   {{- name }}   ", data.rootref()) == "Test\nPeter   ");
    CHECK(env.render("   {{ name }}\n ", data.rootref()) == "   Peter\n ");
    CHECK(env.render("{{ name }}{# name -#}    !", data.rootref()) == "Peter!");
    CHECK(env.render("   {#- name -#}    !", data.rootref()) == "!");

    // Nothing will be stripped if there are other characters before the start of the block.
    CHECK(env.render(".  {%- if is_happy %}{{ name }}{% endif -%}\n", data.rootref()) == ".  Peter");
    CHECK(env.render(".  {#- comment -#}\n.", data.rootref()) == ".  .");

    env.set_lstrip_blocks(true);
    CHECK(env.render("Hello {{ name }}!", data.rootref()) == "Hello Peter!");
    CHECK(env.render("   {% if is_happy %}{{ name }}{% endif %}", data.rootref()) == "Peter");
    CHECK(env.render("   {% if is_happy %}{{ name }}{% endif %}   ", data.rootref()) == "Peter   ");
    CHECK(env.render("   {% if is_happy %}{{ name }}{% endif -%}   ", data.rootref()) == "Peter");
    CHECK(env.render("   {%+ if is_happy %}{{ name }}{% endif %}", data.rootref()) == "   Peter");
    CHECK(env.render("\n   {%+ if is_happy %}{{ name }}{% endif -%}   ", data.rootref()) == "\n   Peter");
    CHECK(env.render("{% if is_happy %}{{ name }}{% endif %}\n", data.rootref()) == "Peter\n");
    CHECK(env.render("   {# comment #}", data.rootref()) == "");

    env.set_trim_blocks(true);
    CHECK(env.render("{% if is_happy %}{{ name }}{% endif %}", data.rootref()) == "Peter");
    CHECK(env.render("{% if is_happy %}{{ name }}{% endif %}\n", data.rootref()) == "Peter");
    CHECK(env.render("{% if is_happy %}{{ name }}{% endif %}   \n.", data.rootref()) == "Peter.");
    CHECK(env.render("{%- if is_happy %}{{ name }}{% endif -%}   \n.", data.rootref()) == "Peter.");
    CHECK(env.render("   {# comment #}   \n.", data.rootref()) == ".");
  }
}

TEST_CASE("other syntax") {
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["name"] << "Peter";
  root["city"] << "Brunswick";
  root["age"] << "29";
  root["names"] |= ryml::SEQ;
  root["names"].append_child() << "Jeff";
  root["names"].append_child() << "Seb";
  root["brother"] |= ryml::MAP;
  root["brother"]["name"] << "Chris";
  root["brother"]["daughters"] |= ryml::SEQ;
  root["brother"]["daughters"].append_child() << "Maria";
  root["brother"]["daughters"].append_child() << "Helen";
  root["brother"]["daughter0"] |= ryml::MAP;
  root["brother"]["daughter0"]["name"] << "Maria";
  root["is_happy"] << "true";

  SUBCASE("other expression syntax") {
    inja::Environment env;

    CHECK(env.render("Hello {{ name }}!", data.rootref()) == "Hello Peter!");

    env.set_expression("(&", "&)");

    CHECK(env.render("Hello {{ name }}!", data.rootref()) == "Hello {{ name }}!");
    CHECK(env.render("Hello (& name &)!", data.rootref()) == "Hello Peter!");
  }

  SUBCASE("other comment syntax") {
    inja::Environment env;
    env.set_comment("(&", "&)");

    CHECK(env.render("Hello {# Test #}", data.rootref()) == "Hello {# Test #}");
    CHECK(env.render("Hello (& Test &)", data.rootref()) == "Hello ");
  }

  SUBCASE("multiple changes") {
    inja::Environment env;
    env.set_line_statement("$$");
    env.set_expression("<%", "%>");

    std::string string_template = R""""(Hello <%name%>
$$ if name == "Peter"
    You really are <%name%>
$$ endif
)"""";

    CHECK(env.render(string_template, data.rootref()) == "Hello Peter\n    You really are Peter\n");
  }
}
