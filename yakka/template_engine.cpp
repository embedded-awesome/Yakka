/**
 * @file template_engine.cpp
 * @brief Implements template rendering helpers used by blueprint generation.
 */

#include "template_engine.hpp"
#include "spdlog.h"

namespace yakka {


/// @brief Executes try_render.

std::string try_render(inja::Environment &env, const std::string &input, ryml::ConstNodeRef data)
{
  try {

    return env.render(input, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", input, e.what());
    return "";
  }
}

/// @brief Executes try_render_file.

std::string try_render_file(inja::Environment &env, const std::string &filename, ryml::ConstNodeRef data)
{
  try {

    return env.render_file(filename, data);
  } catch (std::exception &e) {
    spdlog::error("Template error: {}\n{}", filename, e.what());
    return "";
  }
}


} // namespace yakka
