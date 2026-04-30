/*
  ___        _          Version 3.5.0
 |_ _|_ __  (_) __ _    https://github.com/pantor/inja
  | || '_ \ | |/ _` |   Licensed under the MIT License <http://opensource.org/licenses/MIT>.
  | || | | || | (_| |
 |___|_| |_|/ |\__,_|   Copyright (c) 2018-2025 Lars Berscheid
          |__/
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef INCLUDE_INJA_INJA_HPP_
#define INCLUDE_INJA_INJA_HPP_

// #include "json.hpp"
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

// #include "throw.hpp"
#ifndef INCLUDE_INJA_THROW_HPP_
#define INCLUDE_INJA_THROW_HPP_

#if (defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)) && !defined(INJA_NOEXCEPTION)
#ifndef INJA_THROW
#define INJA_THROW(exception) throw exception
#endif
#else
#include <cstdlib>
#ifndef INJA_THROW
#define INJA_THROW(exception) \
std::abort();                 \
    std::ignore = exception
#endif
#ifndef INJA_NOEXCEPTION
#define INJA_NOEXCEPTION
#endif
#endif

#endif // INCLUDE_INJA_THROW_HPP_

// #include "environment.hpp"
#ifndef INCLUDE_INJA_ENVIRONMENT_HPP_
#define INCLUDE_INJA_ENVIRONMENT_HPP_

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

// #include "json.hpp"

// #include "utils.hpp"
#ifndef INCLUDE_INJA_UTILS_HPP_
#define INCLUDE_INJA_UTILS_HPP_

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// #include "exceptions.hpp"
#ifndef INCLUDE_INJA_EXCEPTIONS_HPP_
#define INCLUDE_INJA_EXCEPTIONS_HPP_

#include <cstddef>
#include <stdexcept>
#include <string>

namespace inja {

struct SourceLocation {
  size_t line;
  size_t column;
};

struct InjaError : public std::runtime_error {
  const std::string type;
  const std::string message;

  const SourceLocation location;

  explicit InjaError(const std::string& type, const std::string& message)
      : std::runtime_error("[inja.exception." + type + "] " + message), type(type), message(message), location({0, 0}) {}

  explicit InjaError(const std::string& type, const std::string& message, SourceLocation location)
      : std::runtime_error("[inja.exception." + type + "] (at " + std::to_string(location.line) + ":" + std::to_string(location.column) + ") " + message),
        type(type), message(message), location(location) {}
};

struct ParserError : public InjaError {
  explicit ParserError(const std::string& message, SourceLocation location): InjaError("parser_error", message, location) {}
};

struct RenderError : public InjaError {
  explicit RenderError(const std::string& message, SourceLocation location): InjaError("render_error", message, location) {}
};

struct FileError : public InjaError {
  explicit FileError(const std::string& message): InjaError("file_error", message) {}
  explicit FileError(const std::string& message, SourceLocation location): InjaError("file_error", message, location) {}
};

struct DataError : public InjaError {
  explicit DataError(const std::string& message, SourceLocation location): InjaError("data_error", message, location) {}
};

} // namespace inja

#endif // INCLUDE_INJA_EXCEPTIONS_HPP_

// #include "json.hpp"

#include "spdlog/spdlog.h"

namespace inja {

namespace string_view {
inline std::string_view slice(std::string_view view, size_t start, size_t end) {
  start = std::min(start, view.size());
  end = std::min(std::max(start, end), view.size());
  return view.substr(start, end - start);
}

inline std::pair<std::string_view, std::string_view> split(std::string_view view, char Separator) {
  const size_t idx = view.find(Separator);
  if (idx == std::string_view::npos) {
    return std::make_pair(view, std::string_view());
  }
  return std::make_pair(slice(view, 0, idx), slice(view, idx + 1, std::string_view::npos));
}

inline bool starts_with(std::string_view view, std::string_view prefix) {
  return (view.size() >= prefix.size() && view.compare(0, prefix.size(), prefix) == 0);
}
} // namespace string_view

inline SourceLocation get_source_location(std::string_view content, size_t pos) {
  // Get line and offset position (starts at 1:1)
  auto sliced = string_view::slice(content, 0, pos);
  const std::size_t last_newline = sliced.rfind('\n');

  if (last_newline == std::string_view::npos) {
    return {1, sliced.length() + 1};
  }

  // Count newlines
  size_t count_lines = 0;
  size_t search_start = 0;
  while (search_start <= sliced.size()) {
    search_start = sliced.find('\n', search_start) + 1;
    if (search_start == 0) {
      break;
    }
    count_lines += 1;
  }

  return {count_lines + 1, sliced.length() - last_newline};
}

inline void replace_substring(std::string& s, const std::string& f, const std::string& t) {
  if (f.empty()) {
    return;
  }
  for (auto pos = s.find(f);            // find first occurrence of f
       pos != std::string::npos;        // make sure f was found
       s.replace(pos, f.size(), t),     // replace with t, and
       pos = s.find(f, pos + t.size())) // find next occurrence of f
  {}
}

inline std::string to_std_string(::ryml::csubstr view) {
  return std::string(view.str, view.len);
}

inline ::ryml::csubstr to_csubstr(std::string_view view) {
  return ::ryml::csubstr(view.data(), view.size());
}

inline std::string_view trim(std::string_view view) {
  size_t start = 0;
  while (start < view.size() && std::isspace(static_cast<unsigned char>(view[start]))) {
    ++start;
  }
  size_t end = view.size();
  while (end > start && std::isspace(static_cast<unsigned char>(view[end - 1]))) {
    --end;
  }
  return view.substr(start, end - start);
}

inline std::optional<int64_t> parse_int(std::string_view view) {
  view = trim(view);
  if (view.empty()) {
    return std::nullopt;
  }

  if (view[0] == '-') {
    int64_t result;
    auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), result);
    if (ec != std::errc() || ptr != view.data() + view.size()) {
      return std::nullopt;
    }
    return result;
  }

  uint64_t result;
  auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), result);
  if (ec != std::errc() || ptr != view.data() + view.size()) {
    return std::nullopt;
  }
  if (result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<int64_t>(result);
}

inline std::optional<uint64_t> parse_uint(std::string_view view) {
  view = trim(view);
  if (view.empty() || view[0] == '-' || view[0] == '+') {
    return std::nullopt;
  }

  uint64_t result;
  auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), result);
  if (ec != std::errc() || ptr != view.data() + view.size()) {
    return std::nullopt;
  } else {
    return result;
  }
}

inline std::optional<double> parse_float(std::string_view view) {
  view = trim(view);
  if (view.empty()) {
    return std::nullopt;
  }
  // Use strtod for portability - std::from_chars for floating-point is not
  // available on all platforms (e.g. older macOS/libc++ versions).
  std::string str(view);
  char* end = nullptr;
  errno = 0;
  double result = std::strtod(str.c_str(), &end);
  if (errno != 0 || end != str.c_str() + str.size()) {
    return std::nullopt;
  }
  return result;
}

inline std::optional<bool> parse_bool(std::string_view view) {
  view = trim(view);
  if (view == "true" || view == "True" || view == "TRUE") {
    return true;
  }
  if (view == "false" || view == "False" || view == "FALSE") {
    return false;
  }
  return std::nullopt;
}

inline bool is_null_like(std::string_view view) {
  view = trim(view);
  return view.empty() || view == "null" || view == "Null" || view == "NULL" || view == "~";
}

inline bool node_is_null(const ConstNodeRef& node) {
  if (!node.valid()) {
    return true;
  }
  if (node.is_val() || node.is_keyval()) {
    if (node.val_is_null()) {
      return true;
    }
    return is_null_like(std::string_view(node.val().str, node.val().len));
  }
  return false;
}

inline std::optional<std::string_view> node_scalar_view(const ConstNodeRef& node) {
  if (!node.valid()) {
    return std::nullopt;
  }
  if (node.is_val() || node.is_keyval()) {
    return std::string_view(node.val().str, node.val().len);
  }
  return std::nullopt;
}

inline bool node_is_bool_like(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  return view && parse_bool(*view).has_value();
}

inline bool node_is_int_like(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  return view && parse_int(*view).has_value();
}

inline bool node_is_uint_like(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  return view && parse_uint(*view).has_value();
}

inline bool node_is_float_like(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  return view && parse_float(*view).has_value();
}

inline std::optional<int64_t> node_to_int(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  if (!view) {
    return std::nullopt;
  }
  return parse_int(*view);
}

inline std::optional<double> node_to_double(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  if (!view) {
    return std::nullopt;
  }
  return parse_float(*view);
}

inline std::optional<uint64_t> node_to_uint(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  if (!view) {
    return std::nullopt;
  }
  return parse_uint(*view);
}

inline std::optional<bool> node_to_bool(const ConstNodeRef& node) {
  const auto view = node_scalar_view(node);
  if (!view) {
    return std::nullopt;
  }
  return parse_bool(*view);
}

inline std::string node_to_string(const ConstNodeRef& node) {
  if (node_is_bool_like(node)) {
    if (auto parsed = node_to_bool(node)) {
      return *parsed ? "true" : "false";
    }
  }
  if (node_is_int_like(node)) {
    if (auto parsed = node_to_int(node)) {
      return std::to_string(*parsed);
    }
  }
  if (node_is_float_like(node)) {
    if (auto parsed = node_to_double(node)) {
      auto s = std::to_string(*parsed);
      // trim excess trailing zeros, leave one
      auto last_non_zero = s.find_last_not_of('0');
      if (last_non_zero != std::string::npos && s[last_non_zero] == '.') {
        s.erase(last_non_zero + 2);
      } else if (last_non_zero != std::string::npos) {
        s.erase(last_non_zero + 1);
      }
      return s;
    }
  }
  const auto view = node_scalar_view(node);
  if (!view) {
    return std::string();
  }
  return std::string(view->data(), view->size());
}

inline bool node_truthy(const ConstNodeRef& node) {
  if (!node.valid()) {
    return false;
  }
  if (node.has_children()) {
    return node.num_children() > 0;
  }
  if (node_is_null(node)) {
    return false;
  }
  const auto view = node_scalar_view(node);
  if (!view) {
    return false;
  }
  if (auto parsed = parse_bool(*view)) {
    return *parsed;
  }
  if (auto parsed = parse_float(*view)) {
    return *parsed != 0.0;
  }
  return !trim(*view).empty();
}

enum class NativeKind {
  Invalid,
  Int64,
  UInt64,
  Float64,
  String,
  Bool,
};

struct NativeValue {
  NativeKind kind {NativeKind::Invalid};
  int64_t i64 {};
  uint64_t u64 {};
  double f64 {};
  bool b {};
  std::string s;
};

inline NativeValue to_native_value(const ConstNodeRef& node) {
  NativeValue out;
  if (!node.valid() || node.has_children() || node_is_null(node)) {
    return out;
  }

  const auto view = node_scalar_view(node);
  if (!view) {
    return out;
  }

  if (auto b = parse_bool(*view)) {
    out.kind = NativeKind::Bool;
    out.b = *b;
    return out;
  }
  if (auto i = parse_int(*view)) {
    out.kind = NativeKind::Int64;
    out.i64 = *i;
    return out;
  }
  if (auto u = parse_uint(*view)) {
    out.kind = NativeKind::UInt64;
    out.u64 = *u;
    return out;
  }
  if (auto f = parse_float(*view)) {
    out.kind = NativeKind::Float64;
    out.f64 = *f;
    return out;
  }

  out.kind = NativeKind::String;
  out.s = std::string(view->data(), view->size());
  return out;
}

inline NativeKind query_kind(const ConstNodeRef& node) {
  return to_native_value(node).kind;
}

struct NativeNodeRef {
  ConstNodeRef node;
  mutable bool has_cached_value {false};
  mutable NativeValue cached;

  NativeNodeRef() = default;
  explicit NativeNodeRef(ConstNodeRef ref): node(ref) {}

  bool valid() const {
    return node.valid();
  }

  const NativeValue& value() const {
    if (!has_cached_value) {
      cached = to_native_value(node);
      has_cached_value = true;
    }
    return cached;
  }

  NativeKind kind() const {
    return value().kind;
  }
};

inline bool native_is_number(const NativeKind kind) {
  return kind == NativeKind::Int64 || kind == NativeKind::UInt64 || kind == NativeKind::Float64;
}

inline std::optional<double> native_to_double(const NativeNodeRef& node) {
  const auto& value = node.value();
  switch (value.kind) {
  case NativeKind::Int64:
    return static_cast<double>(value.i64);
  case NativeKind::UInt64:
    return static_cast<double>(value.u64);
  case NativeKind::Float64:
    return value.f64;
  default:
    return std::nullopt;
  }
}

inline std::optional<int64_t> native_to_int(const NativeNodeRef& node) {
  const auto& value = node.value();
  switch (value.kind) {
  case NativeKind::Int64:
    return value.i64;
  case NativeKind::UInt64:
    if (value.u64 <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return static_cast<int64_t>(value.u64);
    }
    return std::nullopt;
  case NativeKind::Float64:
    return static_cast<int64_t>(value.f64);
  default:
    return std::nullopt;
  }
}

inline std::optional<uint64_t> native_to_uint(const NativeNodeRef& node) {
  const auto& value = node.value();
  switch (value.kind) {
  case NativeKind::UInt64:
    return value.u64;
  case NativeKind::Int64:
    if (value.i64 >= 0) {
      return static_cast<uint64_t>(value.i64);
    }
    return std::nullopt;
  case NativeKind::Float64:
    if (value.f64 >= 0.0) {
      return static_cast<uint64_t>(value.f64);
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

inline std::optional<bool> native_to_bool(const NativeNodeRef& node) {
  const auto& value = node.value();
  if (value.kind == NativeKind::Bool) {
    return value.b;
  }
  return std::nullopt;
}

inline std::string native_to_string(const NativeNodeRef& native_ref) {
  if (!native_ref.valid()) {
    return std::string();
  }
  if (native_ref.node.has_children() && !native_ref.node.is_val()) {
    std::ostringstream os;
    os << ryml::as_json(*native_ref.node.tree(), native_ref.node.id());
    return os.str();
  }

  const auto& value = native_ref.value();
  switch (value.kind) {
  case NativeKind::Bool:
    return value.b ? "true" : "false";
  case NativeKind::Int64:
    return std::to_string(value.i64);
  case NativeKind::UInt64:
    return std::to_string(value.u64);
  case NativeKind::Float64: {
    auto s = std::to_string(value.f64);
    const auto last_non_zero = s.find_last_not_of('0');
    if (last_non_zero != std::string::npos && s[last_non_zero] == '.') {
      s.erase(last_non_zero + 2);
    } else if (last_non_zero != std::string::npos) {
      s.erase(last_non_zero + 1);
    }
    return s;
  }
  case NativeKind::String:
    return value.s;
  case NativeKind::Invalid:
  default:
    return node_to_string(native_ref.node);
  }
}

inline bool native_truthy(const NativeNodeRef& native_ref) {
  if (!native_ref.valid()) {
    return false;
  }
  if (native_ref.node.has_children()) {
    return native_ref.node.num_children() > 0;
  }

  const auto& value = native_ref.value();
  switch (value.kind) {
  case NativeKind::Bool:
    return value.b;
  case NativeKind::Int64:
    return value.i64 != 0;
  case NativeKind::UInt64:
    return value.u64 != 0;
  case NativeKind::Float64:
    return value.f64 != 0.0;
  case NativeKind::String:
    return !trim(value.s).empty();
  case NativeKind::Invalid:
  default:
    return false;
  }
}

inline void write_native_value(NodeRef node, const NativeValue& value) {
  switch (value.kind) {
  case NativeKind::Bool:
    node << (value.b ? "true" : "false");
    break;
  case NativeKind::Int64:
    node << value.i64;
    break;
  case NativeKind::UInt64:
    node << value.u64;
    break;
  case NativeKind::Float64:
    node << value.f64;
    break;
  case NativeKind::String:
    node << to_csubstr(value.s);
    break;
  case NativeKind::Invalid:
  default:
    node = nullptr;
    break;
  }
}

inline NodeRef append_native_tmp(NodeRef tmp_root, const NativeValue& value) {
  auto node = tmp_root.append_child();
  write_native_value(node, value);
  return node;
}

inline ConstNodeRef find_child_by_key(ConstNodeRef node, ::ryml::csubstr key) {
  if (!node.valid()) {
    return ConstNodeRef();
  }
  for (auto child : node.children()) {
    if (child.key() == key) {
      return child;
    }
  }
  return ConstNodeRef();
}

inline NodeRef find_child_by_key(NodeRef node, ::ryml::csubstr key) {
  if (!node.valid()) {
    return NodeRef();
  }
  for (auto child : node.children()) {
    if (child.has_key() && child.key() == key) {
      return child;
    }
  }
  return NodeRef();
}

inline ConstNodeRef resolve_pointer(ConstNodeRef root, const Pointer& ptr) {
  if (root.contains(ptr)) {
    return root[ptr];
  }
  return ConstNodeRef{};
  // ConstNodeRef current = root;
  // for (const auto& token : ptr.tokens()) {
  //   if (!current.valid()) {
  //     return ConstNodeRef();
  //   }
  //   if (current.is_map()) {
  //     current = current.find_child(token);
  //     current = find_child_by_key(current, token);
  //   } else if (current.is_seq()) {
  //     const std::string_view view(token.str, token.len);
  //     const auto index = parse_int(view);
  //     if (!index || *index < 0) {
  //       return ConstNodeRef();
  //     }
  //     if (static_cast<size_t>(*index) >= current.num_children()) {
  //       return ConstNodeRef();
  //     }
  //     current = current.child(static_cast<size_t>(*index));
  //   } else {
  //     return ConstNodeRef();
  //   }
  // }
  // return current;
}

inline NodeRef ensure_child_map(NodeRef parent, std::string_view key) {
  const ::ryml::csubstr key_sub = to_csubstr(key);
  auto child = find_child_by_key(parent, key_sub);
  if (!child.valid()) {
    child = parent.append_child();
    child << ::ryml::key(key_sub);
  }
  child |= ::ryml::MAP;
  return child;
}

inline NodeRef ensure_child_seq(NodeRef parent, std::string_view key) {
  const ::ryml::csubstr key_sub = to_csubstr(key);
  auto child = find_child_by_key(parent, key_sub);
  if (!child.valid()) {
    child = parent.append_child();
    child << ::ryml::key(key_sub);
  }
  child |= ::ryml::SEQ;
  return child;
}

inline NodeRef ensure_path(NodeRef root, const Pointer& ptr) {
  NodeRef current = root;
  for (const auto& token : ptr.tokens()) {
    if (!current.valid()) {
      return NodeRef();
    }
    if (!current.is_map()) {
      current |= ::ryml::MAP;
    }
    auto child = find_child_by_key(current, token);
    if (!child.valid()) {
      child = current.append_child();
      child << ::ryml::key(token);
    }
    current = child;
  }
  return current;
}

inline void set_child_from_node(NodeRef parent, std::string_view key, ConstNodeRef src) {
  if (!parent.valid()) {
    return;
  }
  const ::ryml::csubstr key_sub = to_csubstr(key);
  if (parent.contains(key_sub)) {
    parent.remove_child(key_sub);
  }

  const size_t new_index = parent.tree()->duplicate(src.tree(), src.id(), parent.id(), /*parent.last_child().id());*/ c4::yml::NONE);
  auto node = NodeRef(parent.tree(), new_index);
  node << ryml::key(key_sub);
}

} // namespace inja

