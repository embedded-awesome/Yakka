#pragma once
#include <ryml.hpp>
#include <ryml_std.hpp>
#include "inja.hpp"
#include "yakka.hpp"
#include "yakka_project.hpp"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <map>
#include <string>
#include <functional>
#include <expected>

namespace yakka {

typedef std::function<yakka::process_return(std::string, const ryml::ConstNodeRef &, std::string, const ryml::ConstNodeRef &, ryml::NodeRef &, inja::Environment &)> blueprint_command;

extern const std::map<const std::string, const blueprint_command> blueprint_commands;
} // namespace yakka