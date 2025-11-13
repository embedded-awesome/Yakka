#include "yakka_schema.hpp"
#include "yakka_component.hpp"

namespace yakka {

bool schema_validator::validate(yakka::component *component)
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

std::optional<schema_validator::merge_strategy> schema_validator::get_schema(const nlohmann::json::json_pointer &path)
{
  std::string path_str = path.to_string();
  auto yakka_schema    = schema_validator::get().yakka_schema;
  for (const auto &part_range: std::views::split(path_str, '/')) {
    std::string part(part_range.begin(), part_range.end());
    if (part.empty())
      continue;
    if (yakka_schema["properties"].contains(part)) {
      yakka_schema = yakka_schema["properties"][part];
    } else {
      return std::nullopt;
    }
  }
  if (yakka_schema["merge"].is_string()) {
    std::string merge_str = yakka_schema["merge"].get<std::string>();
    if (merge_str == "concatenate")
      return schema_validator::merge_strategy::DEFAULT;
    else if (merge_str == "max")
      return schema_validator::merge_strategy::MAX;
    else if (merge_str == "min")
      return schema_validator::merge_strategy::MIN;
    else if (merge_str == "sort")
      return schema_validator::merge_strategy::SORT;
    else if (merge_str == "unique")
      return schema_validator::merge_strategy::UNIQUE;
  }
  return std::nullopt;
}

} // namespace yakka