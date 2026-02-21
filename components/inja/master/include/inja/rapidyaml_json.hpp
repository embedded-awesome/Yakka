#pragma once

#include "ryml.hpp"
#include <fstream>
#include <sstream>

namespace inja {
namespace json {

using string_t = c4::csubstr;
using number_integer_t = int64_t;
using number_unsigned_t = uint64_t;
using number_float_t = double;

// inline json::node load_json(const std::string& filename) {
//     std::ifstream in(filename, std::ios::in | std::ios::binary);
//     std::ostringstream contents;
//     contents << in.rdbuf();
//     ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(contents.str()));
//     return tree.rootref();
//   }
  
} // namespace json
} // namespace inja