#include "json-patch.hpp"

#include <ryml/json-schema.hpp>

namespace
{

// originally from http://jsonpatch.com/, http://json.schemastore.org/json-patch
// with fixes
const char *patch_schema_text = R"patch({
    "title": "JSON schema for JSONPatch files",
    "$schema": "http://json-schema.org/draft-04/schema#",
    "type": "array",

    "items": {
        "oneOf": [
            {
                "additionalProperties": false,
                "required": [ "value", "op", "path"],
                "properties": {
                    "path" : { "$ref": "#/definitions/path" },
                    "op": {
                        "description": "The operation to perform.",
                        "type": "string",
                        "enum": [ "add", "replace", "test" ]
                    },
                    "value": {
                        "description": "The value to add, replace or test."
                    }
                }
            },
            {
                "additionalProperties": false,
                "required": [ "op", "path"],
                "properties": {
                    "path" : { "$ref": "#/definitions/path" },
                    "op": {
                        "description": "The operation to perform.",
                        "type": "string",
                        "enum": [ "remove" ]
                    }
                }
            },
            {
                "additionalProperties": false,
                "required": [ "from", "op", "path" ],
                "properties": {
                    "path" : { "$ref": "#/definitions/path" },
                    "op": {
                        "description": "The operation to perform.",
                        "type": "string",
                        "enum": [ "move", "copy" ]
                    },
                    "from": {
                        "$ref": "#/definitions/path",
                        "description": "A JSON Pointer path pointing to the location to move/copy from."
                    }
                }
            }
        ]
    },
    "definitions": {
        "path": {
            "description": "A JSON Pointer path.",
            "type": "string"
        }
    }
})patch";
} // namespace

namespace ryml_schema
{

json_patch::json_patch(json &&patch)
    : j_(std::move(patch))
{
	validateJsonPatch(j_);
}

json_patch::json_patch(const json &patch)
    : j_(patch)
{
	validateJsonPatch(j_);
}

json_patch &json_patch::add(const json_pointer &ptr, json value)
{
    auto root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;

    auto entry = root.append_child();
    entry |= ryml::MAP;

    auto op_node = entry.append_child();
    op_node << ryml::key("op") << "add";

    auto path_node = entry.append_child();
    const auto path = j_.to_arena(ptr.to_string());
    path_node << ryml::key("path") << path;

    auto value_node = entry.append_child();
    value_node << ryml::key("value");
    value_node.tree()->merge_with(&value, value.root_id(), value_node.id());
	return *this;
}

json_patch &json_patch::replace(const json_pointer &ptr, json value)
{
    auto root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;

    auto entry = root.append_child();
    entry |= ryml::MAP;

    auto op_node = entry.append_child();
    op_node << ryml::key("op") << "replace";

    auto path_node = entry.append_child();
    const auto path = j_.to_arena(ptr.to_string());
    path_node << ryml::key("path") << path;

    auto value_node = entry.append_child();
    value_node << ryml::key("value");
    value_node.tree()->merge_with(&value, value.root_id(), value_node.id());
	return *this;
}

json_patch &json_patch::remove(const json_pointer &ptr)
{
    auto root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;

    auto entry = root.append_child();
    entry |= ryml::MAP;

    auto op_node = entry.append_child();
    op_node << ryml::key("op") << "remove";

    auto path_node = entry.append_child();
    const auto path = j_.to_arena(ptr.to_string());
    path_node << ryml::key("path") << path;
	return *this;
}

void json_patch::validateJsonPatch(json const &patch)
{
	// static put here to have it created at the first usage of validateJsonPatch
    static json patch_schema;
    static bool patch_schema_initialized = false;
    if (!patch_schema_initialized) {
        patch_schema = ryml::parse_in_arena(ryml::to_csubstr(patch_schema_text));
        patch_schema_initialized = true;
    }
    static ryml_schema::json_validator patch_validator(patch_schema);

	patch_validator.validate(patch);
}

} // namespace ryml_schema
