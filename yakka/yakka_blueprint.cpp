/**
 * @file yakka_blueprint.cpp
 * @brief Implements blueprint model parsing and value handling.
 */

#include "yakka_blueprint.hpp"
#include "utilities.hpp"
#include <iostream>
#include <pcre2.h>
#include <stdexcept>


namespace yakka {
namespace {

std::string regex_error_message(int error)
{
  PCRE2_UCHAR message[256];
  const int length = pcre2_get_error_message(error, message, sizeof(message));
  return length < 0 ? "PCRE2 error " + std::to_string(error)
                    : std::string(reinterpret_cast<const char *>(message), static_cast<size_t>(length));
}

} // namespace

struct blueprint_regex {
  explicit blueprint_regex(c4::csubstr pattern)
  {
    int error;
    PCRE2_SIZE offset;
    std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> context(
      pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
    if (!context)
      throw std::bad_alloc();
    pcre2_set_newline(context.get(), PCRE2_NEWLINE_ANYCRLF);
    // Full-target matching and the ECMAScript escape/unset-backreference behavior used previously.
    expression.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.str), pattern.len,
                                   PCRE2_ANCHORED | PCRE2_ENDANCHORED | PCRE2_DOLLAR_ENDONLY |
                                     PCRE2_ALT_BSUX | PCRE2_MATCH_UNSET_BACKREF,
                                   &error, &offset, context.get()));
    if (!expression)
      throw std::invalid_argument("Invalid blueprint regex '" + ryml_string(pattern) + "' at offset " +
                                  std::to_string(offset) + ": " + regex_error_message(error));
  }

  std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> expression{ nullptr, pcre2_code_free };
};

blueprint::blueprint(c4::csubstr target, ryml::ConstNodeRef blueprint_data, c4::csubstr parent_path)
{
  this->target      = target;
  this->parent_path = parent_path;
  this->data        = blueprint_data;
  this->process     = ryml::ConstNodeRef();

  auto root = data;

  if (root.has_child("regex"))
    this->regex = root["regex"].val();
  if (this->regex.has_value())
    compiled_regex = std::make_shared<blueprint_regex>(target);

  if (root.has_child("requires") && root["requires"].is_seq()) {
    for (const auto d: root["requires"].children())
      this->requirements.push_back(d.val());
  }

  if (root.has_child("depends") && root["depends"].is_seq()) {
    for (const auto &d: root["depends"].children()) {
      if (!d.is_map() && d.has_val()) {
        const auto dep_value = d.val();
        if (!dep_value.empty() && dep_value.front() == ':')
          this->dependencies.push_back({ dependency::DATA_DEPENDENCY, dep_value });
        else
          this->dependencies.push_back({ dependency::DEFAULT_DEPENDENCY, dep_value });
      } else if (d.is_map()) {
        if (d.has_child("data")) {
          const auto data_node = d["data"];
          if (data_node.is_seq())
            for (const auto &i: data_node.children())
              this->dependencies.push_back({ dependency::DATA_DEPENDENCY, i.val() });
          else
            this->dependencies.push_back({ dependency::DATA_DEPENDENCY, data_node.val() });
        } else if (d.has_child("dependency_file")) {
          this->dependencies.push_back({ dependency::DEPENDENCY_FILE_DEPENDENCY, d["dependency_file"].val() });
        } else if (d.has_child("project")) {
          const auto arguments = d["project"];
          if (!arguments.is_seq() || arguments.num_children() == 0)
            throw std::invalid_argument("Project dependency must be a nonempty array of build arguments");
          dependency project_dependency{ dependency::PROJECT_DEPENDENCY, {} };
          for (const auto argument: arguments.children()) {
            if (!argument.is_val() || argument.val_is_null())
              throw std::invalid_argument("Project dependency arguments must be strings");
            project_dependency.arguments.push_back(argument.val());
          }
          this->dependencies.push_back(std::move(project_dependency));
        }
      }
    }
  }

  if (root.has_child("process"))
    process = root["process"];

  if (root.has_child("group"))
    this->task_group = root["group"].val();

}

bool blueprint::match_regex(const std::string &value, std::vector<std::string> &captures) const
{
  if (!compiled_regex)
    return false;

  std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> matches(
    pcre2_match_data_create_from_pattern(compiled_regex->expression.get(), nullptr), pcre2_match_data_free);
  if (!matches)
    throw std::bad_alloc();
  const int result = pcre2_match(compiled_regex->expression.get(), reinterpret_cast<PCRE2_SPTR>(value.data()),
                                 value.size(), 0, 0, matches.get(), nullptr);
  if (result == PCRE2_ERROR_NOMATCH)
    return false;
  if (result <= 0)
    throw std::runtime_error("Matching blueprint regex '" + ryml_string(target) + "': " + regex_error_message(result));

  captures.clear();
  const auto count = pcre2_get_ovector_count(matches.get());
  const auto offsets = pcre2_get_ovector_pointer(matches.get());
  captures.reserve(count);
  for (uint32_t i = 0; i < count; ++i)
    captures.push_back(offsets[2 * i] == PCRE2_UNSET ? std::string{} :
                       value.substr(offsets[2 * i], offsets[2 * i + 1] - offsets[2 * i]));
  return true;
}

} // namespace yakka
