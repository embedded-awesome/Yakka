#ifndef INCLUDE_INJA_RYML_JSON_HPP_
#define INCLUDE_INJA_RYML_JSON_HPP_

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ryml.hpp>
#include <c4/std/string.hpp>
#include <c4/std/string_view.hpp>

namespace inja {

struct ryml_item_proxy;

class ryml_json {
public:
  using item_proxy = ryml_item_proxy;
  using string_t = std::string;
  using number_integer_t = int64_t;
  using number_unsigned_t = uint64_t;
  using number_float_t = double;
  using json_pointer = c4::yml::Pointer;

  ryml_json(): tree_(std::make_shared<ryml::Tree>()), id_(create_root_value(*tree_)) {}
  ryml_json(std::nullptr_t): ryml_json() {}
  ryml_json(bool v): ryml_json() { set_scalar(v); }
  ryml_json(int v): ryml_json() { set_scalar(static_cast<number_integer_t>(v)); }
  ryml_json(unsigned int v): ryml_json() { set_scalar(static_cast<number_unsigned_t>(v)); }
  ryml_json(size_t v): ryml_json() { set_scalar(static_cast<number_unsigned_t>(v)); }
  ryml_json(number_integer_t v): ryml_json() { set_scalar(v); }
  ryml_json(number_unsigned_t v): ryml_json() { set_scalar(v); }
  ryml_json(number_float_t v): ryml_json() { set_scalar(v); }
  ryml_json(const string_t& v): ryml_json() { set_scalar(v); }
  ryml_json(string_t&& v): ryml_json() { set_scalar(std::move(v)); }
  ryml_json(const char* v): ryml_json() { set_scalar(string_t(v)); }
  ryml_json(const std::vector<ryml_json>& v): ryml_json() { set_array(v); }
  template <typename T, typename std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
  ryml_json(const std::vector<T>& v): ryml_json() { set_array_from_numbers(v); }
  ryml_json(std::shared_ptr<ryml::Tree> tree, size_t id): tree_(std::move(tree)), id_(id) {}

  static ryml_json parse(std::string_view text) {
    ryml::Tree tree = ryml::parse_in_arena(c4::to_csubstr(text));
    auto shared = std::make_shared<ryml::Tree>(std::move(tree));
    return ryml_json(shared, resolve_parsed_root(*shared));
  }

  template <typename InputIt>
  static ryml_json parse(InputIt first, InputIt last) {
    string_t buffer;
    for (auto it = first; it != last; ++it) {
      buffer.push_back(*it);
    }
    return parse(buffer);
  }

  bool is_null() const {
    auto node = node_cref();
    if (node.is_map() || node.is_seq()) {
      return false;
    }
    if (node.val_is_null()) {
      return true;
    }
    const auto val = node.val();
    return val.len == 0 || val == c4::to_csubstr("null");
  }

  bool is_boolean() const {
    if (node_cref().is_map() || node_cref().is_seq()) {
      return false;
    }
    const auto val = node_cref().val();
    return val == c4::to_csubstr("true") || val == c4::to_csubstr("false");
  }

  bool is_number() const {
    return is_number_integer() || is_number_float();
  }

  bool is_number_integer() const {
    if (node_cref().is_map() || node_cref().is_seq()) {
      return false;
    }
    const auto val = node_cref().val();
    if (val.len == 0 || has_float_token(val)) {
      return false;
    }
    number_integer_t out {};
    return c4::yml::read(node_cref(), &out);
  }

  bool is_number_unsigned() const {
    if (node_cref().is_map() || node_cref().is_seq()) {
      return false;
    }
    const auto val = node_cref().val();
    if (val.len == 0 || has_float_token(val) || (val.len > 0 && val.str[0] == '-')) {
      return false;
    }
    number_unsigned_t out {};
    return c4::yml::read(node_cref(), &out);
  }

  bool is_number_float() const {
    if (node_cref().is_map() || node_cref().is_seq()) {
      return false;
    }
    const auto val = node_cref().val();
    if (val.len == 0) {
      return false;
    }
    if (!has_float_token(val)) {
      return false;
    }
    number_float_t out {};
    return c4::yml::read(node_cref(), &out);
  }

