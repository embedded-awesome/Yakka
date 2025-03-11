#pragma once
#include <map>
#include <string>
#include <functional>
#include <expected>
#include "nlohmann/json.hpp"
#include "inja.hpp"

namespace yakka {

struct blueprint_return {
  std::string result;
  int retcode;
};
typedef std::function<yakka::blueprint_return(std::string, const nlohmann::json &, std::string, const nlohmann::json &, inja::Environment &)> blueprint_command;

extern const std::map<const std::string, const blueprint_command> blueprint_commands;
} // namespace yakka