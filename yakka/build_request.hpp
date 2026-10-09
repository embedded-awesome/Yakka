#pragma once

#include <expected>
#include <string>
#include <vector>

namespace yakka {

struct build_request {
  std::vector<std::string> arguments;
  std::string project_name;
};

/// Expand positional component/feature alternatives into independent project requests.
std::expected<std::vector<build_request>, std::string> expand_build_requests(
  const std::vector<std::string> &arguments, const std::string &project_name = "");

} // namespace yakka