  bool is_string() const {
    if (node_cref().is_map() || node_cref().is_seq()) {
      return false;
    }
    return !is_null() && !is_boolean() && !is_number();
  }

  bool is_object() const {
    return node_cref().is_map();
  }

  bool is_array() const {
    return node_cref().is_seq();
  }

  bool empty() const {
    if (is_object() || is_array()) {
      return node_cref().num_children() == 0;
    }
    if (is_string()) {
      return node_cref().val().len == 0;
    }
    return is_null();
  }

  size_t size() const {
    if (is_object() || is_array()) {
      return node_cref().num_children();
    }
    if (is_string()) {
      return node_cref().val().len;
    }
    return 0;
  }

  string_t dump() const {
    string_t out;
    const size_t target_id = id_;
    if (tree_ && tree_->has_key(target_id)) {
      ryml::Tree temp;
      temp.reserve(4);
      temp.change_type(temp.root_id(), ryml::MAP);
      const size_t dup_id = temp.duplicate(tree_.get(), target_id, temp.root_id(), ryml::NONE);
      ryml::NodeRef dup_node(&temp, dup_id);
      dup_node.clear_key();
      ryml::emitrs_json(temp, dup_id, &out);
    } else {
      ryml::emitrs_json(*tree_, target_id, &out);
    }
    return out;
  }

