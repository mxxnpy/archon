#pragma once
// Small JSON reader/writer for the browser-extension bridge (native messaging
// speaks JSON). Strict, bounded (depth, size, element count) and dependency
// free; enough for the few message shapes the extension sends.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    // Member access on objects; a missing key (or a non-object) gives Null.
    const Value& operator[](const std::string& key) const;
    // Typed reads with a fallback when the value has another type.
    std::string string_or(const std::string& fallback = {}) const { return type == Type::String ? str : fallback; }
    double number_or(double fallback) const { return type == Type::Number ? num : fallback; }
    bool bool_or(bool fallback) const { return type == Type::Bool ? b : fallback; }
};

struct Limits {
    size_t max_depth = 8;
    size_t max_elements = 20000;  // values in the whole document
    size_t max_string = 256 * 1024;
};

// Parses one complete JSON document (only whitespace may follow). Returns
// false on any syntax error or limit exceeded.
bool parse(const std::string& text, Value& out, const Limits& limits = {});

// Escapes a UTF-8 string as a JSON string literal (with quotes).
std::string quote(const std::string& s);

}  // namespace agent::json
