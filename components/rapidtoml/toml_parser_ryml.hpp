#pragma once

#include <ryml.hpp>
#include <string>
#include <string_view>

namespace toml_ryml
{

ryml::Tree parse_toml(std::string_view source, const std::string &source_path = "");

} // namespace toml_ryml