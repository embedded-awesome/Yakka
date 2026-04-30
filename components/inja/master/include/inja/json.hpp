#ifndef INCLUDE_INJA_JSON_HPP_
#define INCLUDE_INJA_JSON_HPP_

#include <ryml.hpp>
#include "ryml_std.hpp"

namespace inja {
namespace ryml = ::ryml;
using Tree = ryml::Tree;
using NodeRef = ryml::NodeRef;
using ConstNodeRef = ryml::ConstNodeRef;
using Pointer = ryml::Pointer;
constexpr size_t NONE = size_t(-1);
} // namespace inja

#endif // INCLUDE_INJA_JSON_HPP_