#endif // INCLUDE_INJA_UTILS_HPP_

// #include "config.hpp"
#ifndef INCLUDE_INJA_CONFIG_HPP_
#define INCLUDE_INJA_CONFIG_HPP_

#include <filesystem>
#include <functional>
#include <string>

// #include "template.hpp"
#ifndef INCLUDE_INJA_TEMPLATE_HPP_
#define INCLUDE_INJA_TEMPLATE_HPP_

#include <map>
#include <memory>
#include <string>

// #include "node.hpp"
#ifndef INCLUDE_INJA_NODE_HPP_
#define INCLUDE_INJA_NODE_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// #include "function_storage.hpp"
#ifndef INCLUDE_INJA_FUNCTION_STORAGE_HPP_
#define INCLUDE_INJA_FUNCTION_STORAGE_HPP_

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #include "json.hpp"


namespace inja {

using Arguments = std::vector<ConstNodeRef>;
using CallbackFunction = std::function<ConstNodeRef(Arguments& args, NodeRef additional_data)>;
using VoidCallbackFunction = std::function<void(Arguments& args, NodeRef additional_data)>;

/*!
 * \brief Class for builtin functions and user-defined callbacks.
 */
class FunctionStorage {
public:
  enum class Operation {
    Not,
    And,
    Or,
    In,
    Equal,
    NotEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    Add,
    Subtract,
    Multiplication,
    Division,
    Power,
    Modulo,
    AtId,
    At,
    Capitalize,
    Default,
    DivisibleBy,
    Even,
    Exists,
    ExistsInObject,
    First,
    Float,
    Int,
    IsArray,
    IsBoolean,
    IsFloat,
    IsInteger,
    IsNumber,
    IsObject,
    IsString,
    Last,
    Length,
    Lower,
    Max,
    Min,
    Odd,
    Range,
    Replace,
    Round,
    Sort,
    Upper,
    Super,
    Join,
    Macro,
    Callback,
    Hex,
    None,
  };

  struct FunctionData {
    explicit FunctionData(const Operation& op, const CallbackFunction& cb = CallbackFunction {}): operation(op), callback(cb) {}
    const Operation operation;
    const CallbackFunction callback;
  };

private:
  const int VARIADIC {-1};

  std::map<std::pair<std::string, int>, FunctionData> function_storage = {
      {std::make_pair("at", 1), FunctionData {Operation::At}},
      {std::make_pair("at", 2), FunctionData {Operation::At}},
      {std::make_pair("capitalize", 1), FunctionData {Operation::Capitalize}},
      {std::make_pair("default", 2), FunctionData {Operation::Default}},
      {std::make_pair("divisibleBy", 2), FunctionData {Operation::DivisibleBy}},
      {std::make_pair("even", 1), FunctionData {Operation::Even}},
      {std::make_pair("exists", 1), FunctionData {Operation::Exists}},
      {std::make_pair("existsIn", 2), FunctionData {Operation::ExistsInObject}},
      {std::make_pair("first", 1), FunctionData {Operation::First}},
      {std::make_pair("float", 1), FunctionData {Operation::Float}},
      {std::make_pair("int", 1), FunctionData {Operation::Int}},
      {std::make_pair("isArray", 1), FunctionData {Operation::IsArray}},
      {std::make_pair("isBoolean", 1), FunctionData {Operation::IsBoolean}},
      {std::make_pair("isFloat", 1), FunctionData {Operation::IsFloat}},
      {std::make_pair("isInteger", 1), FunctionData {Operation::IsInteger}},
      {std::make_pair("isNumber", 1), FunctionData {Operation::IsNumber}},
      {std::make_pair("isObject", 1), FunctionData {Operation::IsObject}},
      {std::make_pair("isString", 1), FunctionData {Operation::IsString}},
      {std::make_pair("last", 1), FunctionData {Operation::Last}},
      {std::make_pair("length", 1), FunctionData {Operation::Length}},
      {std::make_pair("lower", 1), FunctionData {Operation::Lower}},
      {std::make_pair("max", 1), FunctionData {Operation::Max}},
      {std::make_pair("min", 1), FunctionData {Operation::Min}},
      {std::make_pair("odd", 1), FunctionData {Operation::Odd}},
      {std::make_pair("range", 1), FunctionData {Operation::Range}},
      {std::make_pair("replace", 3), FunctionData {Operation::Replace}},
      {std::make_pair("round", 2), FunctionData {Operation::Round}},
      {std::make_pair("sort", 1), FunctionData {Operation::Sort}},
      {std::make_pair("upper", 1), FunctionData {Operation::Upper}},
      {std::make_pair("super", 0), FunctionData {Operation::Super}},
      {std::make_pair("super", 1), FunctionData {Operation::Super}},
      {std::make_pair("join", 2), FunctionData {Operation::Join}},
      {std::make_pair("hex", 1), FunctionData {Operation::Hex}},
  };

public:
  void add_builtin(std::string_view name, int num_args, Operation op) {
    function_storage.emplace(std::make_pair(static_cast<std::string>(name), num_args), FunctionData {op});
  }

  void add_callback(std::string_view name, int num_args, const CallbackFunction& callback) {
    function_storage.emplace(std::make_pair(static_cast<std::string>(name), num_args), FunctionData {Operation::Callback, callback});
  }

  FunctionData find_function(std::string_view name, int num_args) const {
    auto it = function_storage.find(std::make_pair(static_cast<std::string>(name), num_args));
    if (it != function_storage.end()) {
      return it->second;

      // Find variadic function
    } else if (num_args > 0) {
      it = function_storage.find(std::make_pair(static_cast<std::string>(name), VARIADIC));
      if (it != function_storage.end()) {
        return it->second;
      }
    }

    return FunctionData {Operation::None};
  }
};

} // namespace inja

#endif // INCLUDE_INJA_FUNCTION_STORAGE_HPP_

// #include "json.hpp"

// #include "utils.hpp"


namespace inja {

class NodeVisitor;
class BlockNode;
class TextNode;
class ExpressionNode;
class LiteralNode;
class DataNode;
class FunctionNode;
class ExpressionListNode;
class StatementNode;
class ForStatementNode;
class ForArrayStatementNode;
class ForObjectStatementNode;
class IfStatementNode;
class IncludeStatementNode;
class ExtendsStatementNode;
class BlockStatementNode;
class SetStatementNode;
class MacroStatementNode;

class NodeVisitor {
public:
  virtual ~NodeVisitor() = default;

  virtual void visit(const BlockNode& node) = 0;
  virtual void visit(const TextNode& node) = 0;
  virtual void visit(const ExpressionNode& node) = 0;
  virtual void visit(const LiteralNode& node) = 0;
  virtual void visit(const DataNode& node) = 0;
  virtual void visit(const FunctionNode& node) = 0;
  virtual void visit(const ExpressionListNode& node) = 0;
  virtual void visit(const StatementNode& node) = 0;
  virtual void visit(const ForStatementNode& node) = 0;
  virtual void visit(const ForArrayStatementNode& node) = 0;
  virtual void visit(const ForObjectStatementNode& node) = 0;
  virtual void visit(const IfStatementNode& node) = 0;
  virtual void visit(const IncludeStatementNode& node) = 0;
  virtual void visit(const ExtendsStatementNode& node) = 0;
  virtual void visit(const BlockStatementNode& node) = 0;
  virtual void visit(const SetStatementNode& node) = 0;
  virtual void visit(const MacroStatementNode& node) = 0;
};

/*!
 * \brief Base node class for the abstract syntax tree (AST).
 */
class AstNode {
public:
  virtual void accept(NodeVisitor& v) const = 0;

  size_t pos;

  explicit AstNode(size_t pos): pos(pos) {}
  virtual ~AstNode() {}
};

class BlockNode : public AstNode {
public:
  std::vector<std::shared_ptr<AstNode>> nodes;

