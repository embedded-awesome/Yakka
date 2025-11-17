#include "yakka_schema.hpp"
#include "yakka_component.hpp"
#include "utilities.hpp"
#include "slcc_schema.hpp"

namespace yakka {

class custom_error_handler : public nlohmann::json_schema::basic_error_handler {
public:
  std::string component_name;
  void error(const nlohmann::json::json_pointer &ptr, const nlohmann::json &instance, const std::string &message) override
  {
    nlohmann::json_schema::basic_error_handler::error(ptr, instance, message);
    spdlog::error("Validation error in '{}': {} - {} : - {}", component_name, ptr.to_string(), instance.dump(3), message);
  }
};

void schema::add_schema_data(const nlohmann::json &schema_data)
{
  const auto schema_json_pointer = "/properties"_json_pointer;
  json_node_merge(schema_json_pointer, this->schema_data["properties"], schema_data);
  validator_updated = false;
}

nlohmann::json schema::validate(const nlohmann::json &data, std::string id)
{
  custom_error_handler err;
  err.component_name = id;

  // Update validator if needed
  if (validator_updated == false) {
    try {
      validator.set_root_schema(this->schema_data);
      validator_updated = true;
    } catch (const std::exception &e) {
      spdlog::error("Setting root schema failed\n{}", e.what());
      return {};
    }
  }

  // Validate schema
  return validator.validate(data, err);
}

schema::merge_strategy schema::get_merge_strategy(const nlohmann::json::json_pointer &path) const
{
  nlohmann::json temp_schema = this->schema_data;
  std::string path_str = path.to_string();
  for (const auto &part_range: std::views::split(path_str, '/')) {
    std::string part(part_range.begin(), part_range.end());
    if (part.empty())
      continue;
    if (temp_schema["properties"].contains(part)) {
      temp_schema = temp_schema["properties"][part];
    } else {
      return schema::merge_strategy::Default;
    }
  }
  if (temp_schema["merge"].is_string()) {
    std::string merge_str = temp_schema["merge"].get<std::string>();
    if (merge_str == "concatenate")
      return schema::merge_strategy::Default;
    else if (merge_str == "max")
      return schema::merge_strategy::Max;
    else if (merge_str == "min")
      return schema::merge_strategy::Min;
    else if (merge_str == "sort")
      return schema::merge_strategy::Sort;
    else if (merge_str == "unique")
      return schema::merge_strategy::Unique;
  }
  return schema::merge_strategy::Default;
}

yakka_schema_validator::yakka_schema_validator() : yakka_validator(nullptr, nlohmann::json_schema::default_string_format_check), slcc_validator(nullptr, nlohmann::json_schema::default_string_format_check)
{
  // This should be straight JSON without conversion
  yakka_schema = YAML::Load(yakka_component_schema_yaml).as<nlohmann::json>();
  slcc_schema  = YAML::Load(slcc_schema_yaml).as<nlohmann::json>();
  yakka_validator.set_root_schema(yakka_schema);
  slcc_validator.set_root_schema(slcc_schema);
}

bool yakka_schema_validator::validate(yakka::component *component)
{
  custom_error_handler err;
  err.component = component;
  if (component->type == component::YAKKA_FILE)
    auto patch = yakka_validator.validate(component->json, err);
  else if (component->type == component::SLCC_FILE)
    auto patch = slcc_validator.validate(component->json, err);
  if (err) {
    return false;
  } else {
    return true;
  }
}

} // namespace yakka