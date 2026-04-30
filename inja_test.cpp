#include <cstdlib>
#include <iostream>
#include <string>

#include <inja.hpp>

int main()
{
  try {
    inja::Environment env;

    {
      inja::json data;
      data["name"]             = "Yakka";
      const std::string result = env.render("Hello {{ name }}!", data);
      if (result != "Hello Yakka!") {
        std::cerr << "Test 1 failed: " << result << '\n';
        return EXIT_FAILURE;
      }
    }

    {
      inja::json data;
      data["values"][0]        = 1;
      data["values"][1]        = 2;
      data["values"][2]        = 3;
      const std::string result = env.render("{% for v in values %}{{ v }}{% endfor %}", data);
      if (result != "123") {
        std::cerr << "Test 2 failed: " << result << '\n';
        return EXIT_FAILURE;
      }
    }

    {
      inja::json data;
      data["enabled"]          = true;
      const std::string result = env.render("{% if enabled %}on{% else %}off{% endif %}", data);
      if (result != "on") {
        std::cerr << "Test 3 failed: " << result << '\n';
        return EXIT_FAILURE;
      }
    }

    {
      inja::json data;
      data["user"]["first"]    = "Ada";
      const std::string result = env.render("{{ user.first | default(\"unknown\") }}", data);
      if (result != "Ada") {
        std::cerr << "Test 4 failed: " << result << '\n';
        return EXIT_FAILURE;
      }
    }

    {
      inja::json data;
      data["items"][0]         = "alpha";
      data["items"][1]         = "beta";
      const std::string result = env.render("{{ items | join(\",\") }}", data);
      if (result != "alpha,beta") {
        std::cerr << "Test 5 failed: " << result << '\n';
        return EXIT_FAILURE;
      }
    }

    std::cout << "All Inja tests passed." << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