  explicit BlockNode(): AstNode(0) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class TextNode : public AstNode {
public:
  const size_t length;

  explicit TextNode(size_t pos, size_t length): AstNode(pos), length(length) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class ExpressionNode : public AstNode {
public:
  explicit ExpressionNode(size_t pos): AstNode(pos) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class LiteralNode : public ExpressionNode {
public:
  const std::string_view text;

  explicit LiteralNode(std::string_view data_text, size_t pos): ExpressionNode(pos), text(data_text) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class DataNode : public ExpressionNode {
public:
  const std::string name;
  const Pointer ptr;

  static std::string convert_dot_to_ptr(std::string_view ptr_name) {
    std::string result;
    while (!ptr_name.empty()) {
      std::string_view part;
      std::tie(part, ptr_name) = string_view::split(ptr_name, '.');
      result.push_back('/');
      result.append(part.begin(), part.end());
    };
    return result;
  }

  explicit DataNode(std::string_view ptr_name, size_t pos): ExpressionNode(pos), name(ptr_name), ptr(Pointer(convert_dot_to_ptr(ptr_name))) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class FunctionNode : public ExpressionNode {
  using Op = FunctionStorage::Operation;

public:
  enum class Associativity {
    Left,
    Right,
  };

  unsigned int precedence;
  Associativity associativity;

  Op operation;

  std::string name;
  int number_args; // Can also be negative -> -1 for unknown number
  std::vector<std::shared_ptr<ExpressionNode>> arguments;
  CallbackFunction callback;

  explicit FunctionNode(std::string_view name, size_t pos)
      : ExpressionNode(pos), precedence(8), associativity(Associativity::Left), operation(Op::Callback), name(name), number_args(0) {}
  explicit FunctionNode(Op operation, size_t pos): ExpressionNode(pos), operation(operation), number_args(1) {
    switch (operation) {
    case Op::Not: {
      number_args = 1;
      precedence = 4;
      associativity = Associativity::Left;
    } break;
    case Op::And: {
      number_args = 2;
      precedence = 1;
      associativity = Associativity::Left;
    } break;
    case Op::Or: {
      number_args = 2;
      precedence = 1;
      associativity = Associativity::Left;
    } break;
    case Op::In: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::Equal: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::NotEqual: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::Greater: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::GreaterEqual: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::Less: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::LessEqual: {
      number_args = 2;
      precedence = 2;
      associativity = Associativity::Left;
    } break;
    case Op::Add: {
      number_args = 2;
      precedence = 3;
      associativity = Associativity::Left;
    } break;
    case Op::Subtract: {
      number_args = 2;
      precedence = 3;
      associativity = Associativity::Left;
    } break;
    case Op::Multiplication: {
      number_args = 2;
      precedence = 4;
      associativity = Associativity::Left;
    } break;
    case Op::Division: {
      number_args = 2;
      precedence = 4;
      associativity = Associativity::Left;
    } break;
    case Op::Power: {
      number_args = 2;
      precedence = 5;
      associativity = Associativity::Right;
    } break;
    case Op::Modulo: {
      number_args = 2;
      precedence = 4;
      associativity = Associativity::Left;
    } break;
    case Op::AtId: {
      number_args = 2;
      precedence = 8;
      associativity = Associativity::Left;
    } break;
    default: {
      precedence = 1;
      associativity = Associativity::Left;
    }
    }
  }

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class ExpressionListNode : public AstNode {
public:
  std::shared_ptr<ExpressionNode> root;

  explicit ExpressionListNode(): AstNode(0) {}
  explicit ExpressionListNode(size_t pos): AstNode(pos) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class StatementNode : public AstNode {
public:
  explicit StatementNode(size_t pos): AstNode(pos) {}

  virtual void accept(NodeVisitor& v) const = 0;
};

class ForStatementNode : public StatementNode {
public:
  ExpressionListNode condition;
  BlockNode body;
  BlockNode* const parent;

  explicit ForStatementNode(BlockNode* const parent, size_t pos): StatementNode(pos), parent(parent) {}

  virtual void accept(NodeVisitor& v) const = 0;
};

class ForArrayStatementNode : public ForStatementNode {
public:
  const std::string value;

  explicit ForArrayStatementNode(const std::string& value, BlockNode* const parent, size_t pos): ForStatementNode(parent, pos), value(value) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class ForObjectStatementNode : public ForStatementNode {
public:
  const std::string key;
  const std::string value;

  explicit ForObjectStatementNode(const std::string& key, const std::string& value, BlockNode* const parent, size_t pos)
      : ForStatementNode(parent, pos), key(key), value(value) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class IfStatementNode : public StatementNode {
public:
  ExpressionListNode condition;
  BlockNode true_statement;
  BlockNode false_statement;
  BlockNode* const parent;

  const bool is_nested;
  bool has_false_statement {false};

  explicit IfStatementNode(BlockNode* const parent, size_t pos): StatementNode(pos), parent(parent), is_nested(false) {}
  explicit IfStatementNode(bool is_nested, BlockNode* const parent, size_t pos): StatementNode(pos), parent(parent), is_nested(is_nested) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class IncludeStatementNode : public StatementNode {
public:
  const std::string file;

  explicit IncludeStatementNode(const std::string& file, size_t pos): StatementNode(pos), file(file) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class ExtendsStatementNode : public StatementNode {
public:
  const std::string file;

  explicit ExtendsStatementNode(const std::string& file, size_t pos): StatementNode(pos), file(file) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class BlockStatementNode : public StatementNode {
public:
  const std::string name;
  BlockNode block;
  BlockNode* const parent;

  explicit BlockStatementNode(BlockNode* const parent, const std::string& name, size_t pos): StatementNode(pos), name(name), parent(parent) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class SetStatementNode : public StatementNode {
public:
  const std::string key;
  ExpressionListNode expression;

  explicit SetStatementNode(const std::string& key, size_t pos): StatementNode(pos), key(key) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

class MacroStatementNode : public StatementNode {
public:
  const std::string name;
  std::vector<std::string> parameters;
  BlockNode body;
  BlockNode* const parent;

  explicit MacroStatementNode(BlockNode* const parent, const std::string& name, size_t pos)
      : StatementNode(pos), name(name), parent(parent) {}

  void accept(NodeVisitor& v) const override {
    v.visit(*this);
  }
};

} // namespace inja

#endif // INCLUDE_INJA_NODE_HPP_

// #include "statistics.hpp"
#ifndef INCLUDE_INJA_STATISTICS_HPP_
#define INCLUDE_INJA_STATISTICS_HPP_

// #include "node.hpp"


namespace inja {

/*!
 * \brief A class for counting statistics on a Template.
 */
class StatisticsVisitor : public NodeVisitor {
  void visit(const BlockNode& node) override {
    for (const auto& n : node.nodes) {
      n->accept(*this);
    }
  }

  void visit(const TextNode&) override {}
  void visit(const ExpressionNode&) override {}
  void visit(const LiteralNode&) override {}

  void visit(const DataNode&) override {
    variable_counter += 1;
  }

  void visit(const FunctionNode& node) override {
    for (const auto& n : node.arguments) {
      n->accept(*this);
    }
  }

  void visit(const ExpressionListNode& node) override {
    node.root->accept(*this);
  }

  void visit(const StatementNode&) override {}
  void visit(const ForStatementNode&) override {}

  void visit(const ForArrayStatementNode& node) override {
    node.condition.accept(*this);
    node.body.accept(*this);
  }

  void visit(const ForObjectStatementNode& node) override {
    node.condition.accept(*this);
    node.body.accept(*this);
  }

  void visit(const IfStatementNode& node) override {
    node.condition.accept(*this);
    node.true_statement.accept(*this);
    node.false_statement.accept(*this);
  }

  void visit(const IncludeStatementNode&) override {}

  void visit(const ExtendsStatementNode&) override {}

  void visit(const BlockStatementNode& node) override {
    node.block.accept(*this);
  }

  void visit(const SetStatementNode&) override {}

  void visit(const MacroStatementNode& node) override {
    node.body.accept(*this);
  }

public:
  size_t variable_counter {0};

  explicit StatisticsVisitor() {}
};

} // namespace inja

#endif // INCLUDE_INJA_STATISTICS_HPP_


namespace inja {

/*!
 * \brief The main inja Template.
 */
struct Template {
  BlockNode root;
  std::string content;
  std::map<std::string, std::shared_ptr<BlockStatementNode>> block_storage;
  std::map<std::pair<std::string, int>, std::shared_ptr<MacroStatementNode>> macro_storage;

  explicit Template() {}
  explicit Template(std::string content): content(std::move(content)) {}

  /// Return number of variables (total number, not distinct ones) in the template
  size_t count_variables() const {
    auto statistic_visitor = StatisticsVisitor();
    root.accept(statistic_visitor);
    return statistic_visitor.variable_counter;
  }
};

using TemplateStorage = std::map<std::string, Template>;

} // namespace inja

#endif // INCLUDE_INJA_TEMPLATE_HPP_


namespace inja {

/*!
 * \brief Class for lexer configuration.
 */
struct LexerConfig {
  std::string statement_open {"{%"};
  std::string statement_open_no_lstrip {"{%+"};
  std::string statement_open_force_lstrip {"{%-"};
  std::string statement_close {"%}"};
  std::string statement_close_force_rstrip {"-%}"};
  std::string line_statement {"##"};
  std::string expression_open {"{{"};
  std::string expression_open_force_lstrip {"{{-"};
  std::string expression_close {"}}"};
  std::string expression_close_force_rstrip {"-}}"};
  std::string comment_open {"{#"};
  std::string comment_open_force_lstrip {"{#-"};
  std::string comment_close {"#}"};
  std::string comment_close_force_rstrip {"-#}"};
  std::string open_chars {"#{"};

  bool trim_blocks {false};
  bool lstrip_blocks {false};

  void update_open_chars() {
    open_chars = "";
    if (open_chars.find(line_statement[0]) == std::string::npos) {
      open_chars += line_statement[0];
    }
    if (open_chars.find(statement_open[0]) == std::string::npos) {
      open_chars += statement_open[0];
    }
    if (open_chars.find(statement_open_no_lstrip[0]) == std::string::npos) {
      open_chars += statement_open_no_lstrip[0];
    }
    if (open_chars.find(statement_open_force_lstrip[0]) == std::string::npos) {
      open_chars += statement_open_force_lstrip[0];
    }
    if (open_chars.find(expression_open[0]) == std::string::npos) {
      open_chars += expression_open[0];
    }
    if (open_chars.find(expression_open_force_lstrip[0]) == std::string::npos) {
      open_chars += expression_open_force_lstrip[0];
    }
    if (open_chars.find(comment_open[0]) == std::string::npos) {
      open_chars += comment_open[0];
    }
    if (open_chars.find(comment_open_force_lstrip[0]) == std::string::npos) {
      open_chars += comment_open_force_lstrip[0];
    }
  }
};

/*!
 * \brief Class for parser configuration.
 */
struct ParserConfig {
  bool search_included_templates_in_files {true};

  std::function<Template(const std::filesystem::path&, const std::string&)> include_callback;
};

/*!
 * \brief Class for render configuration.
 */
struct RenderConfig {
  bool throw_at_missing_includes {true};
  bool html_autoescape {false};
};

} // namespace inja

#endif // INCLUDE_INJA_CONFIG_HPP_

// #include "function_storage.hpp"

// #include "parser.hpp"
#ifndef INCLUDE_INJA_PARSER_HPP_
#define INCLUDE_INJA_PARSER_HPP_

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stack>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #include "config.hpp"

// #include "exceptions.hpp"

// #include "function_storage.hpp"

// #include "lexer.hpp"
#ifndef INCLUDE_INJA_LEXER_HPP_
#define INCLUDE_INJA_LEXER_HPP_

#include <cctype>
#include <cstddef>
#include <string_view>

// #include "config.hpp"

// #include "exceptions.hpp"

// #include "token.hpp"
#ifndef INCLUDE_INJA_TOKEN_HPP_
#define INCLUDE_INJA_TOKEN_HPP_

#include <string>
#include <string_view>

namespace inja {

/*!
 * \brief Helper-class for the inja Lexer.
 */
struct Token {
  enum class Kind {
    Text,
    ExpressionOpen,     // {{
    ExpressionClose,    // }}
    LineStatementOpen,  // ##
    LineStatementClose, // \n
    StatementOpen,      // {%
    StatementClose,     // %}
    CommentOpen,        // {#
    CommentClose,       // #}
    Id,                 // this, this.foo
    Number,             // 1, 2, -1, 5.2, -5.3
    String,             // "this"
    Plus,               // +
    Minus,              // -
    Times,              // *
    Slash,              // /
    Percent,            // %
    Power,              // ^
    Comma,              // ,
    Dot,                // .
    Colon,              // :
    LeftParen,          // (
    RightParen,         // )
    LeftBracket,        // [
    RightBracket,       // ]
    LeftBrace,          // {
    RightBrace,         // }
    Equal,              // ==
    NotEqual,           // !=
    GreaterThan,        // >
    GreaterEqual,       // >=
    LessThan,           // <
    LessEqual,          // <=
    Pipe,               // |
    Unknown,
    Eof,
  };

  Kind kind {Kind::Unknown};
  std::string_view text;

  explicit constexpr Token() = default;
  explicit constexpr Token(Kind kind, std::string_view text): kind(kind), text(text) {}

  std::string describe() const {
    switch (kind) {
    case Kind::Text:
      return "<text>";
    case Kind::LineStatementClose:
      return "<eol>";
    case Kind::Eof:
      return "<eof>";
    default:
      return static_cast<std::string>(text);
    }
  }
};

} // namespace inja

#endif // INCLUDE_INJA_TOKEN_HPP_

// #include "utils.hpp"


namespace inja {

/*!
 * \brief Class for lexing an inja Template.
 */
class Lexer {
  enum class State {
    Text,
    ExpressionStart,
    ExpressionStartForceLstrip,
    ExpressionBody,
    LineStart,
    LineBody,
    StatementStart,
    StatementStartNoLstrip,
    StatementStartForceLstrip,
    StatementBody,
    CommentStart,
    CommentStartForceLstrip,
    CommentBody,
  };

  enum class MinusState {
    Operator,
    Number,
  };

  const LexerConfig& config;

  State state;
  MinusState minus_state;
  std::string_view m_in;
  size_t tok_start;
  size_t pos;

  Token scan_body(std::string_view close, Token::Kind closeKind, std::string_view close_trim = std::string_view(), bool trim = false) {
  again:
    // skip whitespace (except for \n as it might be a close)
    if (tok_start >= m_in.size()) {
      return make_token(Token::Kind::Eof);
    }
    const char ch = m_in[tok_start];
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      tok_start += 1;
      goto again;
    }

    // check for close
    if (!close_trim.empty() && inja::string_view::starts_with(m_in.substr(tok_start), close_trim)) {
      state = State::Text;
      pos = tok_start + close_trim.size();
      const Token tok = make_token(closeKind);
      skip_whitespaces_and_newlines();
      return tok;
    }

    if (inja::string_view::starts_with(m_in.substr(tok_start), close)) {
      state = State::Text;
      pos = tok_start + close.size();
      const Token tok = make_token(closeKind);
      if (trim) {
        skip_whitespaces_and_first_newline();
      }
      return tok;
    }

    // skip \n
    if (ch == '\n') {
      tok_start += 1;
      goto again;
    }

    pos = tok_start + 1;
    if (std::isalpha(ch)) {
      minus_state = MinusState::Operator;
      return scan_id();
    }

    const MinusState current_minus_state = minus_state;
    if (minus_state == MinusState::Operator) {
      minus_state = MinusState::Number;
    }

    switch (ch) {
    case '+':
      return make_token(Token::Kind::Plus);
    case '-':
      if (current_minus_state == MinusState::Operator) {
        return make_token(Token::Kind::Minus);
      }
      return scan_number();
    case '*':
      return make_token(Token::Kind::Times);
    case '/':
      return make_token(Token::Kind::Slash);
    case '^':
      return make_token(Token::Kind::Power);
    case '%':
      return make_token(Token::Kind::Percent);
    case '.':
      return make_token(Token::Kind::Dot);
    case ',':
      return make_token(Token::Kind::Comma);
    case ':':
      return make_token(Token::Kind::Colon);
    case '|':
      return make_token(Token::Kind::Pipe);
    case '(':
      return make_token(Token::Kind::LeftParen);
    case ')':
      minus_state = MinusState::Operator;
      return make_token(Token::Kind::RightParen);
    case '[':
      return make_token(Token::Kind::LeftBracket);
    case ']':
      minus_state = MinusState::Operator;
      return make_token(Token::Kind::RightBracket);
    case '{':
      return make_token(Token::Kind::LeftBrace);
    case '}':
      minus_state = MinusState::Operator;
      return make_token(Token::Kind::RightBrace);
    case '>':
      if (pos < m_in.size() && m_in[pos] == '=') {
        pos += 1;
        return make_token(Token::Kind::GreaterEqual);
      }
      return make_token(Token::Kind::GreaterThan);
    case '<':
      if (pos < m_in.size() && m_in[pos] == '=') {
        pos += 1;
        return make_token(Token::Kind::LessEqual);
      }
      return make_token(Token::Kind::LessThan);
    case '=':
      if (pos < m_in.size() && m_in[pos] == '=') {
        pos += 1;
        return make_token(Token::Kind::Equal);
      }
      return make_token(Token::Kind::Unknown);
    case '!':
      if (pos < m_in.size() && m_in[pos] == '=') {
        pos += 1;
        return make_token(Token::Kind::NotEqual);
      }
      return make_token(Token::Kind::Unknown);
    case '\"':
      return scan_string();
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      minus_state = MinusState::Operator;
      return scan_number();
    case '_':
    case '@':
    case '$':
      minus_state = MinusState::Operator;
      return scan_id();
    default:
      return make_token(Token::Kind::Unknown);
    }
  }

  Token scan_id() {
    for (;;) {
      if (pos >= m_in.size()) {
        break;
      }
      const char ch = m_in[pos];
      if (!std::isalnum(ch) && ch != '.' && ch != '/' && ch != '_' && ch != '-') {
        break;
      }
      pos += 1;
    }
    return make_token(Token::Kind::Id);
  }

  Token scan_number() {
    for (;;) {
      if (pos >= m_in.size()) {
        break;
      }
      const char ch = m_in[pos];
      // be very permissive in lexer (we'll catch errors when conversion happens)
      if (!(std::isdigit(ch) || ch == '.' || ch == 'e' || ch == 'E' || (ch == '+' && (pos == 0 || m_in[pos-1] == 'e' || m_in[pos-1] == 'E')) || (ch == '-' && (pos == 0 || m_in[pos-1] == 'e' || m_in[pos-1] == 'E')))) {
        break;
      }
      pos += 1;
    }
    return make_token(Token::Kind::Number);
  }

  Token scan_string() {
    bool escape {false};
    for (;;) {
      if (pos >= m_in.size()) {
        break;
      }
      const char ch = m_in[pos++];
      if (ch == '\\') {
        escape = !escape;
      } else if (!escape && ch == m_in[tok_start]) {
        break;
      } else {
        escape = false;
      }
    }
    return make_token(Token::Kind::String);
  }

  Token make_token(Token::Kind kind) const {
    return Token(kind, string_view::slice(m_in, tok_start, pos));
  }

  void skip_whitespaces_and_newlines() {
    if (pos < m_in.size()) {
      while (pos < m_in.size() && (m_in[pos] == ' ' || m_in[pos] == '\t' || m_in[pos] == '\n' || m_in[pos] == '\r')) {
        pos += 1;
      }
    }
  }

  void skip_whitespaces_and_first_newline() {
    if (pos < m_in.size()) {
      while (pos < m_in.size() && (m_in[pos] == ' ' || m_in[pos] == '\t')) {
        pos += 1;
      }
    }

    if (pos < m_in.size()) {
      const char ch = m_in[pos];
      if (ch == '\n') {
        pos += 1;
      } else if (ch == '\r') {
        pos += 1;
        if (pos < m_in.size() && m_in[pos] == '\n') {
          pos += 1;
        }
      }
    }
  }

  static std::string_view clear_final_line_if_whitespace(std::string_view text) {
    std::string_view result = text;
    while (!result.empty()) {
      const char ch = result.back();
      if (ch == ' ' || ch == '\t') {
        result.remove_suffix(1);
      } else if (ch == '\n' || ch == '\r') {
        break;
      } else {
        return text;
      }
    }
    return result;
  }

public:
  explicit Lexer(const LexerConfig& config): config(config), state(State::Text), minus_state(MinusState::Number), tok_start(0), pos(0) {}

  SourceLocation current_position() const {
    return get_source_location(m_in, tok_start);
  }

  void start(std::string_view input) {
    m_in = input;
    tok_start = 0;
    pos = 0;
    state = State::Text;
    minus_state = MinusState::Number;

    // Consume byte order mark (BOM) for UTF-8
    if (inja::string_view::starts_with(m_in, "\xEF\xBB\xBF")) {
      m_in = m_in.substr(3);
    }
  }

  Token scan() {
    tok_start = pos;

  again:
    if (tok_start >= m_in.size()) {
      return make_token(Token::Kind::Eof);
    }

    switch (state) {
    default:
    case State::Text: {
      // fast-scan to first open character
      const size_t open_start = m_in.substr(pos).find_first_of(config.open_chars);
      if (open_start == std::string_view::npos) {
        // didn't find open, return remaining text as text token
        pos = m_in.size();
        return make_token(Token::Kind::Text);
      }
      pos += open_start;

      // try to match one of the opening sequences, and get the close
      const std::string_view open_str = m_in.substr(pos);
      bool must_lstrip = false;
      if (inja::string_view::starts_with(open_str, config.expression_open)) {
        if (inja::string_view::starts_with(open_str, config.expression_open_force_lstrip)) {
          state = State::ExpressionStartForceLstrip;
          must_lstrip = true;
        } else {
          state = State::ExpressionStart;
        }
      } else if (inja::string_view::starts_with(open_str, config.statement_open)) {
        if (inja::string_view::starts_with(open_str, config.statement_open_no_lstrip)) {
          state = State::StatementStartNoLstrip;
        } else if (inja::string_view::starts_with(open_str, config.statement_open_force_lstrip)) {
          state = State::StatementStartForceLstrip;
          must_lstrip = true;
        } else {
          state = State::StatementStart;
          must_lstrip = config.lstrip_blocks;
        }
      } else if (inja::string_view::starts_with(open_str, config.comment_open)) {
        if (inja::string_view::starts_with(open_str, config.comment_open_force_lstrip)) {
          state = State::CommentStartForceLstrip;
          must_lstrip = true;
        } else {
          state = State::CommentStart;
          must_lstrip = config.lstrip_blocks;
        }
      } else if ((pos == 0 || m_in[pos - 1] == '\n') && inja::string_view::starts_with(open_str, config.line_statement)) {
        state = State::LineStart;
      } else {
        pos += 1; // wasn't actually an opening sequence
        goto again;
      }

      std::string_view text = string_view::slice(m_in, tok_start, pos);
      if (must_lstrip) {
        text = clear_final_line_if_whitespace(text);
      }

      if (text.empty()) {
        goto again; // don't generate empty token
      }
      return Token(Token::Kind::Text, text);
    }
    case State::ExpressionStart: {
      state = State::ExpressionBody;
      pos += config.expression_open.size();
      return make_token(Token::Kind::ExpressionOpen);
    }
    case State::ExpressionStartForceLstrip: {
      state = State::ExpressionBody;
      pos += config.expression_open_force_lstrip.size();
      return make_token(Token::Kind::ExpressionOpen);
    }
    case State::LineStart: {
      state = State::LineBody;
      pos += config.line_statement.size();
      return make_token(Token::Kind::LineStatementOpen);
    }
    case State::StatementStart: {
      state = State::StatementBody;
      pos += config.statement_open.size();
      return make_token(Token::Kind::StatementOpen);
    }
    case State::StatementStartNoLstrip: {
      state = State::StatementBody;
      pos += config.statement_open_no_lstrip.size();
      return make_token(Token::Kind::StatementOpen);
    }
    case State::StatementStartForceLstrip: {
      state = State::StatementBody;
      pos += config.statement_open_force_lstrip.size();
      return make_token(Token::Kind::StatementOpen);
    }
    case State::CommentStart: {
      state = State::CommentBody;
      pos += config.comment_open.size();
      return make_token(Token::Kind::CommentOpen);
    }
    case State::CommentStartForceLstrip: {
      state = State::CommentBody;
      pos += config.comment_open_force_lstrip.size();
      return make_token(Token::Kind::CommentOpen);
    }
    case State::ExpressionBody:
      return scan_body(config.expression_close, Token::Kind::ExpressionClose, config.expression_close_force_rstrip);
    case State::LineBody:
      return scan_body("\n", Token::Kind::LineStatementClose);
    case State::StatementBody:
      return scan_body(config.statement_close, Token::Kind::StatementClose, config.statement_close_force_rstrip, config.trim_blocks);
    case State::CommentBody: {
      // fast-scan to comment close
      const size_t end = m_in.substr(pos).find(config.comment_close);
      if (end == std::string_view::npos) {
        pos = m_in.size();
        return make_token(Token::Kind::Eof);
      }

      // Check for trim pattern
      const bool must_rstrip = inja::string_view::starts_with(m_in.substr(pos + end - 1), config.comment_close_force_rstrip);

      // return the entire comment in the close token
      state = State::Text;
      pos += end + config.comment_close.size();
      Token tok = make_token(Token::Kind::CommentClose);

      if (must_rstrip || config.trim_blocks) {
        skip_whitespaces_and_first_newline();
      }
      return tok;
    }
    }
  }

  const LexerConfig& get_config() const {
    return config;
  }
};

} // namespace inja

#endif // INCLUDE_INJA_LEXER_HPP_

// #include "node.hpp"

// #include "template.hpp"

// #include "throw.hpp"

// #include "token.hpp"


namespace inja {

/*!
 * \brief Class for parsing an inja Template.
 */
class Parser {
  using Arguments = std::vector<std::shared_ptr<ExpressionNode>>;
  using OperatorStack = std::stack<std::shared_ptr<FunctionNode>>;

  const ParserConfig& config;

  Lexer lexer;
  TemplateStorage& template_storage;
  const FunctionStorage& function_storage;

  Token tok, peek_tok;
  bool have_peek_tok {false};

  std::string_view literal_start;

  BlockNode* current_block {nullptr};
  ExpressionListNode* current_expression_list {nullptr};

  std::stack<IfStatementNode*> if_statement_stack;
  std::stack<ForStatementNode*> for_statement_stack;
  std::stack<BlockStatementNode*> block_statement_stack;
  std::stack<MacroStatementNode*> macro_statement_stack;

  void throw_parser_error(const std::string& message) const {
    INJA_THROW(ParserError(message, lexer.current_position()));
  }

  void get_next_token() {
    if (have_peek_tok) {
      tok = peek_tok;
      have_peek_tok = false;
    } else {
      tok = lexer.scan();
    }
  }

  void get_peek_token() {
    if (!have_peek_tok) {
      peek_tok = lexer.scan();
      have_peek_tok = true;
    }
  }

  void add_literal(Arguments &arguments, const char* content_ptr) {
    const std::string_view data_text(literal_start.data(), tok.text.data() - literal_start.data() + tok.text.size());
    arguments.emplace_back(std::make_shared<LiteralNode>(data_text, data_text.data() - content_ptr));
  }

  void add_operator(Arguments &arguments, OperatorStack &operator_stack) {
    auto function = operator_stack.top();
    operator_stack.pop();

    if (static_cast<int>(arguments.size()) < function->number_args) {
      throw_parser_error("too few arguments in expression for operator '" + function->name + "'");
    }

    for (int i = 0; i < function->number_args; ++i) {
      function->arguments.insert(function->arguments.begin(), arguments.back());
      arguments.pop_back();
    }
    arguments.emplace_back(function);
  }

  void add_to_template_storage(const std::filesystem::path& path, std::string& template_name) {
    if (template_storage.find(template_name) != template_storage.end()) {
      return;
    }

    const std::string original_name = template_name;

    if (config.search_included_templates_in_files) {
      // Build the relative path
      template_name = (path / original_name).string();
      if (template_name.compare(0, 2, "./") == 0) {
        template_name.erase(0, 2);
      }

      if (template_storage.find(template_name) == template_storage.end()) {
        // Load file
        std::ifstream file;
        file.open(template_name);
        if (!file.fail()) {
          const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

          auto include_template = Template(text);
          template_storage.emplace(template_name, include_template);
          parse_into_template(template_storage[template_name], template_name);
          return;
        } else if (!config.include_callback) {
          INJA_THROW(FileError("failed accessing file at '" + template_name + "'"));
        }
      }
    }

    // Try include callback
    if (config.include_callback) {
      auto include_template = config.include_callback(path, original_name);
      template_storage.emplace(template_name, include_template);
    }
  }

  std::string parse_filename() const {
    if (tok.kind != Token::Kind::String) {
      throw_parser_error("expected string, got '" + tok.describe() + "'");
    }

    if (tok.text.length() < 2) {
      throw_parser_error("expected filename, got '" + static_cast<std::string>(tok.text) + "'");
    }

    // Remove first and last character ""
    return std::string {tok.text.substr(1, tok.text.length() - 2)};
  }

  bool parse_expression(Template& tmpl, Token::Kind closing) {
    current_expression_list->root = parse_expression(tmpl);
    return tok.kind == closing;
  }

  std::shared_ptr<ExpressionNode> parse_expression(Template& tmpl) {
    size_t current_bracket_level {0};
    size_t current_brace_level {0};
    Arguments arguments;
    OperatorStack operator_stack;

    while (tok.kind != Token::Kind::Eof) {
      // Literals
      switch (tok.kind) {
      case Token::Kind::String: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          literal_start = tok.text;
          add_literal(arguments, tmpl.content.c_str());
        }
      } break;
      case Token::Kind::Number: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          literal_start = tok.text;
          add_literal(arguments, tmpl.content.c_str());
        }
      } break;
      case Token::Kind::LeftBracket: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          literal_start = tok.text;
        }
        current_bracket_level += 1;
      } break;
      case Token::Kind::LeftBrace: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          literal_start = tok.text;
        }
        current_brace_level += 1;
      } break;
      case Token::Kind::RightBracket: {
        if (current_bracket_level == 0) {
          throw_parser_error("unexpected ']'");
        }

        current_bracket_level -= 1;
        if (current_brace_level == 0 && current_bracket_level == 0) {
          add_literal(arguments, tmpl.content.c_str());
        }
      } break;
      case Token::Kind::RightBrace: {
        if (current_brace_level == 0) {
          throw_parser_error("unexpected '}'");
        }

        current_brace_level -= 1;
        if (current_brace_level == 0 && current_bracket_level == 0) {
          add_literal(arguments, tmpl.content.c_str());
        }
      } break;
      case Token::Kind::Id: {
        get_peek_token();

        // Data Literal
        if (tok.text == static_cast<decltype(tok.text)>("true") || tok.text == static_cast<decltype(tok.text)>("false") ||
            tok.text == static_cast<decltype(tok.text)>("null")) {
          if (current_brace_level == 0 && current_bracket_level == 0) {
            literal_start = tok.text;
            add_literal(arguments, tmpl.content.c_str());
          }

          // Operator
        } else if (tok.text == "and" || tok.text == "or" || tok.text == "in" || tok.text == "not") {
          goto parse_operator;

          // Functions
        } else if (peek_tok.kind == Token::Kind::LeftParen) {
          auto func = std::make_shared<FunctionNode>(tok.text, tok.text.data() - tmpl.content.c_str());
          get_next_token();
          do {
            get_next_token();
            auto expr = parse_expression(tmpl);
            if (!expr) {
              break;
            }
            func->number_args += 1;
            func->arguments.emplace_back(expr);
          } while (tok.kind == Token::Kind::Comma);
          if (tok.kind != Token::Kind::RightParen) {
            throw_parser_error("expected right parenthesis, got '" + tok.describe() + "'");
          }

          auto function_data = function_storage.find_function(func->name, func->number_args);
          if (function_data.operation == FunctionStorage::Operation::None) {
            const auto macro_it = tmpl.macro_storage.find(std::make_pair(func->name, func->number_args));
            if (macro_it == tmpl.macro_storage.end()) {
              throw_parser_error("unknown function " + func->name);
            }
            func->operation = FunctionStorage::Operation::Macro;
          } else {
            func->operation = function_data.operation;
            if (function_data.operation == FunctionStorage::Operation::Callback) {
              func->callback = function_data.callback;
            }
          }
          arguments.emplace_back(func);

          // Variables
        } else {
          arguments.emplace_back(std::make_shared<DataNode>(static_cast<std::string>(tok.text), tok.text.data() - tmpl.content.c_str()));
        }

        // Operators
      } break;
      case Token::Kind::Dot:
        if (arguments.empty()) {
          arguments.emplace_back(std::make_shared<DataNode>("", tok.text.data() - tmpl.content.c_str()));
          break;
        }
        [[fallthrough]];
      case Token::Kind::Equal:
      case Token::Kind::NotEqual:
      case Token::Kind::GreaterThan:
      case Token::Kind::GreaterEqual:
      case Token::Kind::LessThan:
      case Token::Kind::LessEqual:
      case Token::Kind::Plus:
      case Token::Kind::Minus:
      case Token::Kind::Times:
      case Token::Kind::Slash:
      case Token::Kind::Power:
      case Token::Kind::Percent: {

      parse_operator:
        FunctionStorage::Operation operation;
        switch (tok.kind) {
        case Token::Kind::Id: {
          if (tok.text == "and") {
            operation = FunctionStorage::Operation::And;
          } else if (tok.text == "or") {
            operation = FunctionStorage::Operation::Or;
          } else if (tok.text == "in") {
            operation = FunctionStorage::Operation::In;
          } else if (tok.text == "not") {
            operation = FunctionStorage::Operation::Not;
          } else {
            throw_parser_error("unknown operator in parser.");
          }
        } break;
        case Token::Kind::Equal: {
          operation = FunctionStorage::Operation::Equal;
        } break;
        case Token::Kind::NotEqual: {
          operation = FunctionStorage::Operation::NotEqual;
        } break;
        case Token::Kind::GreaterThan: {
          operation = FunctionStorage::Operation::Greater;
        } break;
        case Token::Kind::GreaterEqual: {
          operation = FunctionStorage::Operation::GreaterEqual;
        } break;
        case Token::Kind::LessThan: {
          operation = FunctionStorage::Operation::Less;
        } break;
        case Token::Kind::LessEqual: {
          operation = FunctionStorage::Operation::LessEqual;
        } break;
        case Token::Kind::Plus: {
          operation = FunctionStorage::Operation::Add;
        } break;
        case Token::Kind::Minus: {
          operation = FunctionStorage::Operation::Subtract;
        } break;
        case Token::Kind::Times: {
          operation = FunctionStorage::Operation::Multiplication;
        } break;
        case Token::Kind::Slash: {
          operation = FunctionStorage::Operation::Division;
        } break;
        case Token::Kind::Power: {
          operation = FunctionStorage::Operation::Power;
        } break;
        case Token::Kind::Percent: {
          operation = FunctionStorage::Operation::Modulo;
        } break;
        case Token::Kind::Dot: {
          operation = FunctionStorage::Operation::AtId;
        } break;
        default: {
          throw_parser_error("unknown operator in parser.");
        }
        }
        auto function_node = std::make_shared<FunctionNode>(operation, tok.text.data() - tmpl.content.c_str());

        while (!operator_stack.empty() &&
               ((operator_stack.top()->precedence > function_node->precedence) ||
                (operator_stack.top()->precedence == function_node->precedence && function_node->associativity == FunctionNode::Associativity::Left))) {
          add_operator(arguments, operator_stack);
        }

        operator_stack.emplace(function_node);
      } break;
      case Token::Kind::Comma: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          goto break_loop;
        }
      } break;
      case Token::Kind::Colon: {
        if (current_brace_level == 0 && current_bracket_level == 0) {
          throw_parser_error("unexpected ':'");
        }
      } break;
      case Token::Kind::LeftParen: {
        get_next_token();
        auto expr = parse_expression(tmpl);
        if (tok.kind != Token::Kind::RightParen) {
            throw_parser_error("expected right parenthesis, got '" + tok.describe() + "'");
        }
        if (!expr) {
          throw_parser_error("empty expression in parentheses");
        }
        arguments.emplace_back(expr);
      } break;

      // parse function call pipe syntax
      case Token::Kind::Pipe: {
        // get function name
        get_next_token();
        if (tok.kind != Token::Kind::Id) {
          throw_parser_error("expected function name, got '" + tok.describe() + "'");
        }
        auto func = std::make_shared<FunctionNode>(tok.text, tok.text.data() - tmpl.content.c_str());
        // add first parameter as last value from arguments
        func->number_args += 1;
        func->arguments.emplace_back(arguments.back());
        arguments.pop_back();
        get_peek_token();
        if (peek_tok.kind == Token::Kind::LeftParen) {
          get_next_token();
          // parse additional parameters
          do {
            get_next_token();
            auto expr = parse_expression(tmpl);
            if (!expr) {
              break;
            }
            func->number_args += 1;
            func->arguments.emplace_back(expr);
          } while (tok.kind == Token::Kind::Comma);
          if (tok.kind != Token::Kind::RightParen) {
            throw_parser_error("expected right parenthesis, got '" + tok.describe() + "'");
          }
        }
        // search store for defined function with such name and number of args
        auto function_data = function_storage.find_function(func->name, func->number_args);
        if (function_data.operation == FunctionStorage::Operation::None) {
          const auto macro_it = tmpl.macro_storage.find(std::make_pair(func->name, func->number_args));
          if (macro_it == tmpl.macro_storage.end()) {
            throw_parser_error("unknown function " + func->name);
          }
          func->operation = FunctionStorage::Operation::Macro;
        } else {
          func->operation = function_data.operation;
          if (function_data.operation == FunctionStorage::Operation::Callback) {
            func->callback = function_data.callback;
          }
        }
        arguments.emplace_back(func);
      } break;
      default:
        goto break_loop;
      }

      get_next_token();
    }

  break_loop:
    while (!operator_stack.empty()) {
      add_operator(arguments, operator_stack);
    }

    std::shared_ptr<ExpressionNode> expr;
    if (arguments.size() == 1) {
      expr = arguments[0];
      arguments = {};
    } else if (arguments.size() > 1) {
      throw_parser_error("malformed expression");
    }
    return expr;
  }

  bool parse_statement(Template& tmpl, Token::Kind closing, const std::filesystem::path& path) {
    if (tok.kind != Token::Kind::Id) {
      return false;
    }

    if (tok.text == static_cast<decltype(tok.text)>("if")) {
      get_next_token();

      auto if_statement_node = std::make_shared<IfStatementNode>(current_block, tok.text.data() - tmpl.content.c_str());
      current_block->nodes.emplace_back(if_statement_node);
      if_statement_stack.emplace(if_statement_node.get());
      current_block = &if_statement_node->true_statement;
      current_expression_list = &if_statement_node->condition;

      if (!parse_expression(tmpl, closing)) {
        return false;
      }
    } else if (tok.text == static_cast<decltype(tok.text)>("else")) {
      if (if_statement_stack.empty()) {
        throw_parser_error("else without matching if");
      }
      auto& if_statement_data = if_statement_stack.top();
      get_next_token();

      if_statement_data->has_false_statement = true;
      current_block = &if_statement_data->false_statement;

      // Chained else if
      if (tok.kind == Token::Kind::Id && tok.text == static_cast<decltype(tok.text)>("if")) {
        get_next_token();

        auto if_statement_node = std::make_shared<IfStatementNode>(true, current_block, tok.text.data() - tmpl.content.c_str());
        current_block->nodes.emplace_back(if_statement_node);
        if_statement_stack.emplace(if_statement_node.get());
        current_block = &if_statement_node->true_statement;
        current_expression_list = &if_statement_node->condition;

        if (!parse_expression(tmpl, closing)) {
          return false;
        }
      }
    } else if (tok.text == static_cast<decltype(tok.text)>("endif")) {
      if (if_statement_stack.empty()) {
        throw_parser_error("endif without matching if");
      }

      // Nested if statements
      while (if_statement_stack.top()->is_nested) {
        if_statement_stack.pop();
      }

      auto& if_statement_data = if_statement_stack.top();
      get_next_token();

      current_block = if_statement_data->parent;
      if_statement_stack.pop();
    } else if (tok.text == static_cast<decltype(tok.text)>("block")) {
      get_next_token();

      if (tok.kind != Token::Kind::Id) {
        throw_parser_error("expected block name, got '" + tok.describe() + "'");
      }

      const std::string block_name = static_cast<std::string>(tok.text);

      auto block_statement_node = std::make_shared<BlockStatementNode>(current_block, block_name, tok.text.data() - tmpl.content.c_str());
      current_block->nodes.emplace_back(block_statement_node);
      block_statement_stack.emplace(block_statement_node.get());
      current_block = &block_statement_node->block;
      auto success = tmpl.block_storage.emplace(block_name, block_statement_node);
      if (!success.second) {
        throw_parser_error("block with the name '" + block_name + "' does already exist");
      }

      get_next_token();
    } else if (tok.text == static_cast<decltype(tok.text)>("endblock")) {
      if (block_statement_stack.empty()) {
        throw_parser_error("endblock without matching block");
      }

      auto& block_statement_data = block_statement_stack.top();
      get_next_token();

      current_block = block_statement_data->parent;
      block_statement_stack.pop();
    } else if (tok.text == static_cast<decltype(tok.text)>("for")) {
      get_next_token();

      // options: for a in arr; for a, b in obj
      if (tok.kind != Token::Kind::Id) {
        throw_parser_error("expected id, got '" + tok.describe() + "'");
      }

      Token value_token = tok;
      get_next_token();

      // Object type
      std::shared_ptr<ForStatementNode> for_statement_node;
      if (tok.kind == Token::Kind::Comma) {
        get_next_token();
        if (tok.kind != Token::Kind::Id) {
          throw_parser_error("expected id, got '" + tok.describe() + "'");
        }

        const Token key_token = value_token;
        value_token = tok;
        get_next_token();

        for_statement_node = std::make_shared<ForObjectStatementNode>(static_cast<std::string>(key_token.text), static_cast<std::string>(value_token.text),
                                                                      current_block, tok.text.data() - tmpl.content.c_str());

        // Array type
      } else {
        for_statement_node =
            std::make_shared<ForArrayStatementNode>(static_cast<std::string>(value_token.text), current_block, tok.text.data() - tmpl.content.c_str());
      }

      current_block->nodes.emplace_back(for_statement_node);
      for_statement_stack.emplace(for_statement_node.get());
      current_block = &for_statement_node->body;
      current_expression_list = &for_statement_node->condition;

      if (tok.kind != Token::Kind::Id || tok.text != static_cast<decltype(tok.text)>("in")) {
        throw_parser_error("expected 'in', got '" + tok.describe() + "'");
      }
      get_next_token();

      if (!parse_expression(tmpl, closing)) {
        return false;
      }
    } else if (tok.text == static_cast<decltype(tok.text)>("endfor")) {
      if (for_statement_stack.empty()) {
        throw_parser_error("endfor without matching for");
      }

      auto& for_statement_data = for_statement_stack.top();
      get_next_token();

      current_block = for_statement_data->parent;
      for_statement_stack.pop();
    } else if (tok.text == static_cast<decltype(tok.text)>("include")) {
      get_next_token();

      std::string template_name = parse_filename();
      add_to_template_storage(path, template_name);

      current_block->nodes.emplace_back(std::make_shared<IncludeStatementNode>(template_name, tok.text.data() - tmpl.content.c_str()));

      get_next_token();
    } else if (tok.text == static_cast<decltype(tok.text)>("extends")) {
      get_next_token();

      std::string template_name = parse_filename();
      add_to_template_storage(path, template_name);

      current_block->nodes.emplace_back(std::make_shared<ExtendsStatementNode>(template_name, tok.text.data() - tmpl.content.c_str()));

      get_next_token();
    } else if (tok.text == static_cast<decltype(tok.text)>("set")) {
      get_next_token();

      if (tok.kind != Token::Kind::Id) {
        throw_parser_error("expected variable name, got '" + tok.describe() + "'");
      }

      const std::string key = static_cast<std::string>(tok.text);
      get_next_token();

      auto set_statement_node = std::make_shared<SetStatementNode>(key, tok.text.data() - tmpl.content.c_str());
      current_block->nodes.emplace_back(set_statement_node);
      current_expression_list = &set_statement_node->expression;

      if (tok.text != static_cast<decltype(tok.text)>("=")) {
        throw_parser_error("expected '=', got '" + tok.describe() + "'");
      }
      get_next_token();

      if (!parse_expression(tmpl, closing)) {
        return false;
      }
    } else if (tok.text == static_cast<decltype(tok.text)>("macro")) {
      get_next_token();

      if (tok.kind != Token::Kind::Id) {
        throw_parser_error("expected macro name, got '" + tok.describe() + "'");
      }

      const Token name_token = tok;
      const std::string macro_name = static_cast<std::string>(tok.text);

      get_next_token();
      if (tok.kind != Token::Kind::LeftParen) {
        throw_parser_error("expected left parenthesis, got '" + tok.describe() + "'");
      }

      std::vector<std::string> parameters;
      get_next_token();
      if (tok.kind != Token::Kind::RightParen) {
        for (;;) {
          if (tok.kind != Token::Kind::Id) {
            throw_parser_error("expected parameter name, got '" + tok.describe() + "'");
          }
          parameters.emplace_back(static_cast<std::string>(tok.text));

          get_next_token();
          if (tok.kind == Token::Kind::Comma) {
            get_next_token();
            continue;
          }

          if (tok.kind != Token::Kind::RightParen) {
            throw_parser_error("expected right parenthesis, got '" + tok.describe() + "'");
          }
          break;
        }
      }

      auto macro_statement_node =
          std::make_shared<MacroStatementNode>(current_block, macro_name, name_token.text.data() - tmpl.content.c_str());
      macro_statement_node->parameters = std::move(parameters);

      auto success = tmpl.macro_storage.emplace(std::make_pair(macro_statement_node->name, static_cast<int>(macro_statement_node->parameters.size())),
                                                macro_statement_node);
      if (!success.second) {
        throw_parser_error("macro with the name '" + macro_statement_node->name + "' and arity " +
                           std::to_string(macro_statement_node->parameters.size()) + " does already exist");
      }

      current_block->nodes.emplace_back(macro_statement_node);
      macro_statement_stack.emplace(macro_statement_node.get());
      current_block = &macro_statement_node->body;

      get_next_token();
    } else if (tok.text == static_cast<decltype(tok.text)>("endmacro")) {
      if (macro_statement_stack.empty()) {
        throw_parser_error("endmacro without matching macro");
      }

      auto& macro_statement_data = macro_statement_stack.top();
      get_next_token();

      current_block = macro_statement_data->parent;
      macro_statement_stack.pop();
    } else {
      return false;
    }
    return true;
  }

  void parse_into(Template& tmpl, const std::filesystem::path& path) {
    lexer.start(tmpl.content);
    current_block = &tmpl.root;

    for (;;) {
      get_next_token();
      switch (tok.kind) {
      case Token::Kind::Eof: {
        if (!if_statement_stack.empty()) {
          throw_parser_error("unmatched if");
        }
        if (!for_statement_stack.empty()) {
          throw_parser_error("unmatched for");
        }
        if (!macro_statement_stack.empty()) {
          throw_parser_error("unmatched macro");
        }
      }
        current_block = nullptr;
        return;
      case Token::Kind::Text: {
        current_block->nodes.emplace_back(std::make_shared<TextNode>(tok.text.data() - tmpl.content.c_str(), tok.text.size()));
      } break;
      case Token::Kind::StatementOpen: {
        get_next_token();
        if (!parse_statement(tmpl, Token::Kind::StatementClose, path)) {
          throw_parser_error("expected statement, got '" + tok.describe() + "'");
        }
        if (tok.kind != Token::Kind::StatementClose) {
          throw_parser_error("expected statement close, got '" + tok.describe() + "'");
        }
      } break;
      case Token::Kind::LineStatementOpen: {
        get_next_token();
        if (!parse_statement(tmpl, Token::Kind::LineStatementClose, path)) {
          throw_parser_error("expected statement, got '" + tok.describe() + "'");
        }
        if (tok.kind != Token::Kind::LineStatementClose && tok.kind != Token::Kind::Eof) {
          throw_parser_error("expected line statement close, got '" + tok.describe() + "'");
        }
      } break;
      case Token::Kind::ExpressionOpen: {
        get_next_token();

        auto expression_list_node = std::make_shared<ExpressionListNode>(tok.text.data() - tmpl.content.c_str());
        current_block->nodes.emplace_back(expression_list_node);
        current_expression_list = expression_list_node.get();

        if (!parse_expression(tmpl, Token::Kind::ExpressionClose)) {
          throw_parser_error("expected expression close, got '" + tok.describe() + "'");
        }
      } break;
      case Token::Kind::CommentOpen: {
        get_next_token();
        if (tok.kind != Token::Kind::CommentClose) {
          throw_parser_error("expected comment close, got '" + tok.describe() + "'");
        }
      } break;
      default: {
        throw_parser_error("unexpected token '" + tok.describe() + "'");
      } break;
      }
    }
  }

public:
  explicit Parser(const ParserConfig& parser_config, const LexerConfig& lexer_config, TemplateStorage& template_storage,
                  const FunctionStorage& function_storage)
      : config(parser_config), lexer(lexer_config), template_storage(template_storage), function_storage(function_storage) {}

  Template parse(std::string_view input, const std::filesystem::path& path) {
    auto result = Template(std::string(input));
    parse_into(result, path);
    return result;
  }

  void parse_into_template(Template& tmpl, const std::filesystem::path& filename) {
    auto sub_parser = Parser(config, lexer.get_config(), template_storage, function_storage);
    sub_parser.parse_into(tmpl, filename.parent_path());
  }

  static std::string load_file(const std::filesystem::path& filename) {
    std::ifstream file;
    file.open(filename);
    if (file.fail()) {
      INJA_THROW(FileError("failed accessing file at '" + filename.string() + "'"));
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return text;
  }
};

} // namespace inja

#endif // INCLUDE_INJA_PARSER_HPP_

// #include "renderer.hpp"
#ifndef INCLUDE_INJA_RENDERER_HPP_
#define INCLUDE_INJA_RENDERER_HPP_

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <ostream>
#include <sstream>
#include <stack>
#include <string>
#include <utility>
#include <vector>

// #include "config.hpp"

// #include "exceptions.hpp"

// #include "function_storage.hpp"

// #include "node.hpp"

// #include "template.hpp"

// #include "throw.hpp"

// #include "utils.hpp"

#include "spdlog/spdlog.h"

namespace inja {

/*!
@brief Escapes HTML
*/
inline std::string htmlescape(const std::string& data) {
  std::string buffer;
  buffer.reserve(static_cast<size_t>(1.1 * data.size()));
  for (size_t pos = 0; pos != data.size(); ++pos) {
    switch (data[pos]) {
      case '&':  buffer.append("&amp;");       break;
      case '\"': buffer.append("&quot;");      break;
      case '\'': buffer.append("&apos;");      break;
      case '<':  buffer.append("&lt;");        break;
      case '>':  buffer.append("&gt;");        break;
      default:   buffer.append(&data[pos], 1); break;
    }
  }
  return buffer;
}

namespace detail {

struct LoopFrameSketch {
  ConstNodeRef value;
  ::ryml::csubstr key {};
  size_t index {0};
  size_t size {0};
  bool has_key {false};
  bool first {true};
  bool last {true};

  bool is_first() const {
    return first;
  }

  bool is_last() const {
    return last;
  }
};

class LoopFrameStackSketch {
  std::vector<LoopFrameSketch> frames;

public:
  void clear() {
    frames.clear();
  }

  size_t depth() const {
    return frames.size();
  }

  void push_existing(const LoopFrameSketch& frame) {
    frames.push_back(frame);
  }

  void push_array(ConstNodeRef value, size_t index, size_t size) {
    frames.push_back(LoopFrameSketch {
        value,
        ::ryml::csubstr {},
        index,
        size,
        false,
        index == 0,
        size == 0 ? true : (index + 1 >= size),
    });
  }

  void push_object(::ryml::csubstr key, ConstNodeRef value, size_t index, size_t size) {
    frames.push_back(LoopFrameSketch {
        value,
        key,
        index,
        size,
        true,
        index == 0,
        size == 0 ? true : (index + 1 >= size),
    });
  }

  void pop() {
    if (!frames.empty()) {
      frames.pop_back();
    }
  }

  const LoopFrameSketch* current() const {
    if (frames.empty()) {
      return nullptr;
    }
    return &frames.back();
  }

  const LoopFrameSketch* parent(size_t levels = 1) const {
    if (levels == 0 || frames.size() <= levels) {
      return nullptr;
    }
    return &frames[frames.size() - 1 - levels];
  }
};

} // namespace detail

/*!
 * \brief Class for rendering a Template with data.
 */
class Renderer : public NodeVisitor {
  using Op = FunctionStorage::Operation;

  const RenderConfig config;
  const TemplateStorage& template_storage;
  const FunctionStorage& function_storage;

  const Template* current_template {nullptr};
  size_t current_level {0};
  std::vector<const Template*> template_stack;
  std::vector<const BlockStatementNode*> block_statement_stack;

  ConstNodeRef data_input;
  std::ostream* output_stream {nullptr};

  NodeRef template_scope_data;
  NodeRef additional_data;
  NodeRef current_loop_data;
  NodeRef tmp_sequence;
  detail::LoopFrameStackSketch loop_frames;

  std::vector<NativeNodeRef> data_eval_stack;
  std::stack<const DataNode*> not_found_stack;

  bool break_rendering {false};
  size_t macro_call_depth {0};

  static constexpr size_t MAX_MACRO_CALL_DEPTH = 1000;

  static bool nodes_equal(const NativeNodeRef& lhs, const NativeNodeRef& rhs) {
    if (node_is_null(lhs.node) && node_is_null(rhs.node)) {
      return true;
    }
    const auto l_num = native_to_double(lhs);
    const auto r_num = native_to_double(rhs);
    if (l_num && r_num) {
      return *l_num == *r_num;
    }
    const auto l_bool = native_to_bool(lhs);
    const auto r_bool = native_to_bool(rhs);
    if (l_bool && r_bool) {
      return *l_bool == *r_bool;
    }
    return native_to_string(lhs) == native_to_string(rhs);
  }

  static bool node_less(const NativeNodeRef& lhs, const NativeNodeRef& rhs) {
    const auto l_num = native_to_double(lhs);
    const auto r_num = native_to_double(rhs);
    if (l_num && r_num) {
      return *l_num < *r_num;
    }
    return native_to_string(lhs) < native_to_string(rhs);
  }

  void set_loop_meta_bool(NodeRef node, std::string_view key, bool value) {
    auto field = find_child_by_key(node, to_csubstr(key));
    if (!field.valid()) {
      const auto key_sub = to_csubstr(key);
      field = node.append_child();
      field << ryml::Key(key_sub);
    }
    field << (value ? "true" : "false");
  }

  void set_loop_meta_int(NodeRef node, std::string_view key, int64_t value) {
    auto field = find_child_by_key(node, to_csubstr(key));
    if (!field.valid()) {
      const auto key_sub = to_csubstr(key);
      field = node.append_child();
      field << ryml::Key(key_sub);
    }
    field << value;
  }

  void set_loop_meta_key(NodeRef node, ::ryml::csubstr value) {
    auto field = find_child_by_key(node, to_csubstr("key"));
    if (!field.valid()) {
      field = node.append_child();
      field << ryml::Key("key");
    }
    field << value;
  }

  void write_loop_frame(NodeRef node, const detail::LoopFrameSketch& frame) {
    set_loop_meta_bool(node, "is_first", frame.is_first());
    set_loop_meta_bool(node, "is_last", frame.is_last());
    set_loop_meta_int(node, "index", static_cast<int64_t>(frame.index));
    set_loop_meta_int(node, "index1", static_cast<int64_t>(frame.index + 1));
    if (frame.has_key) {
      set_loop_meta_key(node, frame.key);
    }
  }

  void sync_current_loop_data_from_frames() {
    current_loop_data.clear_children();

    const auto* current = loop_frames.current();
    if (!current) {
      return;
    }

    write_loop_frame(current_loop_data, *current);

    size_t level = 1;
    const auto* parent_frame = loop_frames.parent(level);
    auto cursor = current_loop_data;
    while (parent_frame) {
      cursor = ensure_child_map(cursor, "parent");
      write_loop_frame(cursor, *parent_frame);
      ++level;
      parent_frame = loop_frames.parent(level);
    }
  }

  detail::LoopFrameSketch parse_loop_frame(ConstNodeRef node) {
    detail::LoopFrameSketch frame;

    const auto idx = node_to_int(find_child_by_key(node, to_csubstr("index")));
    if (idx && *idx >= 0) {
      frame.index = static_cast<size_t>(*idx);
    }

    frame.first = node_to_bool(find_child_by_key(node, to_csubstr("is_first"))).value_or(frame.index == 0);
    frame.last = node_to_bool(find_child_by_key(node, to_csubstr("is_last"))).value_or(true);

    const auto key_node = find_child_by_key(node, to_csubstr("key"));
    if (key_node.valid() && !node_is_null(key_node)) {
      frame.has_key = true;
      frame.key = key_node.val();
    }

    frame.size = frame.last ? frame.index + 1 : frame.index + 2;
    return frame;
  }

  void restore_loop_frames_from_current_scope_if_needed() {
    if (loop_frames.depth() > 0 || !current_loop_data.has_children()) {
      return;
    }

    std::vector<detail::LoopFrameSketch> chain;
    auto cursor = ConstNodeRef(current_loop_data.tree(), current_loop_data.id());
    while (cursor.valid()) {
      chain.push_back(parse_loop_frame(cursor));
      cursor = find_child_by_key(cursor, to_csubstr("parent"));
    }

    std::reverse(chain.begin(), chain.end());
    for (const auto& frame : chain) {
      loop_frames.push_existing(frame);
    }
  }

  ConstNodeRef resolve_global_scope_reference(const DataNode& node) const {
    constexpr std::string_view global_scope_name = "scope.global";
    constexpr std::string_view global_scope_prefix = "scope.global.";

    const std::string_view name = node.name;
    if (name == global_scope_name) {
      return ConstNodeRef(template_scope_data.tree(), template_scope_data.id());
    }
    if (!inja::string_view::starts_with(name, global_scope_prefix)) {
      return ConstNodeRef{};
    }

    const auto suffix = name.substr(global_scope_prefix.size());
    const auto ptr = Pointer(DataNode::convert_dot_to_ptr(suffix));
    if (const auto from_template_scope = resolve_pointer(template_scope_data, ptr); from_template_scope.valid()) {
      return from_template_scope;
    }
    return resolve_pointer(data_input, ptr);
  }

  void print_data(const NativeNodeRef& value) {
    if (!value.valid() || node_is_null(value.node)) {
      return;
    }
    if (value.node.has_children() && !value.node.is_val()) {
      const auto emitted = ryml::as_json(*value.node.tree(), value.node.id());
      *output_stream << emitted;
      return;
    }
    const std::string text = native_to_string(value);
    if (config.html_autoescape) {
      *output_stream << htmlescape(text);
    } else {
      *output_stream << text;
    }
  }

  NativeNodeRef eval_expression_list(const ExpressionListNode& expression_list) {
    if (!expression_list.root) {
      throw_renderer_error("empty expression", expression_list);
    }

    expression_list.root->accept(*this);

    if (data_eval_stack.empty()) {
      throw_renderer_error("empty expression", expression_list);
    } else if (data_eval_stack.size() != 1) {
      throw_renderer_error("malformed expression", expression_list);
    }

    const auto result = data_eval_stack.back();
    data_eval_stack.pop_back();

    if (!result.valid()) {
      if (!not_found_stack.empty()) {
        const auto missing_node = not_found_stack.top();
        not_found_stack.pop();
        throw_renderer_error("variable '" + static_cast<std::string>(missing_node->name) + "' not found", *missing_node);
      }
    }
    return result;
  }

  void throw_renderer_error(const std::string& message, const AstNode& node) {
    const SourceLocation loc = get_source_location(current_template->content, node.pos);
    //INJA_THROW(RenderError(message, loc));
    spdlog::debug("[inja.exception.renderer] (at " + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ") " + message);
  }

  template <class T>
  void make_result(const T& result) {
    auto node = tmp_sequence.append_child();
    if constexpr (std::is_same_v<T, bool>) {
      node << (result ? "true" : "false");
    } else {
      node << result;
    }
    data_eval_stack.emplace_back(node);
  }

  void make_null_result() {
    auto node = tmp_sequence.append_child();
    node = nullptr;
    data_eval_stack.emplace_back(node);
  }

  template <size_t N, size_t N_start = 0, bool throw_not_found = true>
  std::array<NativeNodeRef, N> get_arguments(const FunctionNode& node) {
    if (node.arguments.size() < N_start + N) {
      throw_renderer_error("function needs " + std::to_string(N_start + N) + " variables, but has only found " + std::to_string(node.arguments.size()), node);
    }

    for (size_t i = N_start; i < N_start + N; i += 1) {
      node.arguments[i]->accept(*this);
    }

    if (data_eval_stack.size() < N) {
      throw_renderer_error("function needs " + std::to_string(N) + " variables, but has only found " + std::to_string(data_eval_stack.size()), node);
    }

    std::array<NativeNodeRef, N> result;
    for (size_t i = 0; i < N; i += 1) {
      result[N - i - 1] = data_eval_stack.back();
      data_eval_stack.pop_back();

      if (!result[N - i - 1].valid()) {
        const auto data_node = not_found_stack.top();
        not_found_stack.pop();

        if (throw_not_found) {
          throw_renderer_error("variable '" + static_cast<std::string>(data_node->name) + "' not found", *data_node);
        }
      }
    }
    return result;
  }

  template <bool throw_not_found = true>
  Arguments get_argument_vector(const FunctionNode& node) {
    const size_t N = node.arguments.size();
    for (const auto& a : node.arguments) {
      a->accept(*this);
    }

    if (data_eval_stack.size() < N) {
      throw_renderer_error("function needs " + std::to_string(N) + " variables, but has only found " + std::to_string(data_eval_stack.size()), node);
    }

    Arguments result {N};
    for (size_t i = 0; i < N; i += 1) {
      result[N - i - 1] = data_eval_stack.back().node;
      data_eval_stack.pop_back();

      // if (!result[N - i - 1].valid()) {
      //   const auto data_node = not_found_stack.top();
      //   not_found_stack.pop();

      //   if (throw_not_found) {
      //     throw_renderer_error("variable '" + static_cast<std::string>(data_node->name) + "' not found", *data_node);
      //   }
      // }
    }
    return result;
  }

  Arguments get_macro_argument_vector(const FunctionNode& node) {
    const size_t N = node.arguments.size();
    for (const auto& a : node.arguments) {
      a->accept(*this);
    }

    if (data_eval_stack.size() < N) {
      throw_renderer_error("function needs " + std::to_string(N) + " variables, but has only found " + std::to_string(data_eval_stack.size()), node);
    }

    Arguments result {N};
    for (size_t i = 0; i < N; i += 1) {
      const auto value = data_eval_stack.back();
      data_eval_stack.pop_back();
      result[N - i - 1] = value.node;

      if (!value.valid()) {
        if (!not_found_stack.empty()) {
          const auto data_node = not_found_stack.top();
          not_found_stack.pop();
          throw_renderer_error("variable '" + static_cast<std::string>(data_node->name) + "' not found", *data_node);
        }
        throw_renderer_error("invalid macro argument", node);
      }
    }
    return result;
  }

  std::string evaluate_macro(const FunctionNode& node) {
    if (macro_call_depth >= MAX_MACRO_CALL_DEPTH) {
      throw_renderer_error("maximum macro call depth exceeded", node);
      return std::string {};
    }

    const auto macro_it = current_template->macro_storage.find(std::make_pair(node.name, node.number_args));
    if (macro_it == current_template->macro_storage.end()) {
      throw_renderer_error("macro '" + node.name + "' with arity " + std::to_string(node.number_args) + " not found", node);
      return std::string {};
    }

    const auto args = get_macro_argument_vector(node);
    const auto& macro_node = *macro_it->second;

    auto parent_scope = additional_data.parent();
    const size_t macro_scope_id = additional_data.tree()->duplicate(additional_data.tree(), additional_data.id(), parent_scope.id(), NONE);
    auto macro_scope = NodeRef(additional_data.tree(), macro_scope_id);

    for (size_t i = 0; i < macro_node.parameters.size(); ++i) {
      if (i >= args.size() || !args[i].valid()) {
        auto target = macro_scope[Pointer(DataNode::convert_dot_to_ptr(macro_node.parameters[i]))];
        target = nullptr;
        continue;
      }
      set_child_from_node(macro_scope, macro_node.parameters[i], args[i]);
    }

    auto previous_additional_data = additional_data;
    auto previous_current_loop_data = current_loop_data;
    auto previous_tmp_sequence = tmp_sequence;
    auto previous_output_stream = output_stream;
    const bool previous_break_rendering = break_rendering;

    std::ostringstream macro_output;
    additional_data = macro_scope;
    current_loop_data = find_child_by_key(additional_data, to_csubstr("loop"));
    if (!current_loop_data.valid()) {
      current_loop_data = additional_data.append_child() << ryml::key("loop");
    }
    current_loop_data |= ryml::MAP;
    current_loop_data.clear_children();
    tmp_sequence = find_child_by_key(additional_data, to_csubstr("_tmp"));
    if (!tmp_sequence.valid()) {
      tmp_sequence = additional_data.append_child() << ryml::key("_tmp");
    }
    tmp_sequence |= ryml::SEQ;
    tmp_sequence.clear_children();
    output_stream = &macro_output;
    break_rendering = false;

    macro_call_depth += 1;
    macro_node.body.accept(*this);
    macro_call_depth -= 1;

    output_stream = previous_output_stream;
    break_rendering = previous_break_rendering;
    additional_data = previous_additional_data;
    current_loop_data = previous_current_loop_data;
    tmp_sequence = previous_tmp_sequence;

    additional_data.tree()->remove(macro_scope_id);
    return macro_output.str();
  }

  void visit(const BlockNode& node) override {
    for (const auto& n : node.nodes) {
      n->accept(*this);

      if (break_rendering) {
        break;
      }
    }
  }

  void visit(const TextNode& node) override {
    output_stream->write(current_template->content.c_str() + node.pos, node.length);
  }

  void visit(const ExpressionNode&) override {}

  void visit(const LiteralNode& node) override {
    auto dest = tmp_sequence.append_child();
    const ryml::csubstr name = to_csubstr("literal");
    ryml::parse_in_arena(name, to_csubstr(node.text), dest);
    data_eval_stack.emplace_back(dest);
  }

  void visit(const DataNode& node) override {
    if (node.ptr.empty()) {
      // root document
      data_eval_stack.emplace_back(data_input);
    } else if (const auto from_global_scope = resolve_global_scope_reference(node); from_global_scope.valid()) {
      data_eval_stack.emplace_back(from_global_scope);
      return;
    } else if (additional_data.contains(node.ptr)) {
    // const auto from_additional = resolve_pointer(additional_data, node.ptr);
    // if (from_additional.valid()) {
      data_eval_stack.emplace_back(resolve_pointer(additional_data, node.ptr));
      return;
    }

    const auto from_input = resolve_pointer(data_input, node.ptr);
    if (from_input.valid()) {
      data_eval_stack.emplace_back(from_input);
      return;
    }

    const auto function_data = function_storage.find_function(node.name, 0);
    if (function_data.operation == FunctionStorage::Operation::Callback) {
      Arguments empty_args {};
      const auto value = function_data.callback(empty_args, additional_data);
      data_eval_stack.emplace_back(value);
    } else {
      data_eval_stack.emplace_back(ConstNodeRef());
      not_found_stack.emplace(&node);
    }
  }

  void visit(const FunctionNode& node) override {
    switch (node.operation) {
    case Op::Not: {
      const auto args = get_arguments<1>(node);
      make_result(!native_truthy(args[0]));
    } break;
    case Op::And: {
      make_result(native_truthy(get_arguments<1, 0>(node)[0]) && native_truthy(get_arguments<1, 1>(node)[0]));
    } break;
    case Op::Or: {
      make_result(native_truthy(get_arguments<1, 0>(node)[0]) || native_truthy(get_arguments<1, 1>(node)[0]));
    } break;
    case Op::In: {
      const auto args = get_arguments<2>(node);
      bool found = false;
      if (args[1].node.is_seq() || args[1].node.is_map()) {
        for (const auto& child : args[1].node.children()) {
          if (nodes_equal(NativeNodeRef(child), args[0])) {
            found = true;
            break;
          }
        }
      }
      make_result(found);
    } break;
    case Op::Equal: {
      const auto args = get_arguments<2>(node);
      make_result(nodes_equal(args[0], args[1]));
    } break;
    case Op::NotEqual: {
      const auto args = get_arguments<2>(node);
      make_result(!nodes_equal(args[0], args[1]));
    } break;
    case Op::Greater: {
      const auto args = get_arguments<2>(node);
      make_result(node_less(args[1], args[0]));
    } break;
    case Op::GreaterEqual: {
      const auto args = get_arguments<2>(node);
      make_result(!node_less(args[0], args[1]));
    } break;
    case Op::Less: {
      const auto args = get_arguments<2>(node);
      make_result(node_less(args[0], args[1]));
    } break;
    case Op::LessEqual: {
      const auto args = get_arguments<2>(node);
      make_result(!node_less(args[1], args[0]));
    } break;
    case Op::Add: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_double(args[0]);
      const auto rhs_num = native_to_double(args[1]);
      if (lhs_num && rhs_num) {
        make_result(*lhs_num + *rhs_num);
      } else {
        make_result(native_to_string(args[0]) + native_to_string(args[1]));
      }
    } break;
    case Op::Subtract: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_double(args[0]);
      const auto rhs_num = native_to_double(args[1]);
      make_result((lhs_num ? *lhs_num : 0.0) - (rhs_num ? *rhs_num : 0.0));
    } break;
    case Op::Multiplication: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_double(args[0]);
      const auto rhs_num = native_to_double(args[1]);
      make_result((lhs_num ? *lhs_num : 0.0) * (rhs_num ? *rhs_num : 0.0));
    } break;
    case Op::Division: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_double(args[0]);
      const auto rhs_num = native_to_double(args[1]);
      if (!rhs_num || *rhs_num == 0.0) {
        throw_renderer_error("division by zero", node);
      }
      make_result((lhs_num ? *lhs_num : 0.0) / *rhs_num);
    } break;
    case Op::Power: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_double(args[0]);
      const auto rhs_num = native_to_double(args[1]);
      make_result(std::pow(lhs_num ? *lhs_num : 0.0, rhs_num ? *rhs_num : 0.0));
    } break;
    case Op::Modulo: {
      const auto args = get_arguments<2>(node);
      const auto lhs_num = native_to_int(args[0]);
      const auto rhs_num = native_to_int(args[1]);
      make_result((lhs_num && rhs_num && *rhs_num != 0) ? (*lhs_num % *rhs_num) : 0);
    } break;
    case Op::AtId: {
      const auto container = get_arguments<1, 0, false>(node)[0];
      node.arguments[1]->accept(*this);
      if (not_found_stack.empty()) {
        throw_renderer_error("could not find element with given name", node);
      }
      const auto id_node = not_found_stack.top();
      not_found_stack.pop();
      data_eval_stack.pop_back();
      const auto child_ptr = Pointer(DataNode::convert_dot_to_ptr(id_node->name));
      data_eval_stack.emplace_back(resolve_pointer(container.node, child_ptr));
    } break;
    case Op::At: {
      if (node.arguments.size() == 1) {
        const auto arg = get_arguments<1>(node)[0];
        const auto ptr_text = native_to_string(arg);
        if (ptr_text.empty()) {
          data_eval_stack.emplace_back(ConstNodeRef());
        } else {
          const auto ptr = Pointer(ptr_text);
          data_eval_stack.emplace_back(resolve_pointer(additional_data["store"], ptr));
        }
      } else {
        const auto args = get_arguments<2>(node);
        ConstNodeRef container;

        if (args[0].valid() && (args[0].node.is_map() || args[0].node.is_seq())) {
          container = args[0].node;
        } else {
          const auto ptr_text = native_to_string(args[0]);
          if (!ptr_text.empty()) {
            container = resolve_pointer(additional_data["store"], Pointer(ptr_text));
          }
        }

        if (!container.valid()) {
          data_eval_stack.emplace_back(ConstNodeRef());
        } else if (container.is_map()) {
          const auto key = native_to_string(args[1]);
          data_eval_stack.emplace_back(find_child_by_key(container, to_csubstr(key)));
        } else if (container.is_seq()) {
          const auto index = native_to_int(args[1]);
          if (!index || *index < 0 || static_cast<size_t>(*index) >= container.num_children()) {
            data_eval_stack.emplace_back(ConstNodeRef());
          } else {
            data_eval_stack.emplace_back(container.child(static_cast<size_t>(*index)));
          }
        } else {
          data_eval_stack.emplace_back(ConstNodeRef());
        }
      }
    } break;
    case Op::Capitalize: {
      auto result = native_to_string(get_arguments<1>(node)[0]);
      if (!result.empty()) {
        result[0] = static_cast<char>(::toupper(result[0]));
        std::transform(result.begin() + 1, result.end(), result.begin() + 1, [](char c) { return static_cast<char>(::tolower(c)); });
      }
      make_result(result);
    } break;
    case Op::Default: {
      const auto test_arg = get_arguments<1, 0, false>(node)[0];
      if (test_arg.valid()) {
        data_eval_stack.push_back(test_arg);
      } else {
        data_eval_stack.push_back(get_arguments<1, 1>(node)[0]);
      }
    } break;
    case Op::DivisibleBy: {
      const auto args = get_arguments<2>(node);
      const auto divisor = native_to_int(args[1]);
      const auto value = native_to_int(args[0]);
      make_result(divisor && value && *divisor != 0 && (*value % *divisor == 0));
    } break;
    case Op::Even: {
      const auto value = native_to_int(get_arguments<1>(node)[0]);
      make_result(value && (*value % 2 == 0));
    } break;
    case Op::Exists: {
      auto name = native_to_string(get_arguments<1>(node)[0]);
      make_result(resolve_pointer(data_input, Pointer(DataNode::convert_dot_to_ptr(name))).valid());
    } break;
    case Op::ExistsInObject: {
      const auto args = get_arguments<2>(node);
      auto name = native_to_string(args[1]);
      make_result(find_child_by_key(args[0].node, to_csubstr(name)).valid());
    } break;
    case Op::First: {
      const auto value = get_arguments<1>(node)[0];
      if (!value.valid() || value.node.num_children() == 0) {
        data_eval_stack.emplace_back(ConstNodeRef());
      } else {
        data_eval_stack.emplace_back(value.node.child(0));
      }
    } break;
    case Op::Float: {
      const auto value = native_to_double(get_arguments<1>(node)[0]);
      make_result(value ? *value : 0.0);
    } break;
    case Op::Int: {
      const auto value = native_to_double(get_arguments<1>(node)[0]);
      make_result(value ? static_cast<int>(*value) : 0);
    } break;
    case Op::Last: {
      const auto value = get_arguments<1>(node)[0];
      if (!value.valid() || value.node.num_children() == 0) {
        data_eval_stack.emplace_back(ConstNodeRef());
      } else {
        data_eval_stack.emplace_back(value.node.child(value.node.num_children() - 1));
      }
    } break;
    case Op::Length: {
      const auto val = get_arguments<1>(node)[0];
      if (!val.node.valid()){
        make_result(0);
      } else if (val.node.is_val() || val.node.is_keyval()) {
        make_result(native_to_string(val).length());
      } else {
        make_result(val.node.num_children());
      }
    } break;
    case Op::Lower: {
      auto result = native_to_string(get_arguments<1>(node)[0]);
      std::transform(result.begin(), result.end(), result.begin(), [](char c) { return static_cast<char>(::tolower(c)); });
      make_result(result);
    } break;
    case Op::Max: {
      const auto args = get_arguments<1>(node);
      if (!args[0].valid() || args[0].node.num_children() == 0) {
        data_eval_stack.emplace_back(ConstNodeRef());
        break;
      }
      NativeNodeRef best(args[0].node.child(0));
      for (const auto& child : args[0].node.children()) {
        if (node_less(best, NativeNodeRef(child))) {
          best = NativeNodeRef(child);
        }
      }
      data_eval_stack.push_back(best);
    } break;
    case Op::Min: {
      const auto args = get_arguments<1>(node);
      if (!args[0].valid() || args[0].node.num_children() == 0) {
        data_eval_stack.emplace_back(ConstNodeRef());
        break;
      }
      NativeNodeRef best(args[0].node.child(0));
      for (const auto& child : args[0].node.children()) {
        if (node_less(NativeNodeRef(child), best)) {
          best = NativeNodeRef(child);
        }
      }
      data_eval_stack.push_back(best);
    } break;
    case Op::Odd: {
      const auto value = native_to_int(get_arguments<1>(node)[0]);
      make_result(value && (*value % 2 != 0));
    } break;
    case Op::Range: {
      const auto limit = native_to_int(get_arguments<1>(node)[0]);
      auto result = tmp_sequence.append_child();
      result |= ryml::SEQ;
      if (limit && *limit > 0) {
        for (int64_t i = 0; i < *limit; ++i) {
          auto child = result.append_child();
          child << i;
        }
      }
      data_eval_stack.emplace_back(result);
    } break;
    case Op::Replace: {
      const auto args = get_arguments<3>(node);
      auto result = native_to_string(args[0]);
      replace_substring(result, native_to_string(args[1]), native_to_string(args[2]));
      make_result(result);
    } break;
    case Op::Round: {
      const auto args = get_arguments<2>(node);
      const auto precision = native_to_int(args[1]).value_or(0);
      const double value = native_to_double(args[0]).value_or(0.0);
      const double rounded = std::round(value * std::pow(10.0, precision)) / std::pow(10.0, precision);
      if (precision == 0) {
        make_result(static_cast<int>(rounded));
      } else {
        make_result(rounded);
      }
    } break;
    case Op::Sort: {
      const auto value = get_arguments<1>(node)[0];
      auto result = tmp_sequence.append_child();
      result |= ryml::SEQ;
      if (value.valid()) {
        std::vector<ConstNodeRef> children;
        for (const auto& child : value.node.children()) {
          children.push_back(child);
        }
        std::sort(children.begin(), children.end(), [](const ConstNodeRef& lhs, const ConstNodeRef& rhs) {
          return node_less(NativeNodeRef(lhs), NativeNodeRef(rhs));
        });
        size_t last_append = NONE;
        for (const auto& child : children) {
          // Use tree's duplicate method directly since ConstNodeRef doesn't have duplicate
          last_append = result.tree()->duplicate(child.tree(), child.id(), result.id(), last_append);
        }
      }
      data_eval_stack.emplace_back(result);
    } break;
    case Op::Upper: {
      auto result = native_to_string(get_arguments<1>(node)[0]);
      std::transform(result.begin(), result.end(), result.begin(), [](char c) { return static_cast<char>(::toupper(c)); });
      make_result(result);
    } break;
    case Op::IsBoolean: {
      make_result(get_arguments<1>(node)[0].kind() == NativeKind::Bool);
    } break;
    case Op::IsNumber: {
      make_result(native_is_number(get_arguments<1>(node)[0].kind()));
    } break;
    case Op::IsInteger: {
      const auto kind = get_arguments<1>(node)[0].kind();
      make_result(kind == NativeKind::Int64 || kind == NativeKind::UInt64);
    } break;
    case Op::IsFloat: {
      auto value = native_to_double(get_arguments<1>(node)[0]);
      make_result(value && *value != std::floor(*value));
    } break;
    case Op::IsObject: {
      make_result(get_arguments<1>(node)[0].node.is_map());
    } break;
    case Op::IsArray: {
      make_result(get_arguments<1>(node)[0].node.is_seq());
    } break;
    case Op::IsString: {
      const auto value = get_arguments<1>(node)[0];
      make_result(value.valid() && (value.node.is_val() || value.node.is_keyval()) && value.kind() == NativeKind::String);
    } break;
    case Op::Callback: {
      auto args = get_argument_vector(node);
      data_eval_stack.emplace_back(node.callback(args, additional_data));
    } break;
    case Op::Macro: {
      make_result(evaluate_macro(node));
    } break;
    case Op::Super: {
      const auto args = get_argument_vector(node);
      const size_t old_level = current_level;
      const size_t level_diff = (args.size() == 1) ? static_cast<size_t>(node_to_int(args[0]).value_or(1)) : 1;
      const size_t level = current_level + level_diff;

      if (block_statement_stack.empty()) {
        throw_renderer_error("super() call is not within a block", node);
      }

      if (level < 1 || level > template_stack.size() - 1) {
        throw_renderer_error("level of super() call does not match parent templates (between 1 and " + std::to_string(template_stack.size() - 1) + ")", node);
      }

      const auto current_block_statement = block_statement_stack.back();
      const Template* new_template = template_stack.at(level);
      const Template* old_template = current_template;
      const auto block_it = new_template->block_storage.find(current_block_statement->name);
      if (block_it != new_template->block_storage.end()) {
        current_template = new_template;
        current_level = level;
        block_it->second->block.accept(*this);
        current_level = old_level;
        current_template = old_template;
      } else {
        throw_renderer_error("could not find block with name '" + current_block_statement->name + "'", node);
      }
      make_null_result();
    } break;
    case Op::Join: {
      const auto args = get_arguments<2>(node);
      const auto separator = native_to_string(args[1]);
      std::ostringstream os;
      std::string sep;
      for (const auto& value : args[0].node.children()) {
        os << sep;
        if (value.is_val() || value.is_keyval()) {
          os << node_to_string(value);
        } else {
          os << ryml::as_json(*value.tree(), value.id());
        }
        sep = separator;
      }
      make_result(os.str());
    } break;
    case Op::Hex: {
      make_result(std::format("{:x}", native_to_int(get_arguments<1>(node)[0]).value_or(0)));
    } break;
    case Op::None:
      break;
    }
  }

