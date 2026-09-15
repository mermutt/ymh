#include "ymh/tools/tool.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include "ymh/execution/output.hpp"

namespace ymh {
namespace {

constexpr std::string_view kSchemaKeywords[] = {
    "type", "description", "enum", "default", "items",
    "properties", "required", "additionalProperties",
};

bool is_allowed_keyword(std::string_view key) {
    return std::find(std::begin(kSchemaKeywords), std::end(kSchemaKeywords), key) !=
           std::end(kSchemaKeywords);
}

bool is_scalar(const nlohmann::json& value) {
    return value.is_string() || value.is_number() || value.is_boolean();
}

void validate_subschema(const nlohmann::json& schema) {
    if (!schema.is_object()) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "subschema must be an object"};
    }
    for (const auto& [key, value] : schema.items()) {
        (void)value;
        if (!is_allowed_keyword(key)) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "unsupported schema keyword: " + key};
        }
    }
    if (!schema.contains("type") || !schema["type"].is_string()) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "subschema requires a string type"};
    }
    const std::string type = schema["type"].get<std::string>();
    static const std::string_view allowed[] = {"object", "string", "integer",
                                               "number", "boolean", "array"};
    if (std::find(std::begin(allowed), std::end(allowed), type) ==
        std::end(allowed)) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "unsupported schema type: " + type};
    }
    if (schema.contains("enum")) {
        if (!schema["enum"].is_array()) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "enum must be an array"};
        }
        for (const auto& value : schema["enum"]) {
            if (!is_scalar(value)) {
                throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                        "enum values must be scalars"};
            }
        }
    }
    if (schema.contains("default") && !is_scalar(schema["default"])) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "default must be a scalar"};
    }
    if (type == "object") {
        if (!schema.contains("additionalProperties") ||
            schema["additionalProperties"] != false) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "object schemas require additionalProperties=false"};
        }
        if (!schema.contains("properties") || !schema["properties"].is_object()) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "object schemas require properties"};
        }
        for (const auto& [name, sub] : schema["properties"].items()) {
            (void)name;
            validate_subschema(sub);
        }
    }
    if (type == "array") {
        if (!schema.contains("items")) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "array schemas require items"};
        }
        validate_subschema(schema["items"]);
    }
}

bool value_matches(const nlohmann::json& schema, const nlohmann::json& value);

bool object_matches(const nlohmann::json& schema, const nlohmann::json& value) {
    if (!value.is_object()) {
        return false;
    }
    if (schema.contains("required")) {
        for (const auto& required : schema["required"]) {
            if (!required.is_string() || !value.contains(required.get<std::string>())) {
                return false;
            }
        }
    }
    const nlohmann::json& properties = schema.at("properties");
    for (const auto& [key, item] : value.items()) {
        if (!properties.contains(key)) {
            if (schema.contains("additionalProperties") &&
                schema["additionalProperties"] == false) {
                return false;
            }
            continue;
        }
        if (!value_matches(properties.at(key), item)) {
            return false;
        }
    }
    return true;
}

bool value_matches(const nlohmann::json& schema, const nlohmann::json& value) {
    const std::string type = schema.at("type").get<std::string>();
    bool matches = false;
    if (type == "string") {
        matches = value.is_string();
    } else if (type == "integer") {
        matches = value.is_number_integer();
    } else if (type == "number") {
        matches = value.is_number();
    } else if (type == "boolean") {
        matches = value.is_boolean();
    } else if (type == "array") {
        matches = value.is_array();
        if (matches) {
            for (const auto& item : value) {
                if (!value_matches(schema.at("items"), item)) {
                    return false;
                }
            }
        }
    } else if (type == "object") {
        matches = object_matches(schema, value);
    }
    if (!matches) {
        return false;
    }
    if (schema.contains("enum")) {
        bool found = false;
        for (const auto& allowed : schema["enum"]) {
            if (allowed == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_valid_tool_name(std::string_view name) noexcept {
    const auto segment_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    if (name.empty() || !(name.front() >= 'a' && name.front() <= 'z')) {
        return false;
    }
    std::size_t i = 1;
    while (i < name.size() && name[i] != '.') {
        if (!segment_char(name[i])) {
            return false;
        }
        ++i;
    }
    while (i < name.size()) {
        ++i;
        if (i >= name.size()) {
            return false;
        }
        const std::size_t start = i;
        while (i < name.size() && name[i] != '.') {
            if (!segment_char(name[i])) {
                return false;
            }
            ++i;
        }
        if (i == start) {
            return false;
        }
    }
    return true;
}

void validate_input_schema(const nlohmann::json& schema) {
    if (!schema.is_object()) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "input_schema must be an object"};
    }
    if (!schema.contains("type") || schema["type"] != "object") {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "input_schema type must be \"object\""};
    }
    if (!schema.contains("additionalProperties") ||
        schema["additionalProperties"] != false) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "input_schema requires additionalProperties=false"};
    }
    if (!schema.contains("properties") || !schema["properties"].is_object()) {
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "input_schema requires a properties object"};
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) {
            throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                    "required must be an array"};
        }
        for (const auto& name : schema["required"]) {
            if (!name.is_string() ||
                !schema["properties"].contains(name.get<std::string>())) {
                throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                        "required names must be declared properties"};
            }
        }
    }
    for (const auto& [key, value] : schema.items()) {
        (void)value;
        if (key == "type" || key == "properties" || key == "required" ||
            key == "additionalProperties" || key == "description") {
            continue;
        }
        throw ToolRegistryError{ToolRegistryErrorCode::InvalidSchema,
                                "unsupported schema keyword: " + key};
    }
    for (const auto& [name, sub] : schema["properties"].items()) {
        (void)name;
        validate_subschema(sub);
    }
}

bool schema_validate(const nlohmann::json& schema, const nlohmann::json& arguments) {
    if (!arguments.is_object()) {
        return false;
    }
    if (schema.value("type", std::string{}) != "object") {
        return false;
    }
    return object_matches(schema, arguments);
}

bool clamp_tool_result(ToolResult& result, std::size_t max_bytes) {
    if (max_bytes == 0) {
        max_bytes = 1;
    }
    const auto serialized_size = [&result]() {
        nlohmann::json json = result;
        return json.dump().size();
    };
    if (serialized_size() <= max_bytes) {
        return false;
    }

    result.truncated = true;
    while (!result.output.empty() && serialized_size() > max_bytes) {
        const std::size_t remove = std::max<std::size_t>(1, result.output.size() / 8);
        result.output.erase(0, remove);
        result.output = std::string(trim_to_utf8_start(result.output));
    }
    if (serialized_size() > max_bytes) {
        result.output.clear();
    }
    return true;
}

} // namespace ymh
