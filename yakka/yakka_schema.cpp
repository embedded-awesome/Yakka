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

schema::schema(const std::filesystem::path &schema_path) : schema_data()
{
  const auto loaded_schema = ryml_load_file(schema_path);
  if (!loaded_schema.has_value()) {
    spdlog::error("Failed to load schema file '{}': {}", schema_path.generic_string(), loaded_schema.error().message());
    return;
  }

  schema_data = std::move(loaded_schema.value());
}

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
  validator_updated = false;
}

bool schema::validate(ryml::ConstNodeRef data, ryml::csubstr id)
{
  if (!data.valid()) {
    spdlog::error("Schema validation failed for '{}': input data node is invalid", ryml_string(id));
    return false;
  }

  const auto schema_root = schema_data.crootref();
  if (!schema_root.valid() || !schema_root.is_map()) {
    return true;
  }

  try {
    valijson::Schema parsed_schema;
    valijson::SchemaParser parser;
    parser.populateSchema(valijson::adapters::RymlAdapter(schema_root), parsed_schema);
    validator_updated = true;

    valijson::Validator validator(valijson::Validator::kWeakTypes);
    valijson::ValidationResults results;
    const bool is_valid = validator.validate(parsed_schema, valijson::adapters::RymlAdapter(data), &results);
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
    validator_updated = false;
    spdlog::error("Schema validation failed for '{}': {}", ryml_string(id), e.what());
    return false;
  }
}

// bool schema::validate(ryml::ConstNodeRef data, std::string id)
// {
  // custom_error_handler err;
  // err.component_name = id;

  // // Update validator if needed
  // if (validator_updated == false) {
  //   try {
  //     validator.set_root_schema(this->schema_data);
  //     validator_updated = true;
  //   } catch (const std::exception &e) {
  //     spdlog::error("Setting root schema failed\n{}", e.what());
  //     return false;
  //   }
  // }

  // // Validate schema
  // validator.validate(data, err);
  // return !err.error_triggered;
//   return true;
// }

ryml::ConstNodeRef schema::operator[](const ryml::Pointer &path) const
{
  // TODO
  return ryml::ConstNodeRef{};
  // return (*this)[path.to_string()];
}

ryml::ConstNodeRef schema::operator[](const std::string &path) const
{
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

schema::merge_strategy schema::get_merge_strategy(const ryml::Pointer &path) const
{
  // ryml::Tree temp_schema = this->schema_data;
  // std::string path_str = path.to_string();
  // for (const auto &part_range: std::views::split(path_str, '/')) {
  //   std::string part(part_range.begin(), part_range.end());
  //   if (part.empty())
  //     continue;
  //   if (temp_schema["properties"].contains(part)) {
  //     temp_schema = temp_schema["properties"][part];
  //   } else {
  //     return schema::merge_strategy::Default;
  //   }
  // }
  // if (temp_schema["merge"].is_string()) {
  //   std::string merge_str = temp_schema["merge"].get<std::string>();
  //   if (merge_str == "concatenate")
  //     return schema::merge_strategy::Default;
  //   else if (merge_str == "max")
  //     return schema::merge_strategy::Max;
  //   else if (merge_str == "min")
  //     return schema::merge_strategy::Min;
  //   else if (merge_str == "sort")
  //     return schema::merge_strategy::Sort;
  //   else if (merge_str == "unique")
  //     return schema::merge_strategy::Unique;
  // }
  return schema::merge_strategy::Default;
}

yakka_schema_validator::yakka_schema_validator() : 
  yakka_component_schema(yakka_component_schema_yaml), 
  slcc_component_schema(slcc_schema_yaml)
{
}

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