  void visit(const ExpressionListNode& node) override {
    print_data(eval_expression_list(node));
  }

  void visit(const StatementNode&) override {}

  void visit(const ForStatementNode&) override {}

  void visit(const ForArrayStatementNode& node) override {
    restore_loop_frames_from_current_scope_if_needed();

    const auto result = eval_expression_list(node.condition);
    if (!result.valid()) {
      sync_current_loop_data_from_frames();
      return;
    }
    if (!result.node.is_seq()) {
      throw_renderer_error("object must be an array", node);
      sync_current_loop_data_from_frames();
      return;
    }

    const size_t size = result.node.num_children();
    size_t index = 0;

    for (const auto& child : result.node.children()) {
      loop_frames.push_array(child, index, size);
      sync_current_loop_data_from_frames();
      set_child_from_node(additional_data, node.value, child);
      node.body.accept(*this);

      loop_frames.pop();
      ++index;
    }

    if (find_child_by_key(additional_data, to_csubstr(node.value)).valid()) {
      additional_data.remove_child(to_csubstr(node.value));
    }

    sync_current_loop_data_from_frames();
  }

  void visit(const ForObjectStatementNode& node) override {
    restore_loop_frames_from_current_scope_if_needed();

    const auto result = eval_expression_list(node.condition);
    if (!result.valid()) {
      sync_current_loop_data_from_frames();
      return;
    }
    if (!result.node.is_map()) {
      throw_renderer_error("object must be an object", node);
      sync_current_loop_data_from_frames();
      return;
    }

    const size_t size = result.node.num_children();
    size_t index = 0;

    std::vector<ConstNodeRef> children;
    children.reserve(result.node.num_children());
    for (const auto& child : result.node.children()) {
      children.push_back(child);
    }
    std::sort(children.begin(), children.end(), [](const ConstNodeRef& lhs, const ConstNodeRef& rhs) {
      return lhs.key() < rhs.key();
    });

    for (const auto& child : children) {
      loop_frames.push_object(child.key(), child, index, size);
      sync_current_loop_data_from_frames();

      auto key_node = tmp_sequence.append_child();
      key_node << child.key();
      set_child_from_node(additional_data, node.key, key_node);
      set_child_from_node(additional_data, node.value, child);

      node.body.accept(*this);

      loop_frames.pop();
      ++index;
    }

    if (find_child_by_key(additional_data, to_csubstr(node.key)).valid()) {
      additional_data.remove_child(to_csubstr(node.key));
    }
    if (find_child_by_key(additional_data, to_csubstr(node.value)).valid()) {
      additional_data.remove_child(to_csubstr(node.value));
    }

    sync_current_loop_data_from_frames();
  }

