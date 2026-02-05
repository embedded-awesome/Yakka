#include <gtest/gtest.h>

#include <ryml/json-schema.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

ryml_schema::json parse_tree(std::string_view text)
{
	return ryml::parse_in_arena(ryml::to_csubstr(text));
}

class collecting_error_handler : public ryml_schema::error_handler
{
public:
	std::vector<std::string> messages;

	void error(const ryml_schema::json_pointer &ptr,
	           const ryml_schema::json & /* instance */,
	           const std::string &message) override
	{
		messages.push_back(ptr.to_string() + ": " + message);
	}

	bool empty() const { return messages.empty(); }
};

} // namespace

TEST(JsonSchemaValidatorTest, ValidObjectPasses)
{
	auto schema = parse_tree(R"(
{
  "type": "object",
  "properties": {
    "name": { "type": "string" },
    "age": { "type": "integer", "minimum": 0 }
  },
  "required": ["name"]
}
)");

	ryml_schema::json_validator validator(schema);
	collecting_error_handler handler;

	auto instance = parse_tree(R"({ "name": "Alice", "age": 30 })");
	validator.validate(instance, handler);

	EXPECT_TRUE(handler.empty());
}

TEST(JsonSchemaValidatorTest, MissingRequiredFails)
{
	auto schema = parse_tree(R"(
{
  "type": "object",
  "properties": {
    "name": { "type": "string" },
    "age": { "type": "integer" }
  },
  "required": ["name"]
}
)");

	ryml_schema::json_validator validator(schema);
	collecting_error_handler handler;

	auto instance = parse_tree(R"({ "age": 12 })");
	validator.validate(instance, handler);

	EXPECT_FALSE(handler.empty());
}

TEST(JsonSchemaValidatorTest, TypeMismatchFails)
{
	auto schema = parse_tree(R"(
{
  "type": "object",
  "properties": {
    "age": { "type": "integer" }
  },
  "required": ["age"]
}
)");

	ryml_schema::json_validator validator(schema);
	collecting_error_handler handler;

	auto instance = parse_tree(R"({ "age": "old" })");
	validator.validate(instance, handler);

	EXPECT_FALSE(handler.empty());
}

TEST(JsonSchemaValidatorTest, ArrayItemTypeEnforced)
{
	auto schema = parse_tree(R"(
{
  "type": "array",
  "items": { "type": "number" },
  "minItems": 2
}
)");

	ryml_schema::json_validator validator(schema);
	collecting_error_handler handler;

	auto instance = parse_tree(R"([1, 2, "bad"])" );
	validator.validate(instance, handler);

	EXPECT_FALSE(handler.empty());
}