  template <typename T>
  T get() const {
    using Base = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<Base, string_t>) {
      const auto val = node_cref().val();
      return string_t(val.str, val.len);
    } else if constexpr (std::is_same_v<Base, int>) {
      number_integer_t out {};
      c4::yml::read(node_cref(), &out);
      return static_cast<int>(out);
    } else if constexpr (std::is_same_v<Base, number_integer_t>) {
      number_integer_t out {};
      c4::yml::read(node_cref(), &out);
      return out;
    } else if constexpr (std::is_same_v<Base, number_unsigned_t>) {
      number_unsigned_t out {};
      c4::yml::read(node_cref(), &out);
      return out;
    } else if constexpr (std::is_same_v<Base, number_float_t>) {
      number_float_t out {};
      c4::yml::read(node_cref(), &out);
      return out;
    } else if constexpr (std::is_same_v<Base, bool>) {
      const auto val = node_cref().val();
      return val == c4::to_csubstr("true");
    } else if constexpr (std::is_same_v<Base, std::vector<ryml_json>>) {
      std::vector<ryml_json> result;
      if (!is_array()) {
        return result;
      }
      const size_t count = node_cref().num_children();
      result.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        size_t child_id = tree_->child(id_, i);
        result.emplace_back(tree_, child_id);
      }
      return result;
    } else {
      static_assert(sizeof(Base) == 0, "Unsupported type for get()");
    }
  }

  template <typename T>
  const T& get_ref() const {
    using Base = std::remove_cv_t<std::remove_reference_t<T>>;
    static_assert(std::is_same_v<Base, string_t>, "Only string_t is supported for get_ref()");
    string_cache_.assign(node_cref().val().str, node_cref().val().len);
    return static_cast<const T&>(string_cache_);
  }

  bool contains(const json_pointer& ptr) const {
    return find_pointer(ptr, /*create_missing=*/false).has_value();
  }

  ryml_json& operator=(const ryml_json& other) {
    if (this == &other) {
      return *this;
    }
    assign_from(other);
    return *this;
  }

  ryml_json& operator=(bool v) { set_scalar(v); return *this; }
  ryml_json& operator=(int v) { set_scalar(static_cast<number_integer_t>(v)); return *this; }
  ryml_json& operator=(unsigned int v) { set_scalar(static_cast<number_unsigned_t>(v)); return *this; }
  ryml_json& operator=(size_t v) { set_scalar(static_cast<number_unsigned_t>(v)); return *this; }
  ryml_json& operator=(number_integer_t v) { set_scalar(v); return *this; }
  ryml_json& operator=(number_unsigned_t v) { set_scalar(v); return *this; }
  ryml_json& operator=(number_float_t v) { set_scalar(v); return *this; }
  ryml_json& operator=(const string_t& v) { set_scalar(v); return *this; }
  ryml_json& operator=(const char* v) { set_scalar(string_t(v)); return *this; }

  friend bool operator==(const ryml_json& lhs, int rhs) {
    if (lhs.is_number_integer()) {
      return lhs.get<number_integer_t>() == static_cast<number_integer_t>(rhs);
    }
    if (lhs.is_number_float()) {
      return lhs.get<number_float_t>() == static_cast<number_float_t>(rhs);
    }
    return false;
  }

  friend bool operator!=(const ryml_json& lhs, int rhs) {
    return !(lhs == rhs);
  }

  friend bool operator==(const ryml_json& lhs, const ryml_json& rhs) {
    if (lhs.is_number() && rhs.is_number()) {
      return lhs.get<number_float_t>() == rhs.get<number_float_t>();
    }
    if (lhs.is_string() && rhs.is_string()) {
      return lhs.get<string_t>() == rhs.get<string_t>();
    }
    if (lhs.is_boolean() && rhs.is_boolean()) {
      return lhs.get<bool>() == rhs.get<bool>();
    }
    return lhs.dump() == rhs.dump();
  }

  friend bool operator!=(const ryml_json& lhs, const ryml_json& rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(const ryml_json& lhs, const ryml_json& rhs) {
    if (lhs.is_number() && rhs.is_number()) {
      return lhs.get<number_float_t>() < rhs.get<number_float_t>();
    }
    if (lhs.is_string() && rhs.is_string()) {
      return lhs.get<string_t>() < rhs.get<string_t>();
    }
    return lhs.dump() < rhs.dump();
  }

  friend bool operator>(const ryml_json& lhs, const ryml_json& rhs) {
    return rhs < lhs;
  }

  friend bool operator<=(const ryml_json& lhs, const ryml_json& rhs) {
    return !(rhs < lhs);
  }

  friend bool operator>=(const ryml_json& lhs, const ryml_json& rhs) {
    return !(lhs < rhs);
  }

  ryml_json& operator[](const string_t& key) {
    ensure_map();
    auto child_id = tree_->find_child(id_, c4::to_csubstr(key));
    if (child_id == ryml::NONE) {
      child_id = tree_->append_child(id_);
      ryml::NodeRef child(tree_.get(), child_id);
      child.set_type(ryml::VAL);
      child.set_key_serialized(key);
      child << "null";
    }
    auto& cached = object_cache_[key];
    cached.set_ref(tree_, child_id);
    return cached;
  }

  ryml_json& operator[](size_t index) {
    ensure_seq();
    size_t child_id = ensure_seq_index(index);
    if (index >= array_cache_.size()) {
      array_cache_.resize(index + 1, ryml_json(tree_, child_id));
    }
    array_cache_[index].set_ref(tree_, child_id);
    return array_cache_[index];
  }

  ryml_json& operator[](const json_pointer& ptr) {
    auto id = find_pointer(ptr, /*create_missing=*/true).value();
    const auto key = pointer_key(ptr);
    auto& cached = pointer_cache_[key];
    cached.set_ref(tree_, id);
    return cached;
  }

  const ryml_json& operator[](const string_t& key) const {
    if (!is_object()) {
      return null_singleton();
    }
    auto child_id = tree_->find_child(id_, c4::to_csubstr(key));
    if (child_id == ryml::NONE) {
      return null_singleton();
    }
    auto& cached = object_cache_[key];
    cached.set_ref(tree_, child_id);
    return cached;
  }

  const ryml_json& operator[](size_t index) const {
    if (!is_array() || index >= node_cref().num_children()) {
      return null_singleton();
    }
    size_t child_id = tree_->child(id_, index);
    if (index >= array_cache_.size()) {
      array_cache_.resize(index + 1, ryml_json(tree_, child_id));
    }
    array_cache_[index].set_ref(tree_, child_id);
    return array_cache_[index];
  }

  const ryml_json& operator[](const json_pointer& ptr) const {
    auto found = find_pointer(ptr, /*create_missing=*/false);
    if (!found.has_value()) {
      return null_singleton();
    }
    const auto key = pointer_key(ptr);
    auto& cached = pointer_cache_[key];
    cached.set_ref(tree_, found.value());
    return cached;
  }

  const ryml_json& at(const string_t& key) const { return (*this)[key]; }
  ryml_json& at(const string_t& key) { return (*this)[key]; }

  const ryml_json& at(size_t index) const { return (*this)[index]; }
  ryml_json& at(size_t index) { return (*this)[index]; }

  const ryml_json& at(const json_pointer& ptr) const { return (*this)[ptr]; }
  ryml_json& at(const json_pointer& ptr) { return (*this)[ptr]; }

  const ryml_json& front() const { return at(static_cast<size_t>(0)); }

  const ryml_json& back() const {
    const size_t count = size();
    return count == 0 ? null_singleton() : at(count - 1);
  }

  bool contains(const string_t& key) const {
    if (!is_object()) {
      return false;
    }
    return tree_->find_child(id_, c4::to_csubstr(key)) != ryml::NONE;
  }

  void clear() {
    if (is_object() || is_array()) {
      tree_->change_type(id_, ryml::VAL);
      node_ref().set_val_serialized("null");
      return;
    }
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized("null");
  }

  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = ryml_json;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = ryml_json;

    iterator(): tree_(nullptr), parent_id_(ryml::NONE), child_id_(ryml::NONE), index_(0) {}

    ryml_json operator*() const {
      if (!tree_ || child_id_ == ryml::NONE) {
        return ryml_json();
      }
      return ryml_json(tree_, child_id_);
    }

    iterator& operator++() {
      if (!tree_ || child_id_ == ryml::NONE) {
        return *this;
      }
      ++index_;
      child_id_ = tree_->child(parent_id_, index_);
      return *this;
    }

    string_t key() const {
      if (!tree_ || child_id_ == ryml::NONE) {
        return {};
      }
      if (tree_->is_map(parent_id_)) {
        ryml::ConstNodeRef child(tree_.get(), child_id_);
        return string_t(child.key().str, child.key().len);
      }
      return std::to_string(index_);
    }

    ryml_json value() const {
      if (!tree_ || child_id_ == ryml::NONE) {
        return ryml_json();
      }
      return ryml_json(tree_, child_id_);
    }

    bool operator==(const iterator& other) const {
      return tree_ == other.tree_ && parent_id_ == other.parent_id_ && child_id_ == other.child_id_ && index_ == other.index_;
    }

    bool operator!=(const iterator& other) const {
      return !(*this == other);
    }

  private:
    friend class ryml_json;
    iterator(std::shared_ptr<ryml::Tree> tree, size_t parent_id, size_t index)
        : tree_(std::move(tree)), parent_id_(parent_id), child_id_(ryml::NONE), index_(index) {
      if (tree_ && parent_id_ != ryml::NONE) {
        const size_t count = tree_->num_children(parent_id_);
        if (index_ < count) {
          child_id_ = tree_->child(parent_id_, index_);
        }
      }
    }

    std::shared_ptr<ryml::Tree> tree_;
    size_t parent_id_;
    size_t child_id_;
    size_t index_;
  };

  iterator begin() const {
    if (!is_array() && !is_object()) {
      return end();
    }
    return iterator(tree_, id_, 0);
  }

  iterator end() const {
    if (!is_array() && !is_object()) {
      return iterator();
    }
    return iterator(tree_, id_, node_cref().num_children());
  }

  iterator find(const string_t& key) const {
    if (!is_object()) {
      return end();
    }
    size_t child_id = tree_->find_child(id_, c4::to_csubstr(key));
    if (child_id == ryml::NONE) {
      return end();
    }
    size_t pos = tree_->child_pos(id_, child_id);
    return iterator(tree_, id_, pos);
  }

  std::vector<item_proxy> items() const;

  void push_back(const ryml_json& value) {
    ensure_seq();
    size_t child_id = tree_->append_child(id_);
    ryml_json child(tree_, child_id);
    child.assign_from(value);
  }