  void visit(const IfStatementNode& node) override {
    const auto result = eval_expression_list(node.condition);
    if (native_truthy(result)) {
      node.true_statement.accept(*this);
    } else if (node.has_false_statement) {
      node.false_statement.accept(*this);
    }
  }

  void visit(const IncludeStatementNode& node) override {
    auto sub_renderer = Renderer(config, template_storage, function_storage);
    const auto included_template_it = template_storage.find(node.file);
    if (included_template_it != template_storage.end()) {
      sub_renderer.render_to(*output_stream, included_template_it->second, data_input, additional_data);
    } else if (config.throw_at_missing_includes) {
      throw_renderer_error("include '" + node.file + "' not found", node);
    }
  }

  void visit(const ExtendsStatementNode& node) override {
    const auto included_template_it = template_storage.find(node.file);
    if (included_template_it != template_storage.end()) {
      const Template* parent_template = &included_template_it->second;
      render_to(*output_stream, *parent_template, data_input, additional_data);
      break_rendering = true;
    } else if (config.throw_at_missing_includes) {
      throw_renderer_error("extends '" + node.file + "' not found", node);
    }
  }

  void visit(const BlockStatementNode& node) override {
    const size_t old_level = current_level;
    current_level = 0;
    current_template = template_stack.front();
    const auto block_it = current_template->block_storage.find(node.name);
    if (block_it != current_template->block_storage.end()) {
      block_statement_stack.emplace_back(&node);
      block_it->second->block.accept(*this);
      block_statement_stack.pop_back();
    }
    current_level = old_level;
    current_template = template_stack.back();
  }

