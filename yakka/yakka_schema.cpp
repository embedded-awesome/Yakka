/**
 * @file yakka_schema.cpp
 * @brief Implements schema loading, merging, and validation utilities.
 */

#include "yakka_schema.hpp"
#include "yakka_component.hpp"
#include "utilities.hpp"
#include "slcc_schema.hpp"


#include <valijson/adapters/rapidyaml_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>

namespace yakka {

schema::schema() = default;

schema::~schema() = default;

schema::schema(schema &&) noexcept = default;

schema &schema::operator=(schema &&) noexcept = default;

/// @brief Executes schema.

schema::schema(const std::string &schema_yaml) : schema_data()
{
  if (schema_yaml.empty()) {

    return;
  }

  try {
    schema_data = ryml::parse_in_arena(c4::to_csubstr(schema_yaml));
  } catch (const std::exception &e) {
    spdlog::error("Failed to parse schema YAML string: {}", e.what());
  }
}

/// @brief Executes schema.

schema::schema(const std::filesystem::path &schema_path) : schema_data()
{
  const auto loaded_schema = ryml_load_file(schema_path);

  if (!loaded_schema.has_value()) {
    spdlog::error("Failed to load schema file '{}': {}", schema_path.generic_string(), loaded_schema.error().message());
    return;
  }

  schema_data = std::move(loaded_schema.value());
}

/// @brief Executes add_schema_data.

void schema::add_schema_data(ryml::ConstNodeRef schema_data)
{
  if (!schema_data.valid()) {

    return;
  }

  auto root = this->schema_data.rootref();
  if (!root.has_children()) {
    root |= ryml::MAP;
    root.append_child() << ryml::key("type") << "object";
    auto properties = root.append_child() << ryml::key("properties");
    properties |= ryml::MAP;
  }
  merge_nodes(root["properties"], schema_data);
  parsed_schema.reset();
  validator_updated = false;
}

/// @brief Executes validate.

bool schema::validate(ryml::ConstNodeRef data, ryml::csubstr id)
{
  if (!data.valid()) {

    spdlog::error("Schema validation failed for '{}': input data node is invalid", ryml_string(id));
    return false;
  }

  if (schema_data.empty()) {
    return true;
  }

  const auto schema_root = schema_data.crootref();
  if (!schema_root.valid() || !schema_root.is_map()) {
    return true;
  }

  try {
    if (!validator_updated || !parsed_schema) {
      auto new_schema = std::make_unique<valijson::Schema>();
      valijson::SchemaParser parser;
      parser.populateSchema(valijson::adapters::RymlAdapter(schema_root), *new_schema);
      parsed_schema    = std::move(new_schema);
      validator_updated = true;
    }

    valijson::Validator validator;
    valijson::ValidationResults results;
    const bool is_valid = validator.validate(*parsed_schema, valijson::adapters::RymlAdapter(data), &results);
    if (is_valid) {
      return true;
    }

    valijson::ValidationResults::Error error;
    while (results.popError(error)) {
      if (error.jsonPointer.empty()) {
        spdlog::error("Validation error in '{}': {}", ryml_string(id), error.description);
      } else {
        spdlog::error("Validation error in '{}': {} ({})", ryml_string(id), error.description, error.jsonPointer);
      }
    }

    return false;
  } catch (const std::exception &e) {
    parsed_schema.reset();
    validator_updated = false;
    spdlog::error("Schema validation failed for '{}': {}", ryml_string(id), e.what());
    return false;
  }
}

ryml::ConstNodeRef schema::operator[](const ryml::Pointer &path) const
{
  return (*this)[path.to_string()];
}

ryml::ConstNodeRef schema::operator[](const std::string &path) const
{
  if (schema_data.empty()) {
    return ryml::ConstNodeRef{};
  }

  auto current = schema_data.crootref();
  if (!current.valid()) {
    return ryml::ConstNodeRef{};
  }

  if (path.empty() || path == "/") {
    return current;
  }

  size_t position = 0;
  while (position < path.size()) {
    while (position < path.size() && path[position] == '/') {
      ++position;
    }

    if (position >= path.size()) {
      break;
    }

    const size_t next = path.find('/', position);
    auto segment      = path.substr(position, next == std::string::npos ? std::string::npos : next - position);

    if (!current.is_map()) {
      return ryml::ConstNodeRef{};
    }

    const auto key = c4::to_csubstr(segment);
    if (current.has_child(key)) {
      current = current.find_child(key);
    } else {
      auto properties = current.find_child("properties");
      if (!properties.valid() || !properties.is_map() || !properties.has_child(key)) {
        return ryml::ConstNodeRef{};
      }
      current = properties.find_child(key);
    }

    if (next == std::string::npos) {
      break;
    }

    position = next + 1;
  }

  return current;
}

/// @brief Executes get_merge_strategy.

schema::merge_strategy schema::get_merge_strategy(const ryml::Pointer &path) const
{
  const auto node = (*this)[path];
  if (!node.valid() || !node.is_map()) {
    return schema::merge_strategy::Default;
  }

  const auto merge = node.find_child("merge");
  if (!merge.valid() || !merge.has_val()) {
    return schema::merge_strategy::Default;
  }

  const auto strategy = merge.val();
  if (strategy == "concatenate" || strategy == "append")
    return schema::merge_strategy::Append;
  if (strategy == "max")
    return schema::merge_strategy::Max;
  if (strategy == "min")
    return schema::merge_strategy::Min;
  if (strategy == "sort")
    return schema::merge_strategy::Sort;
  if (strategy == "unique")
    return schema::merge_strategy::Unique;
  if (strategy == "overwrite")
    return schema::merge_strategy::Overwrite;
  if (strategy == "abort")
    return schema::merge_strategy::Abort;

  return schema::merge_strategy::Default;
}

yakka_schema_validator::yakka_schema_validator() : 
  yakka_component_schema(yakka_component_schema_yaml), 
  slcc_component_schema(slcc_schema_yaml)

{
}

/// @brief Executes validate.

bool yakka_schema_validator::validate(yakka::component *component)
{
  if (component == nullptr) {

    spdlog::error("Cannot validate null component");
    return false;
  }

  if (!component->root.valid()) {
    spdlog::error("Cannot validate component '{}': invalid root node", component->file_path.generic_string());
    return false;
  }

  switch (component->type) {
  case component::YAKKA_FILE:
    return yakka_component_schema.validate(component->root, component->id);

  case component::SLCC_FILE: {
    const std::string component_name = component->file_path.filename().generic_string();
    return slcc_component_schema.validate(component->root, c4::to_csubstr(component_name));
  }

  default:
    return true;
  }
}

} // namespace yakka
