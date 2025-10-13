#include "ryml-schema.hpp"
#include <regex>

namespace ryml_schema {

string_schema::string_schema(
    std::optional<size_t> min_length,
    std::optional<size_t> max_length,
    std::optional<c4::csubstr> pattern
)
    : min_length_(min_length)
    , max_length_(max_length)
    , pattern_(pattern)
{}

bool string_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    if (!instance.is_val()) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Expected string, got " + std::string(type_name(instance))));
        return false;
    }

    c4::csubstr value = instance.val();

    if (min_length_ && value.len < *min_length_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("String length " + std::to_string(value.len) + 
                                " is less than minimum " + std::to_string(*min_length_)));
        return false;
    }

    if (max_length_ && value.len > *max_length_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("String length " + std::to_string(value.len) + 
                                " is greater than maximum " + std::to_string(*max_length_)));
        return false;
    }

    if (pattern_) {
        try {
            std::regex pattern(pattern_->str);
            std::string str(value.str, value.len);
            if (!std::regex_match(str, pattern)) {
                handler.error(c4::csubstr(""), c4::csubstr(""), 
                            c4::csubstr("String does not match pattern"));
                return false;
            }
        } catch (const std::regex_error& e) {
            handler.error(c4::csubstr(""), c4::csubstr(""), 
                        c4::csubstr("Invalid regex pattern"));
            return false;
        }
    }

    return true;
}

} // namespace ryml_schema