  void visit(const SetStatementNode& node) override {
    const auto result = eval_expression_list(node.expression);
    constexpr std::string_view global_scope_prefix = "scope.global.";

    NodeRef target_scope = additional_data;
    std::string target_key = node.key;
    if (inja::string_view::starts_with(target_key, global_scope_prefix)) {
      target_scope = template_scope_data;
      target_key = target_key.substr(global_scope_prefix.size());
    }

    const Pointer ptr(DataNode::convert_dot_to_ptr(target_key));
    if (ptr.empty()) {
      throw_renderer_error("invalid variable name", node);
      return;
    }
    const auto last_key = ptr.back();
    auto target = target_scope[ptr];
    
    if (!result.valid()) {
      target = nullptr;
      return;
    }

    // Check if there is already a node, if so delete and recreate as seed node
    if (!target.is_seed()) {
      auto parent = target.parent();
      parent.remove_child(last_key);
      // target.tree()->remove(target.id());
      target = target_scope[ptr];
      if (parent.contains(last_key)) {
        spdlog::error("failed to overwrite existing variable");
        return;
      }
    }
    auto parent = NodeRef(target.tree(), target.id()); // A seed node has the ID of the parent
    if (parent.contains(last_key)) {
        spdlog::error("failed to overwrite existing variable");
        return;
      }
    auto new_index = parent.tree()->duplicate(result.node.tree(), result.node.id(), parent.id(), NONE);
    auto new_node = NodeRef(parent.tree(), new_index);
    new_node << ryml::key(last_key);
  }