private:
  std::shared_ptr<ryml::Tree> tree_;
  size_t id_ {ryml::NONE};
  mutable string_t string_cache_;
  mutable std::unordered_map<string_t, ryml_json> object_cache_;
  mutable std::vector<ryml_json> array_cache_;
  mutable std::unordered_map<string_t, ryml_json> pointer_cache_;

  static const ryml_json& null_singleton() {
    static const ryml_json null_value {};
    return null_value;
  }

  ryml::NodeRef node_ref() {
    return ryml::NodeRef(tree_.get(), id_);
  }

  ryml::ConstNodeRef node_cref() const {
    return ryml::ConstNodeRef(tree_.get(), id_);
  }

  static size_t create_root_value(ryml::Tree& tree) {
    tree.reserve(4);
    ryml::NodeRef root = tree.rootref();
    root.set_type(ryml::VAL);
    root << "null";
    return tree.root_id();
  }

  static size_t resolve_parsed_root(ryml::Tree& tree) {
    ryml::NodeRef root = tree.rootref();
    if (root.is_stream() && root.num_children() > 0) {
      size_t doc_id = tree.child(tree.root_id(), 0);
      ryml::NodeRef doc(&tree, doc_id);
      if (doc.is_doc() && doc.num_children() > 0) {
        return tree.child(doc_id, 0);
      }
      return doc_id;
    }
    if (root.is_doc() && root.num_children() > 0) {
      return tree.child(tree.root_id(), 0);
    }
    return tree.root_id();
  }

  void ensure_map() {
    if (!node_ref().is_map()) {
      tree_->change_type(id_, ryml::MAP);
    }
  }

  void ensure_seq() {
    if (!node_ref().is_seq()) {
      tree_->change_type(id_, ryml::SEQ);
    }
  }

  size_t ensure_seq_index(size_t index) {
    const size_t current = node_ref().num_children();
    for (size_t i = current; i <= index; ++i) {
      const size_t child_id = tree_->append_child(id_);
      ryml::NodeRef child(tree_.get(), child_id);
      child.set_type(ryml::VAL);
      child << "null";
    }
    return tree_->child(id_, index);
  }

  void set_scalar(bool v) {
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized(v ? "true" : "false");
  }

  void set_scalar(number_integer_t v) {
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized(v);
  }

  void set_scalar(number_unsigned_t v) {
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized(v);
  }

  void set_scalar(number_float_t v) {
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized(v);
  }

  void set_scalar(const string_t& v) {
    tree_->change_type(id_, ryml::VAL);
    node_ref().set_val_serialized(v);
  }

  void set_scalar(string_t&& v) {
    set_scalar(static_cast<const string_t&>(v));
  }

  void set_array(const std::vector<ryml_json>& v) {
    ensure_seq();
    tree_->remove_children(id_);
    for (const auto& item : v) {
      size_t child_id = tree_->append_child(id_);
      ryml_json child(tree_, child_id);
      child.assign_from(item);
    }
  }

  template <typename T>
  void set_array_from_numbers(const std::vector<T>& v) {
    ensure_seq();
    tree_->remove_children(id_);
    for (const auto& item : v) {
      size_t child_id = tree_->append_child(id_);
      ryml::NodeRef child(tree_.get(), child_id);
      child.set_type(ryml::VAL);
      if constexpr (std::is_integral_v<T>) {
        child << static_cast<number_integer_t>(item);
      } else {
        child << static_cast<number_float_t>(item);
      }
    }
  }

  void assign_from(const ryml_json& other) {
    if (other.is_object()) {
      ensure_map();
      tree_->remove_children(id_);
      const size_t count = other.node_cref().num_children();
      for (size_t i = 0; i < count; ++i) {
        const size_t child_id = other.tree_->child(other.id_, i);
        ryml::ConstNodeRef child(other.tree_.get(), child_id);
        string_t key(child.key().str, child.key().len);
        (*this)[key].assign_from(ryml_json(other.tree_, child_id));
      }
      return;
    }
    if (other.is_array()) {
      ensure_seq();
      tree_->remove_children(id_);
      for (const auto& value : other) {
        push_back(value);
      }
      return;
    }
    if (other.is_boolean()) {
      set_scalar(other.get<bool>());
      return;
    }
    if (other.is_number_integer()) {
      set_scalar(other.get<number_integer_t>());
      return;
    }
    if (other.is_number_unsigned()) {
      set_scalar(other.get<number_unsigned_t>());
      return;
    }
    if (other.is_number_float()) {
      set_scalar(other.get<number_float_t>());
      return;
    }
    if (other.is_string()) {
      set_scalar(other.get<string_t>());
      return;
    }
    set_scalar("null");
  }

  void set_ref(std::shared_ptr<ryml::Tree> tree, size_t id) {
    tree_ = std::move(tree);
    id_ = id;
  }

  static bool has_float_token(c4::csubstr val) {
    for (size_t i = 0; i < val.len; ++i) {
      const char c = val.str[i];
      if (c == '.' || c == 'e' || c == 'E') {
        return true;
      }
    }
    return false;
  }

  static bool is_integer_token(c4::csubstr token) {
    if (token.len == 0) {
      return false;
    }
    size_t start = 0;
    if (token.str[0] == '-') {
      if (token.len == 1) {
        return false;
      }
      start = 1;
    }
    for (size_t i = start; i < token.len; ++i) {
      if (token.str[i] < '0' || token.str[i] > '9') {
        return false;
      }
    }
    return true;
  }

  static size_t token_to_index(c4::csubstr token) {
    string_t tmp(token.str, token.len);
    return static_cast<size_t>(std::stoll(tmp));
  }

  std::optional<size_t> find_pointer(const json_pointer& ptr, bool create_missing) const {
    if (!tree_) {
      return std::nullopt;
    }
    size_t current = id_;
    for (const auto& token : ptr.path()) {
      ryml::NodeRef node(tree_.get(), current);
      if (node.is_seq() || is_integer_token(token)) {
        const size_t index = token_to_index(token);
        if (create_missing) {
          if (!node.is_seq()) {
            tree_->remove_children(current);
            node.set_type(ryml::SEQ);
          }
          current = const_cast<ryml_json*>(this)->ensure_seq_index(index);
        } else {
          if (!node.is_seq() || node.num_children() <= index) {
            return std::nullopt;
          }
          current = tree_->child(current, index);
        }
      } else {
        if (create_missing) {
          if (!node.is_map()) {
            tree_->remove_children(current);
            node.set_type(ryml::MAP);
          }
          size_t child_id = tree_->find_child(current, token);
          if (child_id == ryml::NONE) {
            child_id = tree_->append_child(current);
            ryml::NodeRef child(tree_.get(), child_id);
            child.set_type(ryml::VAL);
            child.set_key_serialized(token);
            child << "null";
          }
          current = child_id;
        } else {
          if (!node.is_map()) {
            return std::nullopt;
          }
          size_t child_id = tree_->find_child(current, token);
          if (child_id == ryml::NONE) {
            return std::nullopt;
          }
          current = child_id;
        }
      }
    }
    return current;
  }

  static string_t pointer_key(const json_pointer& ptr) {
    string_t key;
    for (const auto& token : ptr.path()) {
      key.push_back('/');
      key.append(token.str, token.len);
    }
    return key;
  }
};

#if 0
struct ryml_item_proxy {
  using string_t = c4::csubstr;

  string_t key;
  std::shared_ptr<ryml::Tree> tree;
  size_t id {ryml::NONE};
  ryml_json value_holder;

  ryml_item_proxy(string_t key_value, std::shared_ptr<ryml::Tree> tree_value, size_t node_id)
      : key(std::move(key_value)), tree(std::move(tree_value)), id(node_id), value_holder(tree, id) {}

  const ryml_json& value() const { return value_holder; }
};

inline std::vector<ryml_item_proxy> ryml_json::items() const {
  std::vector<ryml_item_proxy> result;
  if (is_object()) {
    const size_t count = node_cref().num_children();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t child_id = tree_->child(id_, i);
      ryml::ConstNodeRef child(tree_.get(), child_id);
      string_t key(child.key().str, child.key().len);
      result.emplace_back(std::move(key), tree_, child_id);
    }
  } else if (is_array()) {
    const size_t count = node_cref().num_children();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t child_id = tree_->child(id_, i);
      result.emplace_back(std::to_string(i), tree_, child_id);
    }
  }
  return result;
}
#endif

} // namespace inja

#endif // INCLUDE_INJA_RYML_JSON_HPP_
