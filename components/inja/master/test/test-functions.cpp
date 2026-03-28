// Copyright (c) 2020 Pantor. All rights reserved.

#include "inja/environment.hpp"
#include "inja/utils.hpp"
#include "test-common.hpp"

TEST_CASE("functions") {
  inja::Environment env;

  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["name"] << "Peter";
  root["city"] << "New York";
  root["names"] |= ryml::SEQ;
  root["names"].append_child() << "Jeff";
  root["names"].append_child() << "Seb";
  root["names"].append_child() << "Peter";
  root["names"].append_child() << "Tom";
  root["temperature"] << "25.6789";
  root["brother"] |= ryml::MAP;
  root["brother"]["name"] << "Chris";
  root["brother"]["daughters"] |= ryml::SEQ;
  root["brother"]["daughters"].append_child() << "Maria";
  root["brother"]["daughters"].append_child() << "Helen";
  root["property"] << "name";
  root["age"] << "29";
  root["u64"] << "18446744073709551615";
  root["i"] << "1";
  root["is_happy"] << "true";
  root["is_sad"] << "false";
  root["vars"] |= ryml::SEQ;
  root["vars"].append_child() << "2";
  root["vars"].append_child() << "3";
  root["vars"].append_child() << "4";
  root["vars"].append_child() << "0";
  root["vars"].append_child() << "-1";
  root["vars"].append_child() << "-2";
  root["vars"].append_child() << "-3";

  SUBCASE("math") {
    CHECK(env.render("{{ 1e3 }}", data.rootref()) == "1000.0");
    CHECK(env.render("{{ 1e+3 }}", data.rootref()) == "1000.0");
    CHECK(env.render("{{ 1e-3 }}", data.rootref()) == "0.001");
    CHECK(env.render("{{1.0}}", data.rootref()) == "1.0");
    CHECK(env.render("{{ -1 }}", data.rootref()) == "-1");
    CHECK(env.render("{{1e0}}", data.rootref()) == "1.0");

    CHECK(env.render("{{ 1 + 1 }}", data.rootref()) == "2");
    CHECK(env.render("{{ 1+3 }}", data.rootref()) == "4");
    CHECK(env.render("{{ 1-3 }}", data.rootref()) == "-2");
    CHECK(env.render("{{ 3 - 21 }}", data.rootref()) == "-18");
    CHECK(env.render("{{ 1 + 1 * 3 }}", data.rootref()) == "4");
    CHECK(env.render("{{ (1 + 1) * 3 }}", data.rootref()) == "6");
    CHECK(env.render("{{ 5 / 2 }}", data.rootref()) == "2.5");
    CHECK(env.render("{{ 5^3 }}", data.rootref()) == "125");
    CHECK(env.render("{{ 5 + 12 + 4 * (4 - (1 + 1))^2 - 75 * 1 }}", data.rootref()) == "-42");

    CHECK_THROWS_WITH(env.render("{{ +1 }}", data.rootref()), "[inja.exception.parser_error] (at 1:7) too few arguments");
    CHECK_THROWS_WITH(env.render("{{ 1 + }}", data.rootref()), "[inja.exception.parser_error] (at 1:8) too few arguments");
  }

  SUBCASE("upper") {
    CHECK(env.render("{{ upper(name) }}", data.rootref()) == "PETER");
    CHECK(env.render("{{ upper(  name  ) }}", data.rootref()) == "PETER");
    CHECK(env.render("{{ upper(city) }}", data.rootref()) == "NEW YORK");
    CHECK(env.render("{{ upper(upper(name)) }}", data.rootref()) == "PETER");

    // CHECK_THROWS_WITH( env.render("{{ upper(5) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be string, but is number" ); CHECK_THROWS_WITH( env.render("{{
    // upper(true) }}", data), "[inja.exception.json_error] [json.exception.type_error.302] type must be string, but is
    // boolean" );
  }

  SUBCASE("lower") {
    CHECK(env.render("{{ lower(name) }}", data.rootref()) == "peter");
    CHECK(env.render("{{ lower(city) }}", data.rootref()) == "new york");
    // CHECK_THROWS_WITH( env.render("{{ lower(5.45) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be string, but is number" );
  }

  SUBCASE("capitalize") {
    CHECK(env.render("{{ capitalize(name) }}", data.rootref()) == "Peter");
    CHECK(env.render("{{ capitalize(city) }}", data.rootref()) == "New york");
  }

  SUBCASE("range") {
    CHECK(env.render("{{ range(2) }}", data.rootref()) == "[0,1]");
    CHECK(env.render("{{ range(4) }}", data.rootref()) == "[0,1,2,3]");
    // CHECK_THROWS_WITH( env.render("{{ range(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be number, but is string" );
  }

  SUBCASE("length") {
    CHECK(env.render("{{ length(names) }}", data.rootref()) == "4"); // Length of array
    CHECK(env.render("{{ length(name) }}", data.rootref()) == "5");  // Length of string
                                                           // CHECK_THROWS_WITH( env.render("{{ length(5) }}", data), "[inja.exception.json_error]
                                                           // [json.exception.type_error.302] type must be array, but is number" );
  }

  SUBCASE("sort") {
    CHECK(env.render("{{ sort([3, 2, 1]) }}", data.rootref()) == "[1,2,3]");
    CHECK(env.render("{{ sort([\"bob\", \"charlie\", \"alice\"]) }}", data.rootref()) == "[\"alice\",\"bob\",\"charlie\"]");
    // CHECK_THROWS_WITH( env.render("{{ sort(5) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is number" );
  }

  SUBCASE("at") {
    CHECK(env.render("{{ at(names, 0) }}", data.rootref()) == "Jeff");
    CHECK(env.render("{{ at(names, i) }}", data.rootref()) == "Seb");
    CHECK(env.render("{{ at(brother, \"name\") }}", data.rootref()) == "Chris");
    CHECK(env.render("{{ at(at(brother, \"daughters\"), 0) }}", data.rootref()) == "Maria");
    // CHECK(env.render("{{ at(names, 45) }}", data) == "Jeff");
  }

  SUBCASE("first") {
    CHECK(env.render("{{ first(names) }}", data.rootref()) == "Jeff");
    // CHECK_THROWS_WITH( env.render("{{ first(5) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is number" );
  }

  SUBCASE("last") {
    CHECK(env.render("{{ last(names) }}", data.rootref()) == "Tom");
    // CHECK_THROWS_WITH( env.render("{{ last(5) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is number" );
  }

  SUBCASE("replace") {
    CHECK(env.render("{{ replace(name, \"e\", \"3\") }}", data.rootref()) == "P3t3r");
    CHECK(env.render("{{ replace(city, \" \", \"_\") }}", data.rootref()) == "New_York");
  }

  SUBCASE("round") {
    CHECK(env.render("{{ round(4, 0) }}", data.rootref()) == "4");
    CHECK(env.render("{{ round(temperature, 2) }}", data.rootref()) == "25.68");
    // CHECK_THROWS_WITH( env.render("{{ round(name, 2) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be number, but is string" );
  }

  SUBCASE("divisibleBy") {
    CHECK(env.render("{{ divisibleBy(50, 5) }}", data.rootref()) == "true");
    CHECK(env.render("{{ divisibleBy(12, 3) }}", data.rootref()) == "true");
    CHECK(env.render("{{ divisibleBy(11, 3) }}", data.rootref()) == "false");
    // CHECK_THROWS_WITH( env.render("{{ divisibleBy(name, 2) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be number, but is string" );
  }

  SUBCASE("odd") {
    CHECK(env.render("{{ odd(11) }}", data.rootref()) == "true");
    CHECK(env.render("{{ odd(12) }}", data.rootref()) == "false");
    // CHECK_THROWS_WITH( env.render("{{ odd(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be number, but is string" );
  }

  SUBCASE("even") {
    CHECK(env.render("{{ even(11) }}", data.rootref()) == "false");
    CHECK(env.render("{{ even(12) }}", data.rootref()) == "true");
    // CHECK_THROWS_WITH( env.render("{{ even(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be number, but is string" );
  }

  SUBCASE("max") {
    CHECK(env.render("{{ max([1, 2, 3]) }}", data.rootref()) == "3");
    CHECK(env.render("{{ max([-5.2, 100.2, 2.4]) }}", data.rootref()) == "100.2");
    // CHECK_THROWS_WITH( env.render("{{ max(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is string" );
  }

  SUBCASE("min") {
    CHECK(env.render("{{ min([1, 2, 3]) }}", data.rootref()) == "1");
    CHECK(env.render("{{ min([-5.2, 100.2, 2.4]) }}", data.rootref()) == "-5.2");
    // CHECK_THROWS_WITH( env.render("{{ min(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is string" );
  }

  SUBCASE("float") {
    CHECK(env.render("{{ float(\"2.2\") == 2.2 }}", data.rootref()) == "true");
    CHECK(env.render("{{ float(\"-1.25\") == -1.25 }}", data.rootref()) == "true");
    // CHECK_THROWS_WITH( env.render("{{ max(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is string" );
  }

  SUBCASE("int") {
    CHECK(env.render("{{ int(\"2\") == 2 }}", data.rootref()) == "true");
    CHECK(env.render("{{ int(\"-1.25\") == -1 }}", data.rootref()) == "true");
    // CHECK_THROWS_WITH( env.render("{{ max(name) }}", data), "[inja.exception.json_error]
    // [json.exception.type_error.302] type must be array, but is string" );
  }

  SUBCASE("default") {
    CHECK(env.render("{{ default(11, 0) }}", data.rootref()) == "11");
    CHECK(env.render("{{ default(nothing, 0) }}", data.rootref()) == "0");
    CHECK(env.render("{{ default(name, \"nobody\") }}", data.rootref()) == "Peter");
    CHECK(env.render("{{ default(surname, \"nobody\") }}", data.rootref()) == "nobody");
    CHECK(env.render("{{ default(surname, \"{{ surname }}\") }}", data.rootref()) == "{{ surname }}");
    CHECK_THROWS_WITH(env.render("{{ default(surname, lastname) }}", data.rootref()), "[inja.exception.render_error] (at 1:21) variable 'lastname' not found");
  }

  SUBCASE("exists") {
    CHECK(env.render("{{ exists(\"name\") }}", data.rootref()) == "true");
    CHECK(env.render("{{ exists(\"zipcode\") }}", data.rootref()) == "false");
    CHECK(env.render("{{ exists(name) }}", data.rootref()) == "false");
    CHECK(env.render("{{ exists(property) }}", data.rootref()) == "true");

    // CHECK(env.render("{{ exists(\"keywords\") and length(keywords) > 0 }}", data) == "false");
  }

  SUBCASE("existsIn") {
    CHECK(env.render("{{ existsIn(brother, \"name\") }}", data.rootref()) == "true");
    CHECK(env.render("{{ existsIn(brother, \"parents\") }}", data.rootref()) == "false");
    CHECK(env.render("{{ existsIn(brother, property) }}", data.rootref()) == "true");
    CHECK(env.render("{{ existsIn(brother, name) }}", data.rootref()) == "false");
    CHECK_THROWS_WITH(env.render("{{ existsIn(sister, \"lastname\") }}", data.rootref()), "[inja.exception.render_error] (at 1:13) variable 'sister' not found");
    CHECK_THROWS_WITH(env.render("{{ existsIn(brother, sister) }}", data.rootref()), "[inja.exception.render_error] (at 1:22) variable 'sister' not found");
  }

  SUBCASE("join") {
    CHECK(env.render("{{ join(names, \" | \") }}", data.rootref()) == "Jeff | Seb | Peter | Tom");
    CHECK(env.render("{{ join(vars, \", \") }}", data.rootref()) == "2, 3, 4, 0, -1, -2, -3");
  }

  SUBCASE("isType") {
    CHECK(env.render("{{ isBoolean(is_happy) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isBoolean(vars) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isNumber(age) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isNumber(name) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isInteger(age) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isInteger(is_happy) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isFloat(temperature) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isFloat(age) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isObject(brother) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isObject(vars) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isArray(vars) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isArray(name) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isString(name) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isString(names) }}", data.rootref()) == "false");
    CHECK(env.render("{{ isInteger(u64) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isNumber(u64) }}", data.rootref()) == "true");
    CHECK(env.render("{{ isString(u64) }}", data.rootref()) == "false");
  }
}

