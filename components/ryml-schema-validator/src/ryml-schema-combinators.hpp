#ifndef RYML_SCHEMA_COMBINATORS_HPP
#define RYML_SCHEMA_COMBINATORS_HPP

#include "ryml-schema.hpp"
#include <vector>

namespace ryml_schema {

/**
 * @brief Base class for schema combinators
 */
class combinator_schema : public schema {
protected:
    std::vector<std::shared_ptr<schema>> sub_schemas_;

public:
    explicit combinator_schema(std::vector<std::shared_ptr<schema>> sub_schemas)
        : sub_schemas_(std::move(sub_schemas)) {}
};

/**
 * @brief Implements allOf validation - all schemas must validate
 */
class all_of_schema : public combinator_schema {
public:
    using combinator_schema::combinator_schema;

    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;
};

/**
 * @brief Implements anyOf validation - at least one schema must validate
 */
class any_of_schema : public combinator_schema {
public:
    using combinator_schema::combinator_schema;

    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;
};

/**
 * @brief Implements oneOf validation - exactly one schema must validate
 */
class one_of_schema : public combinator_schema {
public:
    using combinator_schema::combinator_schema;

    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;
};

/**
 * @brief Implements not validation - schema must not validate
 */
class not_schema : public schema {
private:
    std::shared_ptr<schema> sub_schema_;

public:
    explicit not_schema(std::shared_ptr<schema> sub_schema)
        : sub_schema_(std::move(sub_schema)) {}

    bool validate(const ryml::ConstNodeRef& instance, error_handler& handler) const override;
};

} // namespace ryml_schema

#endif // RYML_SCHEMA_COMBINATORS_HPP