#include "ryml-schema-combinators.hpp"

namespace ryml_schema {

bool all_of_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    // Create a new error handler for collecting sub-schema errors
    error_handler sub_handler;
    
    for (const auto& schema : sub_schemas_) {
        if (!schema->validate(instance, sub_handler)) {
            // Combine error messages from sub-handler
            for (const auto& error : sub_handler.errors()) {
                handler.error(error.instance_path, error.schema_path,
                            c4::csubstr("allOf schema validation failed: " + 
                                      std::string(error.message.str, error.message.len)));
            }
            return false;
        }
    }
    
    return true;
}

bool any_of_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    std::vector<error_handler> sub_handlers(sub_schemas_.size());
    bool any_valid = false;
    
    for (size_t i = 0; i < sub_schemas_.size(); ++i) {
        if (sub_schemas_[i]->validate(instance, sub_handlers[i])) {
            any_valid = true;
            break;
        }
    }
    
    if (!any_valid) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("anyOf validation failed - none of the schemas matched"));
        
        // Add all sub-schema errors for debugging
        for (const auto& sub_handler : sub_handlers) {
            for (const auto& error : sub_handler.errors()) {
                handler.error(error.instance_path, error.schema_path,
                            c4::csubstr("Schema validation error: " + 
                                      std::string(error.message.str, error.message.len)));
            }
        }
        
        return false;
    }
    
    return true;
}

bool one_of_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    std::vector<error_handler> sub_handlers(sub_schemas_.size());
    int valid_count = 0;
    size_t last_valid_index = 0;
    
    for (size_t i = 0; i < sub_schemas_.size(); ++i) {
        if (sub_schemas_[i]->validate(instance, sub_handlers[i])) {
            valid_count++;
            last_valid_index = i;
            if (valid_count > 1) {
                break;  // More than one schema validated
            }
        }
    }
    
    if (valid_count == 0) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("oneOf validation failed - no schema matched"));
        
        // Add all sub-schema errors for debugging
        for (const auto& sub_handler : sub_handlers) {
            for (const auto& error : sub_handler.errors()) {
                handler.error(error.instance_path, error.schema_path,
                            c4::csubstr("Schema validation error: " + 
                                      std::string(error.message.str, error.message.len)));
            }
        }
        
        return false;
    }
    
    if (valid_count > 1) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("oneOf validation failed - multiple schemas matched"));
        return false;
    }
    
    return true;
}

bool not_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    error_handler sub_handler;
    
    bool is_valid = sub_schema_->validate(instance, sub_handler);
    
    if (is_valid) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("not validation failed - schema should not validate"));
        return false;
    }
    
    return true;
}

} // namespace ryml_schema