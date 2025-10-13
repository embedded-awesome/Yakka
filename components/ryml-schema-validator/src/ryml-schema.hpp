#ifndef RYML_SCHEMA_HPP
#define RYML_SCHEMA_HPP

#include <ryml.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace ryml_schema {

// Forward declarations
class validator;
class error_handler;
class schema;

/**
 * @brief Custom error handler for schema validation
 */
class error_handler {
public:
    struct error {
        c4::csubstr instance_path;
        c4::csubstr schema_path;
        c4::csubstr message;
    };

    virtual void error(c4::csubstr instance_path, c4::csubstr schema_path, c4::csubstr message) {
        errors_.push_back({instance_path, schema_path, message});
    }

    const std::vector<error>& errors() const { return errors_; }
    void clear() { errors_.clear(); }

private:
    std::vector<error> errors_;
};

/**
 * @brief Base class for all schema validators
 */
class schema {
public:
    virtual ~schema() = default;
    virtual bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const = 0;
    
protected:
    static c4::csubstr type_name(const ryml::ConstNodeRef& node) {
        switch(node.type()) {
            case ryml::NOTYPE: return c4::csubstr("null");
            case ryml::VAL: return c4::csubstr("string");
            case ryml::MAP: return c4::csubstr("object");
            case ryml::SEQ: return c4::csubstr("array");
            default: return c4::csubstr("unknown");
        }
    }
};

/**
 * @brief Main validator class
 */
class validator {
public:
    validator();
    
    /**
     * @brief Load a schema from a YAML string
     */
    void load_schema(c4::csubstr schema_yaml);
    
    /**
     * @brief Validate a YAML document against the loaded schema
     */
    bool validate(c4::csubstr document_yaml, error_handler& handler);
    
    /**
     * @brief Validate a YAML node against the loaded schema
     */
    bool validate(const ryml::ConstNodeRef& document, error_handler& handler);

private:
    ryml::Tree schema_tree_;
    std::shared_ptr<schema> root_schema_;
    
    std::shared_ptr<schema> parse_schema(const ryml::ConstNodeRef& schema_node);
};

/**
 * @brief String schema validator
 */
class string_schema : public schema {
public:
    string_schema(
        std::optional<size_t> min_length = std::nullopt,
        std::optional<size_t> max_length = std::nullopt,
        std::optional<c4::csubstr> pattern = std::nullopt
    );
    
    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;

private:
    std::optional<size_t> min_length_;
    std::optional<size_t> max_length_;
    std::optional<c4::csubstr> pattern_;
};

/**
 * @brief Number schema validator
 */
class number_schema : public schema {
public:
    number_schema(
        std::optional<double> minimum = std::nullopt,
        std::optional<double> maximum = std::nullopt,
        std::optional<double> multiple_of = std::nullopt,
        bool exclusive_minimum = false,
        bool exclusive_maximum = false
    );
    
    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;

private:
    std::optional<double> minimum_;
    std::optional<double> maximum_;
    std::optional<double> multiple_of_;
    bool exclusive_minimum_;
    bool exclusive_maximum_;
};

/**
 * @brief Object schema validator
 */
class object_schema : public schema {
public:
    struct property {
        c4::csubstr name;
        std::shared_ptr<schema> schema;
        bool required;
    };

    object_schema(
        std::vector<property> properties,
        bool additional_properties = true,
        std::optional<size_t> min_properties = std::nullopt,
        std::optional<size_t> max_properties = std::nullopt
    );
    
    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;

private:
    std::vector<property> properties_;
    bool additional_properties_;
    std::optional<size_t> min_properties_;
    std::optional<size_t> max_properties_;
    std::unordered_map<c4::csubstr, size_t, c4::HashCSubstr> property_index_;
};

/**
 * @brief Array schema validator
 */
class array_schema : public schema {
public:
    array_schema(
        std::shared_ptr<schema> items,
        std::optional<size_t> min_items = std::nullopt,
        std::optional<size_t> max_items = std::nullopt,
        bool unique_items = false
    );
    
    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;

private:
    std::shared_ptr<schema> items_;
    std::optional<size_t> min_items_;
    std::optional<size_t> max_items_;
    bool unique_items_;
};

} // namespace ryml_schema

#endif // RYML_SCHEMA_HPP