#include "template_engine.hpp"
#include "spdlog.h"

namespace yakka {

std::string try_render(inja::Environment &env, const std::string &input, const nlohmann::json &data)
{
  try {
    return env.render(input, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

std::string try_render_file(inja::Environment &env, const std::string &filename, const nlohmann::json &data)
{
  try {
    return env.render_file(filename, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
}

void add_common_commands(inja::Environment &env)
{
  
}

} // namespace yakka