TEST_CASE("assignments") {
  inja::Environment env;
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["age"] << "28";

  CHECK(env.render("{% set new_hour=23 %}{{ new_hour }}", data.rootref()) == "23");
  CHECK(env.render("{% set time.start=18 %}{{ time.start }}pm", data.rootref()) == "18pm");
  CHECK(env.render("{% set v1 = \"a\" %}{% set v2 = \"b\" %}{% set var = v1 + v2 %}{{ var }}", data.rootref()) == "ab");
  CHECK(env.render("{% set flag=true %}{{ isBoolean(flag) }}", data.rootref()) == "true");
  CHECK(env.render("{% set big=18446744073709551615 %}{{ isInteger(big) }}", data.rootref()) == "true");
}

TEST_CASE("callbacks") {
  inja::Environment env;
  inja::Tree data;
  auto root = data.rootref();
  root |= ryml::MAP;
  root["age"] << "28";

  env.add_callback("double", 1, [](inja::Arguments& args, inja::NodeRef additional_data) {
    int number = inja::node_to_int(args.at(0)).value();
    return additional_data().append_child() << 2 * number;
  });

  env.add_callback("half", 1, [](inja::Arguments args, inja::NodeRef additional_data) {
    int number = inja::node_to_int(args.at(0)).value();
    return additional_data().append_child() << number / 2;
  });

  std::string greet = "Hello";
  env.add_callback("double-greetings", 0, [greet](inja::Arguments, inja::NodeRef additional_data) { 
    return additional_data().append_child() << greet + " " + greet + "!";
  });

  env.add_callback("multiply", 2, [](inja::Arguments args, inja::NodeRef additional_data) {
    double number1 = inja::node_to_double(args.at(0)).value();
    double number2 = inja::node_to_double(args.at(1)).value();
    return additional_data().append_child() << number1 * number2;
  });

  env.add_callback("multiply", 3, [](inja::Arguments args, inja::NodeRef additional_data) {
    double number1 = inja::node_to_double(args.at(0)).value();
    double number2 = inja::node_to_double(args.at(1)).value();
    double number3 = inja::node_to_double(args.at(2)).value();
    return additional_data().append_child() << number1 * number2 * number3;
  });

  env.add_callback("length", 1, [](inja::Arguments args, inja::NodeRef additional_data) {
    auto str = inja::node_to_string(args.at(0));
    return additional_data().append_child() << str.length();
  });

  env.add_void_callback("log", 1, [](inja::Arguments, inja::Tree&) {

  });

  env.add_callback("multiply", 0, [](inja::Arguments, inja::NodeRef additional_data) { 
    return additional_data().append_child() << 1.0;
  });

  CHECK(env.render("{{ double(age) }}", data.rootref()) == "56");
  CHECK(env.render("{{ half(age) }}", data.rootref()) == "14");
  CHECK(env.render("{{ log(age) }}", data.rootref()) == "");
  CHECK(env.render("{{ double-greetings }}", data.rootref()) == "Hello Hello!");
  CHECK(env.render("{{ double-greetings() }}", data.rootref()) == "Hello Hello!");
  CHECK(env.render("{{ multiply(4, 5) }}", data.rootref()) == "20");
  CHECK(env.render("{{ multiply(4, 2 + 3) }}", data.rootref()) == "20");
  CHECK(env.render("{{ multiply(2 + 2, 6) }}", data.rootref()) == "24");
  CHECK(env.render("{{ multiply(length(\"tester\"), 5) }}", data.rootref()) == "30");
  CHECK(env.render("{{ multiply(5, length(\"t\")) }}", data.rootref()) == "5");
  CHECK(env.render("{{ multiply(3, 4, 5) }}", data.rootref()) == "60");
  CHECK(env.render("{{ multiply }}", data.rootref()) == "1");

  SUBCASE("Variadic") {
    env.add_callback("argmax", [](inja::Arguments& args, inja::NodeRef additional_data) {
      auto result = std::max_element(args.begin(), args.end(), [](const inja::ConstNodeRef& a, const inja::ConstNodeRef& b) { 
        return inja::node_to_double(a) < inja::node_to_double(b);
      });
      return additional_data().append_child() << std::distance(args.begin(), result);
    });

    CHECK(env.render("{{ argmax(4, 2, 6) }}", data.rootref()) == "2");
    CHECK(env.render("{{ argmax(0, 2, 6, 8, 3) }}", data.rootref()) == "3");
  }
}

