#include "yakka_schema.hpp"
#include "yakka_component.hpp"
#include "utilities.hpp"
#include "slcc_schema.hpp"

namespace yakka {

void schema::add_schema_data(ryml::ConstNodeRef schema_data)
{
  // const auto schema_ryml::Pointer = ryml::Pointer("/properties");
  // json_node_merge(schema_ryml::Pointer, this->schema_data["properties"], schema_data);
  validator_updated = false;
}

bool schema::validate(ryml::ConstNodeRef data, ryml::csubstr id)
{
  return true;
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

yakka_schema_validator::yakka_schema_validator() /*: yakka_validator(nullptr, ryml_schema::default_string_format_check), slcc_validator(nullptr, ryml_schema::default_string_format_check)*/
{
  // This should be straight JSON without conversion
  // yakka_schema = YAML::Load(yakka_component_schema_yaml).as<ryml::Tree>();
  // slcc_schema  = YAML::Load(slcc_schema_yaml).as<ryml::Tree>();
  // yakka_validator.set_root_schema(yakka_schema);
  // slcc_validator.set_root_schema(slcc_schema);
}

bool yakka_schema_validator::validate(yakka::component *component)
{
  // custom_error_handler err;
  // err.component = component;
  // if (component->type == component::YAKKA_FILE)
  //   auto patch = yakka_validator.validate(component->json, err);
  // else if (component->type == component::SLCC_FILE)
  //   auto patch = slcc_validator.validate(component->json, err);
  // if (err) {
  //   return false;
  // } else {
  //   return true;
  // }
  return true;
}

} // namespace yakka