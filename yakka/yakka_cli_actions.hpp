#pragma once

#include "yakka.hpp"
#include "yakka_workspace.hpp"
#include "yakka_project.hpp"
#include "yakka_config.hpp"
#include "cxxopts.hpp"
#include "spdlog/spdlog.h"
#include "subprocess.hpp"
#include <filesystem>
#include <unordered_map>
#include <functional>

namespace yakka {

using action_handler = std::function<int(yakka::workspace &, const cxxopts::ParseResult &)>;

int register_action(workspace &workspace, const cxxopts::ParseResult &result);
int list_action(workspace &workspace, const cxxopts::ParseResult &result);
int update_action(workspace &workspace, const cxxopts::ParseResult &result);
int remove_action(workspace &workspace, const cxxopts::ParseResult &result);
int git_action(workspace &workspace, const cxxopts::ParseResult &result);
int fetch_action(workspace &workspace, const cxxopts::ParseResult &result);
int serve_action(workspace &workspace, const cxxopts::ParseResult &result);

// clang-format off
const std::unordered_map<std::string, action_handler> cli_actions = { 
  { "register", register_action }, 
  { "list", list_action },   
  { "update", update_action }, 
  { "remove", remove_action },
  { "git", git_action },           
  { "fetch", fetch_action }, 
  { "serve", serve_action } 
};
// clang-format on

void download_unknown_components(yakka::workspace &workspace, yakka::project &project);

} // namespace yakka