#include "ryml-schema.hpp"

namespace ryml_schema {

object_schema::object_schema(
    std::vector<property> properties,
    bool additional_properties,
    std::optional<size_t> min_properties,
    std::optional<size_t> max_properties
)
    : properties_(std::move(properties))
    , additional_properties_(additional_properties)
    , min_properties_(min_properties)
    , max_properties_(max_properties)
{
    // Build property index for fast lookup
    for (size_t i = 0; i < properties_.size(); ++i) {
        property_index_[properties_[i].name] = i;
    }
}

bool object_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    if (!instance.is_map()) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Expected object, got " + std::string(type_name(instance))));
        return false;
    }

    size_t num_properties = instance.num_children();

    if (min_properties_ && num_properties < *min_properties_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Object has fewer properties than required minimum"));
        return false;
    }

    if (max_properties_ && num_properties > *max_properties_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Object has more properties than allowed maximum"));
        return false;
    }

    // Track validated properties
    std::vector<bool> found_properties(properties_.size(), false);

    // Validate all properties in the instance
    for (const auto& child : instance.children()) {
        c4::csubstr prop_name = child.key();
        auto it = property_index_.find(prop_name);

        if (it != property_index_.end()) {
            // Property is defined in schema
            const auto& prop = properties_[it->second];
            found_properties[it->second] = true;

            if (!prop.schema->validate(child, handler)) {
                return false;
            }
        } else if (!additional_properties_) {
            // Property not in schema and additional properties not allowed
            handler.error(c4::csubstr(""), c4::csubstr(""), 
                         c4::csubstr("Additional property '" + std::string(prop_name.str, prop_name.len) + "' not allowed"));
            return false;
        }
    }

    // Check required properties
    for (size_t i = 0; i < properties_.size(); ++i) {
        if (properties_[i].required && !found_properties[i]) {
            handler.error(c4::csubstr(""), c4::csubstr(""), 
                         c4::csubstr("Required property '" + 
                                   std::string(properties_[i].name.str, properties_[i].name.len) + 
                                   "' is missing"));
            return false;
        }
    }

    return true;
}

} // namespace ryml_schema