TEST_CASE("combinations") {
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
  root["names"].append_child() << "Chris";
  root["brother"] |= ryml::MAP;
  root["brother"]["name"] << "Chris";
  root["brother"]["daughters"] |= ryml::SEQ;
  root["brother"]["daughters"].append_child() << "Maria";
  root["brother"]["daughters"].append_child() << "Helen";
  root["brother"]["daughter0"] |= ryml::MAP;
  root["brother"]["daughter0"]["name"] << "Maria";
  root["is_happy"] << "true";
  root["list_of_objects"] |= ryml::SEQ;
  auto obj0 = root["list_of_objects"].append_child();
  obj0 |= ryml::MAP;
  obj0["a"] << "2";
  auto obj1 = root["list_of_objects"].append_child();
  obj1 |= ryml::MAP;
  obj1["b"] << "3";
  auto obj2 = root["list_of_objects"].append_child();
  obj2 |= ryml::MAP;
  obj2["c"] << "4";
  auto obj3 = root["list_of_objects"].append_child();
  obj3 |= ryml::MAP;
  obj3["d"] << "5";

  CHECK(env.render("{% if upper(\"Peter\") == \"PETER\" %}TRUE{% endif %}", data.rootref()) == "TRUE");
  CHECK(env.render("{% if lower(upper(name)) == \"peter\" %}TRUE{% endif %}", data.rootref()) == "TRUE");
  CHECK(env.render("{% for i in range(4) %}{{ loop.index1 }}{% endfor %}", data.rootref()) == "1234");
  CHECK(env.render("{{ upper(last(brother.daughters)) }}", data.rootref()) == "HELEN");
  CHECK(env.render("{{ length(name) * 2.5 }}", data.rootref()) == "12.5");
  CHECK(env.render("{{ upper(first(sort(brother.daughters)) + \"_test\") }}", data.rootref()) == "HELEN_TEST");
  CHECK(env.render("{% for i in range(3) %}{{ at(names, i) }}{% endfor %}", data.rootref()) == "JeffSebChris");
  CHECK(env.render("{% if not is_happy or age > 26 %}TRUE{% endif %}", data.rootref()) == "TRUE");
  CHECK(env.render("{{ last(list_of_objects).d * 2}}", data.rootref()) == "10");
  CHECK(env.render("{{ last(range(5)) * 2 }}", data.rootref()) == "8");
  CHECK(env.render("{{ last(range(5 * 2)) }}", data.rootref()) == "9");
  CHECK(env.render("{{ last(range(5 * 2)) }}", data.rootref()) == "9");
  CHECK(env.render("{{ not true }}", data.rootref()) == "false");
  CHECK(env.render("{{ not (true) }}", data.rootref()) == "false");
  CHECK(env.render("{{ true or (true or true) }}", data.rootref()) == "true");
  CHECK(env.render("{{ at(list_of_objects, 1).b }}", data.rootref()) == "3");
}
