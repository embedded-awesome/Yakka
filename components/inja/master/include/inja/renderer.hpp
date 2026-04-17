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
#include <unordered_set>
#include <utility>
#include <vector>

#include "config.hpp"
#include "exceptions.hpp"
#include "function_storage.hpp"
#include "node.hpp"
#include "template.hpp"
#include "throw.hpp"
#include "utils.hpp"
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
      // Emplace a seed node
      data_eval_stack.emplace_back(additional_data[node.ptr]);
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
          data_eval_stack.emplace_back(resolve_pointer(additional_data, ptr));
        }
      } else {
        const auto args = get_arguments<2>(node);
        ConstNodeRef container;

        if (args[0].valid() && (args[0].node.is_map() || args[0].node.is_seq())) {
          container = args[0].node;
        } else {
          const auto ptr_text = native_to_string(args[0]);
          if (!ptr_text.empty()) {
            container = resolve_pointer(additional_data, Pointer(ptr_text));
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
    case Op::SetAt: {
      const auto args = get_argument_vector(node);
      const auto num_args = args.size();
      // Check the type of arg 0
      const auto arg0 = args[0];
      NodeRef arg0_ref;
      if (!arg0.valid()) {
        spdlog::error("setAt: invalid first path argument");
        make_null_result();
        break;
      } else if (arg0.is_val()) {
        arg0_ref = additional_data[ryml::Pointer{ arg0.val() }];
      } else if (arg0.tree() == additional_data.tree()) {
        // If the referenced node is already in the additional_data tree, we can use it directly
        arg0_ref = NodeRef{additional_data.tree(), arg0.id()};
      } else {
        // Otherwise, we need to duplicate the node into the additional_data tree
        arg0_ref = additional_data[ryml::Pointer{ arg0.val() }];
      }

      if (num_args == 2) {
        // setAt(path, value)
        if (args[1].is_map()) {
          arg0_ref |= ryml::MAP;
          additional_data.tree()->duplicate(args[1].tree(), args[1].id(), arg0_ref.id(), NONE);
        } else if (args[1].is_seq()) {
          arg0_ref |= ryml::SEQ;
          additional_data.tree()->duplicate_children(args[1].tree(), args[1].id(), arg0_ref.id(), NONE);
        } else {
          arg0_ref << args[1].val();
          arg0_ref.set_key_serialized(arg0.val());
        }
      } else if (num_args == 3) {
        // store(path, key, value)
        ryml::Pointer ptr{ args[1].val() };
        arg0_ref[ptr] << args[2].val();
        if (arg0_ref[ptr].is_map())
          arg0_ref[ptr].set_key_serialized(ptr.back());
      }
      make_null_result();
    } break;
    case Op::Fetch: {
      const auto num_args = get_argument_vector(node).size();
      if (num_args == 1) {
        // fetch(path)
        const auto args = get_arguments<1>(node);
        ryml::Pointer ptr{ native_to_string(args[0]) };
        data_eval_stack.emplace_back(additional_data[ptr]);
      } else if (num_args == 2) {
        // fetch(path, key)
        const auto args = get_arguments<2>(node);
        ryml::Pointer ptr{ native_to_string(args[0]) };
        auto key = args[1].node.val();
        data_eval_stack.emplace_back(additional_data[ptr][key]);
      }
    } break;
    case Op::PushBack: {
      const auto args = get_argument_vector(node);
      const auto num_args = args.size();
      // Check the type of arg 0
      const auto arg0 = args[0];
      NodeRef arg0_ref;
      if (!arg0.valid()) {
        spdlog::error("pushBack: invalid first path argument");
        make_null_result();
        break;
      } else if (arg0.is_val()) {
        arg0_ref = additional_data[ryml::Pointer{ arg0.val() }];
      } else if (arg0.tree() == additional_data.tree()) {
        // If the referenced node is already in the additional_data tree, we can use it directly
        arg0_ref = NodeRef{additional_data.tree(), arg0.id()};
      } else {
        // Otherwise, we need to duplicate the node into the additional_data tree
        arg0_ref = additional_data[ryml::Pointer{ arg0.val() }];
      }

      if (num_args == 2) {
        // push_back(path, value)
        const auto args = get_arguments<2>(node);
        if (args[1].node.is_map()) {
          additional_data.tree()->duplicate(args[1].node.tree(), args[1].node.id(), arg0_ref.id(), NONE);
        } else if (args[1].node.is_seq()) {
          additional_data.tree()->duplicate_children(args[1].node.tree(), args[1].node.id(), arg0_ref.id(), NONE);
        } else {
          if (!arg0_ref.is_seq()) {
            arg0_ref |= ryml::SEQ;
          }
          arg0_ref.append_child() << args[1].node.val();
        }
      } else if (num_args == 3) {
        // push_back(path, key, value)
        const auto args = get_arguments<3>(node);
        ryml::Pointer ptr{ native_to_string(args[1]) };
        if (!arg0_ref.contains(ptr)) {
          arg0_ref[ptr] |= ryml::SEQ;
        }
        arg0_ref[ptr].append_child() << args[2].node.val();
      }
      make_null_result();
    } break;
    case Op::Erase: {
      const auto args = get_arguments<1>(node);
      ryml::Pointer ptr{ native_to_string(args[0]) };
      if (additional_data.contains(ptr))
        additional_data.tree()->remove(additional_data[ptr].id());
      make_null_result();
    } break;
    case Op::Unique: {
      const auto args = get_arguments<1>(node);
      auto filtered = tmp_sequence.append_child();
      filtered |= ryml::SEQ;
      std::unordered_set<std::string> seen;
      for (const auto& i : args[0].node.children()) {
        auto val_str = native_to_string(NativeNodeRef(i));
        if (seen.find(val_str) == seen.end()) {
          seen.insert(val_str);
          size_t last_append = NONE;
          last_append = filtered.tree()->duplicate(i.tree(), i.id(), filtered.id(), last_append);
        }
      }
      data_eval_stack.emplace_back(filtered);
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

  void visit(const ExpressionStatementNode& node) override {
    // Statement expressions are evaluated for side effects only.
    eval_expression_list(node.expression);
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
