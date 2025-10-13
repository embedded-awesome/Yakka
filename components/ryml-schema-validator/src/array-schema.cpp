#include "ryml-schema.hpp"
#include <unordered_set>

namespace ryml_schema {

array_schema::array_schema(
    std::shared_ptr<schema> items,
    std::optional<size_t> min_items,
    std::optional<size_t> max_items,
    bool unique_items
)
    : items_(std::move(items))
    , min_items_(min_items)
    , max_items_(max_items)
    , unique_items_(unique_items)
{}

bool array_schema::validate(const ryml::ConstNodeRef& instance, error_handler& handler) const {
    if (!instance.is_seq()) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Expected array, got " + std::string(type_name(instance))));
        return false;
    }

    size_t num_items = instance.num_children();

    if (min_items_ && num_items < *min_items_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Array has fewer items than required minimum"));
        return false;
    }

    if (max_items_ && num_items > *max_items_) {
        handler.error(c4::csubstr(""), c4::csubstr(""), 
                     c4::csubstr("Array has more items than allowed maximum"));
        return false;
    }

    if (items_) {
        // Track unique values if required
        std::unordered_set<c4::csubstr, c4::HashCSubstr> unique_values;

        for (const auto& item : instance.children()) {
            if (!items_->validate(item, handler)) {
                return false;
            }

            if (unique_items_) {
                if (item.is_val()) {
                    c4::csubstr value = item.val();
                    if (!unique_values.insert(value).second) {
                        handler.error(c4::csubstr(""), c4::csubstr(""), 
                                    c4::csubstr("Duplicate value in array"));
                        return false;
                    }
                } else {
                    // TODO: Handle unique checking for complex types
                    // This would require serializing the node to a string
                    // or implementing deep comparison
                }
            }
        }
    }

    return true;
}

} // namespace ryml_schema