#include "ryml-schema.hpp"
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ryml_schema {

validator::validator() = default;

void validator::load_schema(c4::csubstr schema_yaml) {
    schema_tree_ = ryml::parse(schema_yaml);
    root_schema_ = parse_schema(schema_tree_.rootref());
}

bool validator::validate(c4::csubstr document_yaml, error_handler& handler) {
    ryml::Tree doc = ryml::parse(document_yaml);
    return validate(doc.rootref(), handler);
}

bool validator::validate(const ryml::ConstNodeRef& document, error_handler& handler) {
    if (!root_schema_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), c4::csubstr("No schema loaded"));
        return false;
    }
    return root_schema_->validate(document, handler);
}

std::shared_ptr<schema> validator::parse_schema(const ryml::ConstNodeRef& schema_node) {
    if (!schema_node.valid()) {
        return nullptr;
    }

    // Handle combinators first
    if (schema_node.has_child("allOf")) {
        std::vector<std::shared_ptr<schema>> sub_schemas;
        const auto& all_of = schema_node["allOf"];
        if (!all_of.is_seq()) {
            return nullptr;
        }
        for (const auto& sub_schema : all_of.children()) {
            auto parsed = parse_schema(sub_schema);
            if (parsed) {
                sub_schemas.push_back(std::move(parsed));
            }
        }
        return std::make_shared<all_of_schema>(std::move(sub_schemas));
    }

    if (schema_node.has_child("anyOf")) {
        std::vector<std::shared_ptr<schema>> sub_schemas;
        const auto& any_of = schema_node["anyOf"];
        if (!any_of.is_seq()) {
            return nullptr;
        }
        for (const auto& sub_schema : any_of.children()) {
            auto parsed = parse_schema(sub_schema);
            if (parsed) {
                sub_schemas.push_back(std::move(parsed));
            }
        }
        return std::make_shared<any_of_schema>(std::move(sub_schemas));
    }

    if (schema_node.has_child("oneOf")) {
        std::vector<std::shared_ptr<schema>> sub_schemas;
        const auto& one_of = schema_node["oneOf"];
        if (!one_of.is_seq()) {
            return nullptr;
        }
        for (const auto& sub_schema : one_of.children()) {
            auto parsed = parse_schema(sub_schema);
            if (parsed) {
                sub_schemas.push_back(std::move(parsed));
            }
        }
        return std::make_shared<one_of_schema>(std::move(sub_schemas));
    }

    if (schema_node.has_child("not")) {
        auto sub_schema = parse_schema(schema_node["not"]);
        if (sub_schema) {
            return std::make_shared<not_schema>(std::move(sub_schema));
        }
        return nullptr;
    }

    // Handle type-specific schema
    if (schema_node.has_child("type")) {
        c4::csubstr type = schema_node["type"].val();
        
        if (type == c4::csubstr("string")) {
            std::optional<size_t> min_length;
            std::optional<size_t> max_length;
            std::optional<c4::csubstr> pattern;

            if (schema_node.has_child("minLength")) {
                min_length = std::stoul(schema_node["minLength"].val().str);
            }
            if (schema_node.has_child("maxLength")) {
                max_length = std::stoul(schema_node["maxLength"].val().str);
            }
            if (schema_node.has_child("pattern")) {
                pattern = schema_node["pattern"].val();
            }

            return std::make_shared<string_schema>(min_length, max_length, pattern);
        }
        else if (type == c4::csubstr("number") || type == c4::csubstr("integer")) {
            std::optional<double> minimum;
            std::optional<double> maximum;
            std::optional<double> multiple_of;
            bool exclusive_minimum = false;
            bool exclusive_maximum = false;

            if (schema_node.has_child("minimum")) {
                minimum = std::stod(schema_node["minimum"].val().str);
            }
            if (schema_node.has_child("maximum")) {
                maximum = std::stod(schema_node["maximum"].val().str);
            }
            if (schema_node.has_child("multipleOf")) {
                multiple_of = std::stod(schema_node["multipleOf"].val().str);
            }
            if (schema_node.has_child("exclusiveMinimum")) {
                exclusive_minimum = schema_node["exclusiveMinimum"].val() == c4::csubstr("true");
            }
            if (schema_node.has_child("exclusiveMaximum")) {
                exclusive_maximum = schema_node["exclusiveMaximum"].val() == c4::csubstr("true");
            }

            return std::make_shared<number_schema>(minimum, maximum, multiple_of, 
                                                 exclusive_minimum, exclusive_maximum);
        }
        else if (type == c4::csubstr("object")) {
            std::vector<object_schema::property> properties;
            bool additional_properties = true;
            std::optional<size_t> min_properties;
            std::optional<size_t> max_properties;

            if (schema_node.has_child("properties")) {
                const auto& props = schema_node["properties"];
                for (const auto& prop : props.children()) {
                    auto prop_schema = parse_schema(prop);
                    if (prop_schema) {
                        properties.push_back({prop.key(), prop_schema, false});
                    }
                }
            }

            if (schema_node.has_child("required")) {
                const auto& required = schema_node["required"];
                for (const auto& req : required.children()) {
                    c4::csubstr req_name = req.val();
                    for (auto& prop : properties) {
                        if (prop.name == req_name) {
                            prop.required = true;
                            break;
                        }
                    }
                }
            }

            if (schema_node.has_child("additionalProperties")) {
                const auto& add_props = schema_node["additionalProperties"];
                additional_properties = add_props.val() == c4::csubstr("true");
            }

            if (schema_node.has_child("minProperties")) {
                min_properties = std::stoul(schema_node["minProperties"].val().str);
            }
            if (schema_node.has_child("maxProperties")) {
                max_properties = std::stoul(schema_node["maxProperties"].val().str);
            }

            return std::make_shared<object_schema>(properties, additional_properties,
                                                 min_properties, max_properties);
        }
        else if (type == c4::csubstr("array")) {
            std::shared_ptr<schema> items;
            std::optional<size_t> min_items;
            std::optional<size_t> max_items;
            bool unique_items = false;

            if (schema_node.has_child("items")) {
                items = parse_schema(schema_node["items"]);
            }

            if (schema_node.has_child("minItems")) {
                min_items = std::stoul(schema_node["minItems"].val().str);
            }
            if (schema_node.has_child("maxItems")) {
                max_items = std::stoul(schema_node["maxItems"].val().str);
            }
            if (schema_node.has_child("uniqueItems")) {
                unique_items = schema_node["uniqueItems"].val() == c4::csubstr("true");
            }

            return std::make_shared<array_schema>(items, min_items, max_items, unique_items);
        }
    }

    // TODO: Handle combinators (allOf, anyOf, oneOf, not)
    // TODO: Handle references ($ref)

    return nullptr;
}

} // namespace ryml_schema