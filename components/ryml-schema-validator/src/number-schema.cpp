#include "ryml-schema.hpp"
#include <cmath>

namespace ryml_schema {

number_schema::number_schema(
    std::optional<double> minimum,
    std::optional<double> maximum,
    std::optional<double> multiple_of,
    bool exclusive_minimum,
    bool exclusive_maximum
)
    : minimum_(minimum)
    , maximum_(maximum)
    , multiple_of_(multiple_of)
    , exclusive_minimum_(exclusive_minimum)
    , exclusive_maximum_(exclusive_maximum)
{}

bool number_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    if (!instance.is_val()) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Expected number, got " + std::string(type_name(instance))));
        return false;
    }

    c4::csubstr value_str = instance.val();
    double value;
    
    try {
        value = std::stod(value_str.str);
    } catch (const std::invalid_argument&) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Invalid number format"));
        return false;
    } catch (const std::out_of_range&) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Number out of range"));
        return false;
    }

    if (minimum_) {
        if (exclusive_minimum_) {
            if (value <= *minimum_) {
                handler.error(c4::csubstr(""), c4::csubstr(""), 
                            c4::csubstr("Value must be greater than " + std::to_string(*minimum_)));
                return false;
            }
        } else {
            if (value < *minimum_) {
                handler.error(c4::csubstr(""), c4::csubstr(""), 
                            c4::csubstr("Value must be greater than or equal to " + std::to_string(*minimum_)));
                return false;
            }
        }
    }

    if (maximum_) {
        if (exclusive_maximum_) {
            if (value >= *maximum_) {
                handler.error(c4::csubstr(""), c4::csubstr(""), 
                            c4::csubstr("Value must be less than " + std::to_string(*maximum_)));
                return false;
            }
        } else {
            if (value > *maximum_) {
                handler.error(c4::csubstr(""), c4::csubstr(""), 
                            c4::csubstr("Value must be less than or equal to " + std::to_string(*maximum_)));
                return false;
            }
        }
    }

    if (multiple_of_) {
        double remainder = std::fmod(value, *multiple_of_);
        if (std::abs(remainder) > 1e-10) { // Account for floating point precision
            handler.error(c4::csubstr(""), c4::csubstr(""), 
                        c4::csubstr("Value must be a multiple of " + std::to_string(*multiple_of_)));
            return false;
        }
    }

    return true;
}

} // namespace ryml_schema