  void visit(const MacroStatementNode&) override {
    // Macro declarations do not produce output during normal rendering.
  }

public:
  explicit Renderer(const RenderConfig& config, const TemplateStorage& template_storage, const FunctionStorage& function_storage)
      : config(config), template_storage(template_storage), function_storage(function_storage) {}

  void render_to(std::ostream& os, const Template& tmpl, const ConstNodeRef& data, NodeRef shared_additional_data) {
    output_stream = &os;
    current_template = &tmpl;
    data_input = data;
    while (!not_found_stack.empty()) {
      not_found_stack.pop();
    }

    template_scope_data = shared_additional_data;
    additional_data = shared_additional_data;
    current_loop_data = additional_data.append_child() << ryml::key("loop");
    current_loop_data |= ryml::MAP;
    tmp_sequence = additional_data.append_child() << ryml::key("_tmp");
    tmp_sequence |= ryml::SEQ;

    template_stack.emplace_back(current_template);
    current_template->root.accept(*this);

    data_eval_stack.clear();
  }
};

} // namespace inja

#endif // INCLUDE_INJA_RENDERER_HPP_

// #include "template.hpp"

// #include "throw.hpp"


namespace inja {

/*!
 * \brief Class for changing the configuration.
 */
class Environment {
  FunctionStorage function_storage;
  TemplateStorage template_storage;

protected:
  LexerConfig lexer_config;
  ParserConfig parser_config;
  RenderConfig render_config;

