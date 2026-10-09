#pragma once

#include "yakka_component.hpp"
#include "yaml-cpp/yaml.h"
#include <ryml.hpp>
#include <ryml_std.hpp>
// #include <ryml/json-schema.hpp>
#include "spdlog.h"
#include <filesystem>
#include <memory>
#include <ranges>

namespace valijson {
class Schema;
}

namespace yakka {

// clang-format off
const std::string yakka_component_schema_yaml = R"(
title: Yakka file
type: object
properties:
  name:
    description: Name
    type: string

  features:
    type: object
    description: Features provided and enabled by this component
    additionalProperties:
      type: object
      properties:
        condition:
          type: string
          minLength: 1

  requires:
    type: object
    description: Requires relationships
    properties:
      features:
        type: [array, 'null']
        merge: concatenate
        description: Collection of features
        uniqueItems: true
        items:
          type: [string, object]
      components:
        type: [array, 'null']
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
  schema();
  ~schema();
  schema(schema &&) noexcept;
  schema &operator=(schema &&) noexcept;
  explicit schema(const std::string &schema_yaml);
  explicit schema(const std::filesystem::path &schema_path);
  

  void add_schema_data(ryml::ConstNodeRef schema_data);
  // bool validate(ryml::ConstNodeRef data, std::string id = "");
  bool validate(ryml::ConstNodeRef data, ryml::csubstr id);
  ryml::ConstNodeRef operator[](const ryml::Pointer &path) const;
  ryml::ConstNodeRef operator[](const std::string &path) const;
  schema::merge_strategy get_merge_strategy(const ryml::Pointer &path) const;

private:
  ryml::Tree schema_data;
  /// Parsed form of schema_data, rebuilt lazily whenever validator_updated is false.
  std::unique_ptr<valijson::Schema> parsed_schema;
  bool validator_updated = false;
};

class yakka_schema_validator {
  schema yakka_component_schema;
  schema slcc_component_schema;

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

} // namespace yakka