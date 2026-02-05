#pragma once

#include "yakka_component.hpp"
#include "yaml-cpp/yaml.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <nlohmann/json-schema.hpp>
#include "spdlog.h"
#include <ranges>

namespace yakka {

struct RymlPointer;

class schema {
public:
  enum class merge_strategy {
    Default,   // Default behavior (convert multipel scalar values to arrays, merge objects and arrays)
    Max,       // choose the larger numeric value
    Min,       // choose the smaller numeric value
    Append,    // append values (e.g., arrays or strings)
    Abort,     // stop and signal an error on conflict
    Sort,      // sort arrays after merging
    Unique,    // remove duplicate entries in arrays after merging
    Overwrite, // overwrite existing values
  };

public:
  schema() : schema_data(ryml::Tree()), validator(nullptr, ryml_schema::default_string_format_check)
  {
  }

  void add_schema_data(const ryml::Tree &schema_data);
  bool validate(const ryml::Tree &data, std::string id = "");
  schema::merge_strategy get_merge_strategy(const RymlPointer &path) const;

private:
  ryml::Tree schema_data;
  ryml_schema::json_validator validator;
  bool validator_updated = false;
};

class yakka_schema_validator {
  ryml::Tree yakka_schema;
  ryml::Tree slcc_schema;
  ryml_schema::json_validator yakka_validator;
  ryml_schema::json_validator slcc_validator;

  // clang-format off
  const std::string yakka_component_schema_yaml = R"(
  title: Yakka file
  type: object
  properties:
    name:
      description: Name
      type: string

    requires:
      type: object
      description: Requires relationships
      properties:
        features:
          type: [array, null]
          merge: concatenate
          description: Collection of features
          uniqueItems: true
          items:
            type: [string, object]
        components:
          type: [array, null]
          merge: concatenate
          description: Collection of components
          uniqueItems: true
          items:
            type: [string, object]

    supports:
      type: object
      description: Supporting relationships
      properties:
        features:
          type: object
          description: Collection of features
          patternProperties:
            '.*':
              type: object
        components:
          type: object
          description: Collection of components
          patternProperties:
            '.*':
              type: object

    blueprints:
      type: object
      description: Blueprints
      propertyNames:
        pattern: "^[A-Za-z_.:{][A-Za-z0-9.{}/\\\\_-]*$"
      patternProperties:
        '.*':
          type: object
          additionalProperties: false
          minProperties: 1
          properties:
            regex:
              type: string
            group:
              type: string
            depends:
              type: array
            process:
              type: array
              items:
                type: object
    choices:
      type: object
      description: Choices
      propertyNames:
        pattern: "^[A-Za-z0-9_.]*$"
      patternProperties:
        '.*':
          type: object
          additionalProperties: false
          minProperties: 1
          required:
            - description
          properties:
            description:
              type: string
            exclusive:
              type: boolean
            options:
              type: array
              items:
                type: object
                oneOf:
                  - properties:
                      feature:
                        type: string
                      label:
                        type: string
                      description:
                        type: string
                    required:
                      - feature
                  - properties:
                      component:
                        type: string
                      label:
                        type: string
                      description:
                        type: string
                    required:
                      - component
            default:
              oneOf:
                - type: object
                  properties:
                    feature:
                      type: string
                  required:
                    - feature
                - type: object
                  properties:
                    component:
                      type: string
                  required:
                    - component
                - type: array
                  items:
                    type: object


  required: 
    - name
  )";
  // clang-format on

  class custom_error_handler : public ryml_schema::basic_error_handler {
  public:
    yakka::component *component;
    void error(const RymlPointer &ptr, const ryml::Tree &instance, const std::string &message) override
    {
      ryml_schema::basic_error_handler::error(ptr, instance, message);
      spdlog::error("Validation error in '{}': {} - {} : - {}", component->file_path.generic_string(), ptr.to_string(), instance.dump(3), message);
    }
  };

public:
  static yakka_schema_validator &get()
  {
    static yakka_schema_validator the_validator;
    return the_validator;
  }

private:
  yakka_schema_validator();

public:
  yakka_schema_validator(yakka_schema_validator const &) = delete;
  void operator=(yakka_schema_validator const &)   = delete;

  bool validate(yakka::component *component);
};

// schema_validator yakka_validator();
} // namespace yakka