  std::filesystem::path input_path;
  std::filesystem::path output_path;

  inja::Tree temp_data_tree;

public:
  Environment() {setup_data();}
  explicit Environment(const std::filesystem::path& global_path): input_path(global_path), output_path(global_path) { setup_data(); }
  Environment(const std::filesystem::path& input_path, const std::filesystem::path& output_path): input_path(input_path), output_path(output_path) {setup_data();}

  void setup_data() {
    temp_data_tree.reserve_arena(2 * 1024 * 1024); // Reserve 2MB
    temp_data_tree.add_flags(inja::Tree::TREEF_NO_ARENA_REALLOC);
  }

  /// Sets the opener and closer for template statements
  void set_statement(const std::string& open, const std::string& close) {
    lexer_config.statement_open = open;
    lexer_config.statement_open_no_lstrip = open + "+";
    lexer_config.statement_open_force_lstrip = open + "-";
    lexer_config.statement_close = close;
    lexer_config.statement_close_force_rstrip = "-" + close;
    lexer_config.update_open_chars();
  }

  /// Sets the opener for template line statements
  void set_line_statement(const std::string& open) {
    lexer_config.line_statement = open;
    lexer_config.update_open_chars();
  }

  /// Sets the opener and closer for template expressions
  void set_expression(const std::string& open, const std::string& close) {
    lexer_config.expression_open = open;
    lexer_config.expression_open_force_lstrip = open + "-";
    lexer_config.expression_close = close;
    lexer_config.expression_close_force_rstrip = "-" + close;
    lexer_config.update_open_chars();
  }

  /// Sets the opener and closer for template comments
  void set_comment(const std::string& open, const std::string& close) {
    lexer_config.comment_open = open;
    lexer_config.comment_open_force_lstrip = open + "-";
    lexer_config.comment_close = close;
    lexer_config.comment_close_force_rstrip = "-" + close;
    lexer_config.update_open_chars();
  }

  /// Sets whether to remove the first newline after a block
  void set_trim_blocks(bool trim_blocks) {
    lexer_config.trim_blocks = trim_blocks;
  }

  /// Sets whether to strip the spaces and tabs from the start of a line to a block
  void set_lstrip_blocks(bool lstrip_blocks) {
    lexer_config.lstrip_blocks = lstrip_blocks;
  }

  /// Sets the element notation syntax
  void set_search_included_templates_in_files(bool search_in_files) {
    parser_config.search_included_templates_in_files = search_in_files;
  }

  /// Sets whether a missing include will throw an error
  void set_throw_at_missing_includes(bool will_throw) {
    render_config.throw_at_missing_includes = will_throw;
  }

  /// Sets whether we'll automatically perform HTML escape
  void set_html_autoescape(bool will_escape) {
    render_config.html_autoescape = will_escape;
  }

  Template parse(std::string_view input) {
    Parser parser(parser_config, lexer_config, template_storage, function_storage);
    return parser.parse(input, input_path);
  }

  Template parse_template(const std::filesystem::path& filename) {
    Parser parser(parser_config, lexer_config, template_storage, function_storage);
    auto result = Template(Parser::load_file(input_path / filename));
    parser.parse_into_template(result, (input_path / filename).string());
    return result;
  }

  Template parse_file(const std::filesystem::path& filename) {
    return parse_template(filename);
  }

  std::string render(std::string_view input, const ConstNodeRef& data) {
    return render(parse(input), data);
  }

  std::string render(const Template& tmpl, const ConstNodeRef& data) {
    std::stringstream os;
    render_to(os, tmpl, data);
    return os.str();
  }

  std::string render_file(const std::filesystem::path& filename, const ConstNodeRef& data) {
    return render(parse_template(filename), data);
  }

  std::string render_file_with_json_file(const std::filesystem::path& filename, const std::string& filename_data) {
    Tree data = load_json(filename_data);
    return render_file(filename, data.rootref());
  }

  void write(const std::filesystem::path& filename, const ConstNodeRef& data, const std::string& filename_out) {
    std::ofstream file(output_path / filename_out);
    file << render_file(filename, data);
    file.close();
  }

  void write(const Template& temp, const ConstNodeRef& data, const std::string& filename_out) {
    std::ofstream file(output_path / filename_out);
    file << render(temp, data);
    file.close();
  }

  void write_with_json_file(const std::filesystem::path& filename, const std::string& filename_data, const std::string& filename_out) {
    Tree data = load_json(filename_data);
    write(filename, data.rootref(), filename_out);
  }

  void write_with_json_file(const Template& temp, const std::string& filename_data, const std::string& filename_out) {
    Tree data = load_json(filename_data);
    write(temp, data.rootref(), filename_out);
  }

  std::ostream& render_to(std::ostream& os, const Template& tmpl, const ConstNodeRef& data) {
    NodeRef additional_data = temp_data_tree.rootref().append_child();
    additional_data |= ryml::MAP;
    additional_data["store"] |= ryml::MAP;
    additional_data["values"] |= ryml::SEQ;
    Renderer(render_config, template_storage, function_storage).render_to(os, tmpl, data, additional_data);
    temp_data_tree.remove(additional_data.id());
    return os;
  }

  std::ostream& render_to(std::ostream& os, const std::string_view input, const ConstNodeRef& data) {
    return render_to(os, parse(input), data);
  }

  std::string load_file(const std::string& filename) {
    const Parser parser(parser_config, lexer_config, template_storage, function_storage);
    return Parser::load_file(input_path / filename);
  }

  Tree load_json(const std::string& filename) {
    std::ifstream file;
    file.open(input_path / filename);
    if (file.fail()) {
      const auto trimmed = trim(filename);
      if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
        const ryml::csubstr source_name = to_csubstr("inline-json");
        return ryml::parse_in_arena(source_name, to_csubstr(filename));
      }
      INJA_THROW(FileError("failed accessing file at '" + (input_path / filename).string() + "'"));
    }

    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const ryml::csubstr file_name = to_csubstr(filename);
    return ryml::parse_in_arena(file_name, to_csubstr(contents));
  }

  /*!
  @brief Adds a variadic callback
  */
  void add_callback(const std::string& name, const CallbackFunction& callback) {
    add_callback(name, -1, callback);
  }

  /*!
  @brief Adds a variadic void callback
  */
  void add_void_callback(const std::string& name, const VoidCallbackFunction& callback) {
    add_void_callback(name, -1, callback);
  }

  /*!
  @brief Adds a callback with given number or arguments
  */
  void add_callback(const std::string& name, int num_args, const CallbackFunction& callback) {
    function_storage.add_callback(name, num_args, callback);
  }

  /*!
  @brief Adds a void callback with given number or arguments
  */
  void add_void_callback(const std::string& name, int num_args, const VoidCallbackFunction& callback) {
    function_storage.add_callback(name, num_args, [callback](Arguments& args, NodeRef additional_data) {
      callback(args, additional_data);
      auto root = additional_data;
      root |= ryml::MAP;
      auto tmp = ensure_child_seq(root, "_tmp");
      auto node = tmp.append_child();
      node = nullptr;
      return ConstNodeRef(node);
    });
  }

  /** Includes a template with a given name into the environment.
   * Then, a template can be rendered in another template using the
   * include "<name>" syntax.
   */
  void include_template(const std::string& name, const Template& tmpl) {
    template_storage[name] = tmpl;
  }

  /*!
  @brief Sets a function that is called when an included file is not found
  */
  void set_include_callback(const std::function<Template(const std::filesystem::path&, const std::string&)>& callback) {
    parser_config.include_callback = callback;
  }
};

/*!
@brief render with default settings to a string
*/
inline std::string render(std::string_view input, const ConstNodeRef& data) {
  return Environment().render(input, data);
}

/*!
@brief render with default settings to the given output stream
*/
inline void render_to(std::ostream& os, std::string_view input, const ConstNodeRef& data) {
  Environment env;
  env.render_to(os, env.parse(input), data);
}

} // namespace inja

#endif // INCLUDE_INJA_ENVIRONMENT_HPP_

// #include "exceptions.hpp"

// #include "parser.hpp"

// #include "renderer.hpp"

// #include "template.hpp"


#endif // INCLUDE_INJA_INJA_HPP_
