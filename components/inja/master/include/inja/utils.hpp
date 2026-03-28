#ifndef INCLUDE_INJA_UTILS_HPP_
#define INCLUDE_INJA_UTILS_HPP_

#include <algorithm>
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

#include "exceptions.hpp"
#include "json.hpp"
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
  double result;
  auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), result);
  if (ec != std::errc() || ptr != view.data() + view.size()) {
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
