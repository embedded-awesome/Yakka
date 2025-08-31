#pragma once
#include "nlohmann/json.hpp"
#include "inja.hpp"
#include "yakka.hpp"
#include "yakka_project.hpp"
#include <map>
#include <string>
#include <functional>
#include <expected>

namespace yakka {

typedef std::function<yakka::process_return(std::string, const nlohmann::json &, std::string, const nlohmann::json &, nlohmann::json &, inja::Environment &)> blueprint_command;

extern const std::map<const std::string, const blueprint_command> blueprint_commands;
} // namespace yakka