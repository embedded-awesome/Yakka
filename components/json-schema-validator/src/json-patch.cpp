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
    : j_(std::move(patch))
{
	validateJsonPatch(j_);
}

json_patch &json_patch::add(const json_pointer &ptr, json value)
{
    // Minimal patch representation: list of maps
    ryml::NodeRef root = j_.rootref();
    if (!root.valid())
        root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;
    ryml::NodeRef entry = root.append_child();
    entry |= ryml::MAP;
    entry["op"] << "add";
    entry["path"] << ptr.to_string();
    entry["value"] = value.rootref();
	return *this;
}

json_patch &json_patch::replace(const json_pointer &ptr, json value)
{
    ryml::NodeRef root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;
    ryml::NodeRef entry = root.append_child();
    entry |= ryml::MAP;
    entry["op"] << "replace";
    entry["path"] << ptr.to_string();
    entry["value"] = value.rootref();
	return *this;
}

json_patch &json_patch::remove(const json_pointer &ptr)
{
    ryml::NodeRef root = j_.rootref();
    if (!root.is_seq())
        root |= ryml::SEQ;
    ryml::NodeRef entry = root.append_child();
    entry |= ryml::MAP;
    entry["op"] << "remove";
    entry["path"] << ptr.to_string();
	return *this;
}

void json_patch::validateJsonPatch(json const &patch)
{
	// static put here to have it created at the first usage of validateJsonPatch
    static json patch_schema;
    if (!patch_schema.rootref().valid()) {
        patch_schema = ryml::parse_in_arena(ryml::to_csubstr(patch_schema_text));
    }
    static ryml_schema::json_validator patch_validator(patch_schema);

	patch_validator.validate(patch);
}

} // namespace ryml_schema
