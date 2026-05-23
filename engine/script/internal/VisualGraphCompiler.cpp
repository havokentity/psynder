// SPDX-License-Identifier: MIT
// Psynder script lane -- JSON visual graph to Lua compiler.

#include "VisualGraphCompiler.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psynder::script::detail {

namespace {

struct JsonValue {
    enum class Kind {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    f64 number = 0.0;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::string> object_keys;
    std::vector<JsonValue> object_values;

    [[nodiscard]] const JsonValue* field(std::string_view key) const noexcept {
        if (kind != Kind::Object) {
            return nullptr;
        }
        for (usize i = 0; i < object_keys.size(); ++i) {
            if (object_keys[i] == key) {
                return &object_values[i];
            }
        }
        return nullptr;
    }
};

class JsonParser {
   public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out)) {
            return false;
        }
        skip_ws();
        if (pos_ != input_.size()) {
            fail("unexpected trailing input");
            return false;
        }
        return true;
    }

   private:
    std::string_view input_;
    usize pos_ = 0;
    std::string error_;

    [[nodiscard]] char peek() const noexcept { return pos_ < input_.size() ? input_[pos_] : '\0'; }

    bool consume(char c) noexcept {
        if (peek() != c) {
            return false;
        }
        ++pos_;
        return true;
    }

    void skip_ws() noexcept {
        while (pos_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(input_[pos_]);
            if (!std::isspace(ch)) {
                break;
            }
            ++pos_;
        }
    }

    bool fail(std::string_view msg) {
        if (error_.empty()) {
            error_ = "graph json: ";
            error_ += msg;
            error_ += " at byte ";
            error_ += std::to_string(pos_);
        }
        return false;
    }

    bool parse_value(JsonValue& out) {
        skip_ws();
        const char c = peek();
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return parse_string(out.text);
        }
        if (c == '{') {
            return parse_object(out);
        }
        if (c == '[') {
            return parse_array(out);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out);
        }
        if (match_literal("true")) {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (match_literal("false")) {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (match_literal("null")) {
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        return fail("expected value");
    }

    bool match_literal(std::string_view literal) noexcept {
        if (input_.substr(pos_, literal.size()) != literal) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) {
            return fail("expected string");
        }
        out.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                return fail("unterminated string escape");
            }
            const char esc = input_[pos_++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    if (pos_ + 4 > input_.size()) {
                        return fail("short unicode escape");
                    }
                    pos_ += 4;
                    out.push_back('?');
                    break;
                default:
                    return fail("bad string escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(JsonValue& out) {
        const usize start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        } else {
            return fail("bad number");
        }
        if (peek() == '.') {
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("bad number fraction");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("bad number exponent");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }

        std::string tmp{input_.substr(start, pos_ - start)};
        char* end = nullptr;
        const f64 value = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str()) {
            return fail("bad number");
        }
        out.kind = JsonValue::Kind::Number;
        out.number = value;
        return true;
    }

    bool parse_array(JsonValue& out) {
        if (!consume('[')) {
            return fail("expected array");
        }
        out.kind = JsonValue::Kind::Array;
        out.array.clear();
        skip_ws();
        if (consume(']')) {
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parse_value(item)) {
                return false;
            }
            out.array.push_back(std::move(item));
            skip_ws();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return fail("expected ',' or ']'");
            }
        }
    }

    bool parse_object(JsonValue& out) {
        if (!consume('{')) {
            return fail("expected object");
        }
        out.kind = JsonValue::Kind::Object;
        out.object_keys.clear();
        out.object_values.clear();
        skip_ws();
        if (consume('}')) {
            return true;
        }
        while (true) {
            std::string key;
            skip_ws();
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (!consume(':')) {
                return fail("expected ':'");
            }
            JsonValue value;
            if (!parse_value(value)) {
                return false;
            }
            out.object_keys.push_back(std::move(key));
            out.object_values.push_back(std::move(value));
            skip_ws();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return fail("expected ',' or '}'");
            }
        }
    }
};

std::string trim_copy(std::string_view s) {
    usize first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    usize last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return std::string{s.substr(first, last - first)};
}

bool command_matches(std::string_view line, usize pos, std::string_view command) noexcept {
    if (line.substr(pos, command.size()) != command) {
        return false;
    }
    const usize end = pos + command.size();
    return end == line.size() || std::isspace(static_cast<unsigned char>(line[end]));
}

bool ends_with_ci(std::string_view s, std::string_view suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = s.substr(s.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(tail[i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

bool is_string(const JsonValue* v) noexcept {
    return v && v->kind == JsonValue::Kind::String;
}

bool is_array(const JsonValue* v) noexcept {
    return v && v->kind == JsonValue::Kind::Array;
}

std::string lua_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string lua_number(f64 value) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<f64>::max_digits10) << value;
    return os.str();
}

bool is_lua_identifier(std::string_view s) noexcept {
    if (s.empty()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(s.front());
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    for (const char c : s.substr(1)) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

std::string lua_key(std::string_view key) {
    if (is_lua_identifier(key)) {
        return std::string{key};
    }
    return "[" + lua_quote(key) + "]";
}

std::string lua_literal(const JsonValue& value);

std::string lua_array_literal(const JsonValue& value) {
    std::string out = "{";
    for (usize i = 0; i < value.array.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += lua_literal(value.array[i]);
    }
    out += "}";
    return out;
}

std::string lua_object_literal(const JsonValue& value) {
    std::string out = "{";
    for (usize i = 0; i < value.object_keys.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += lua_key(value.object_keys[i]);
        out += " = ";
        out += lua_literal(value.object_values[i]);
    }
    out += "}";
    return out;
}

std::string lua_literal(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::Null:
            return "nil";
        case JsonValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case JsonValue::Kind::Number:
            return lua_number(value.number);
        case JsonValue::Kind::String:
            return lua_quote(value.text);
        case JsonValue::Kind::Array:
            return lua_array_literal(value);
        case JsonValue::Kind::Object:
            return lua_object_literal(value);
    }
    return "nil";
}

const JsonValue* nested_value_field(const JsonValue& object, std::string_view key) noexcept {
    if (const JsonValue* direct = object.field(key)) {
        return direct;
    }
    const JsonValue* values = object.field("values");
    if (!values || values->kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    return values->field(key);
}

std::string string_field_or_empty(const JsonValue& object, std::string_view key) {
    const JsonValue* field = nested_value_field(object, key);
    return is_string(field) ? field->text : std::string{};
}

const JsonValue* first_field(const JsonValue& object, std::initializer_list<std::string_view> keys) {
    for (std::string_view key : keys) {
        if (const JsonValue* value = nested_value_field(object, key)) {
            return value;
        }
    }
    return nullptr;
}

std::string graph_var_name(std::string_view id, usize node_index) {
    std::string out = "__v_";
    for (const char c : id) {
        const unsigned char ch = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(ch) || ch == '_') ? c : '_');
    }
    if (out == "__v_") {
        out += "node";
    }
    out += "_";
    out += std::to_string(node_index);
    return out;
}

class GraphCompiler {
   public:
    VisualCompileResult compile(const JsonValue& root) {
        const JsonValue* nodes = root.field("nodes");
        if (!is_array(nodes)) {
            return err("graph root must contain a nodes array");
        }

        graph_name_ = string_field_or_empty(root, "name");
        target_group_ = string_field_or_empty(root, "targetGroup");
        if (target_group_.empty()) {
            target_group_ = string_field_or_empty(root, "target_group");
        }

        out_ = "-- generated by Psynder visual graph compiler\n";
        for (usize node_index = 0; node_index < nodes->array.size(); ++node_index) {
            if (!compile_node(nodes->array[node_index], node_index)) {
                return err(error_);
            }
        }

        if (const JsonValue* ret = root.field("return")) {
            if (ret->kind == JsonValue::Kind::String) {
                std::string expr;
                if (!ref(ret->text, expr)) {
                    return err(error_);
                }
                out_ += "return ";
                out_ += expr;
                out_ += "\n";
            } else if (ret->kind == JsonValue::Kind::Array) {
                out_ += "return ";
                for (usize i = 0; i < ret->array.size(); ++i) {
                    if (i > 0) {
                        out_ += ", ";
                    }
                    if (ret->array[i].kind != JsonValue::Kind::String) {
                        return err("return array entries must be node ids");
                    }
                    std::string expr;
                    if (!ref(ret->array[i].text, expr)) {
                        return err(error_);
                    }
                    out_ += expr;
                }
                out_ += "\n";
            } else {
                return err("return must be a node id or array of node ids");
            }
        }

        VisualCompileResult result;
        result.ok = true;
        result.lua_source = std::move(out_);
        return result;
    }

   private:
    std::unordered_map<std::string, std::string> vars_;
    std::string out_;
    std::string error_;
    std::string graph_name_;
    std::string target_group_;

    static VisualCompileResult err(std::string_view message) {
        VisualCompileResult result;
        result.ok = false;
        result.diagnostic = std::string{message};
        return result;
    }

    bool set_error(std::string_view message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    bool ref(std::string_view id, std::string& out) {
        const auto it = vars_.find(std::string{id});
        if (it == vars_.end()) {
            return set_error("unknown graph input '" + std::string{id} +
                             "'; referenced nodes must appear earlier in the nodes array");
        }
        out = it->second;
        return true;
    }

    bool input_expr(const JsonValue& node, std::string& out) {
        const JsonValue* input = nested_value_field(node, "input");
        if (!is_string(input)) {
            return set_error("node input must be a node id string");
        }
        return ref(input->text, out);
    }

    bool binary_inputs(const JsonValue& node, std::string& a, std::string& b) {
        const JsonValue* inputs = nested_value_field(node, "inputs");
        if (!is_array(inputs) || inputs->array.size() != 2) {
            return set_error("binary node inputs must contain two node ids");
        }
        if (inputs->array[0].kind != JsonValue::Kind::String ||
            inputs->array[1].kind != JsonValue::Kind::String) {
            return set_error("binary node inputs must be node id strings");
        }
        return ref(inputs->array[0].text, a) && ref(inputs->array[1].text, b);
    }

    std::string node_name(const JsonValue& node, usize node_index) const {
        std::string name = string_field_or_empty(node, "name");
        if (!name.empty()) {
            return name;
        }
        name = string_field_or_empty(node, "title");
        if (!name.empty()) {
            return name;
        }
        if (!graph_name_.empty()) {
            return graph_name_;
        }
        return "psygraph_node_" + std::to_string(node_index);
    }

    std::string target_group(const JsonValue& node) const {
        std::string group = string_field_or_empty(node, "targetGroup");
        if (group.empty()) {
            group = string_field_or_empty(node, "target_group");
        }
        return group.empty() ? target_group_ : group;
    }

    static std::string vec3_literal_or_default(const JsonValue* value, std::string_view fallback) {
        if (is_array(value) && value->array.size() == 3) {
            return lua_literal(*value);
        }
        return std::string{fallback};
    }

    static std::string scalar_expr(const JsonValue* value,
                                   std::string_view index_expr,
                                   std::string_view fallback) {
        if (!value) {
            return std::string{fallback};
        }
        if (value->kind == JsonValue::Kind::Number) {
            return lua_number(value->number);
        }
        if (value->kind != JsonValue::Kind::Object) {
            return std::string{fallback};
        }
        const JsonValue* type = value->field("type");
        if (is_string(type) && type->text == "linearIndex") {
            const JsonValue* base = value->field("base");
            const JsonValue* step = value->field("step");
            const std::string base_expr =
                base && base->kind == JsonValue::Kind::Number ? lua_number(base->number) : "0";
            const std::string step_expr =
                step && step->kind == JsonValue::Kind::Number ? lua_number(step->number) : "0";
            return "(" + base_expr + " + " + step_expr + " * (" + std::string{index_expr} +
                   " - 1))";
        }
        const JsonValue* constant = value->field("value");
        if (constant && constant->kind == JsonValue::Kind::Number) {
            return lua_number(constant->number);
        }
        return std::string{fallback};
    }

    bool compile_transform_behavior(const JsonValue& node, usize node_index, std::string_view op) {
        const std::string group = target_group(node);
        if (group.empty()) {
            return set_error(std::string{op} + " node requires targetGroup");
        }

        const std::string axis =
            vec3_literal_or_default(nested_value_field(node, "axis"), "{0, 1, 0}");
        const std::string name = node_name(node, node_index);
        out_ += "world:register_system({ writes = {'Transform'}, name = ";
        out_ += lua_quote(name);
        out_ += " }, function(transforms, dt)\n";
        out_ += "  for __i, __entry in ipairs(transforms) do\n";
        out_ += "    local __data = __entry.data\n";
        out_ += "    if __data and (__data.group == ";
        out_ += lua_quote(group);
        out_ += " or __data.targetGroup == ";
        out_ += lua_quote(group);
        out_ += " or __entry.archetype == ";
        out_ += lua_quote(group);
        out_ += ") then\n";
        out_ += "      local __axis = ";
        out_ += axis;
        out_ += "\n";
        if (op == "spin" || op == "transform.spin") {
            out_ += "      local __speed = ";
            out_ += scalar_expr(nested_value_field(node, "speed"), "__i", "0");
            out_ += "\n";
            out_ += "      local __phase = ";
            out_ += scalar_expr(nested_value_field(node, "phase"), "__i", "0");
            out_ += "\n";
            out_ += "      __data.rotation = __data.rotation or { x = 0, y = 0, z = 0 }\n";
            out_ += "      __data.rotation.x = __data.rotation.x + __axis[1] * (__speed * dt + __phase)\n";
            out_ += "      __data.rotation.y = __data.rotation.y + __axis[2] * (__speed * dt + __phase)\n";
            out_ += "      __data.rotation.z = __data.rotation.z + __axis[3] * (__speed * dt + __phase)\n";
        } else {
            out_ += "      local __amount = ";
            out_ += scalar_expr(nested_value_field(node, "amount"), "__i", "0");
            out_ += "\n";
            out_ += "      __data.position = __data.position or { x = 0, y = 0, z = 0 }\n";
            out_ += "      __data.position.x = __data.position.x + __axis[1] * __amount * dt\n";
            out_ += "      __data.position.y = __data.position.y + __axis[2] * __amount * dt\n";
            out_ += "      __data.position.z = __data.position.z + __axis[3] * __amount * dt\n";
        }
        out_ += "    end\n";
        out_ += "  end\n";
        out_ += "end)\n";
        return true;
    }

    std::string spawn_prop_expr(const JsonValue& node) {
        const JsonValue* prop = first_field(node, {"prop_id", "propId", "prop"});
        if (!is_string(prop)) {
            set_error("spawn_prop node requires prop_id string");
            return {};
        }

        std::string options = "{";
        bool wrote_option = false;
        if (const JsonValue* position = nested_value_field(node, "position")) {
            options += " position = ";
            options += lua_literal(*position);
            wrote_option = true;
        }
        if (const JsonValue* components = nested_value_field(node, "components")) {
            if (wrote_option) {
                options += ",";
            }
            options += " components = ";
            options += lua_literal(*components);
            wrote_option = true;
        }
        (void)wrote_option;
        options += " }";
        return "world:spawn_prop(" + lua_quote(prop->text) + ", " + options + ")";
    }

    bool compile_node(const JsonValue& node, usize node_index) {
        if (node.kind != JsonValue::Kind::Object) {
            return set_error("each node must be an object");
        }
        const JsonValue* op_value = node.field("op");
        if (!op_value) {
            op_value = node.field("type");
        }
        if (!is_string(op_value)) {
            return set_error("node op must be a string");
        }

        const std::string& op = op_value->text;
        if (op == "on_update" || op == "compiled_behavior_op") {
            return true;
        }
        if (op == "spin" || op == "transform.spin" || op == "translate" ||
            op == "transform.translate") {
            return compile_transform_behavior(node, node_index, op);
        }

        if (op == "return") {
            std::string expr;
            if (!input_expr(node, expr)) {
                return false;
            }
            out_ += "return ";
            out_ += expr;
            out_ += "\n";
            return true;
        }

        const JsonValue* id_value = node.field("id");
        if (!is_string(id_value)) {
            return set_error("node id must be a string");
        }
        const std::string var = graph_var_name(id_value->text, node_index);
        if (vars_.contains(id_value->text)) {
            return set_error("duplicate graph node id '" + id_value->text + "'");
        }

        std::string expr;
        if (op == "const") {
            const JsonValue* value = nested_value_field(node, "value");
            if (!value) {
                return set_error("const node requires value");
            }
            expr = lua_literal(*value);
        } else if (op == "add" || op == "sub" || op == "mul" || op == "div") {
            std::string a;
            std::string b;
            if (!binary_inputs(node, a, b)) {
                return false;
            }
            const char* symbol = op == "add" ? "+" : op == "sub" ? "-" : op == "mul" ? "*" : "/";
            expr = "(" + a + " " + symbol + " " + b + ")";
        } else if (op == "neg") {
            std::string a;
            if (!input_expr(node, a)) {
                return false;
            }
            expr = "(-" + a + ")";
        } else if (op == "component") {
            const JsonValue* name = nested_value_field(node, "name");
            if (!is_string(name)) {
                return set_error("component node requires name");
            }
            expr = "world:component(" + lua_quote(name->text) + ")";
        } else if (op == "spawn") {
            const JsonValue* archetype = nested_value_field(node, "archetype");
            const JsonValue* components = nested_value_field(node, "components");
            if (!is_string(archetype)) {
                return set_error("spawn node requires archetype string");
            }
            if (!components || components->kind != JsonValue::Kind::Object) {
                return set_error("spawn node requires components object");
            }
            expr =
                "world:spawn(" + lua_quote(archetype->text) + ", " + lua_literal(*components) + ")";
        } else if (op == "spawn_prop") {
            expr = spawn_prop_expr(node);
            if (!error_.empty()) {
                return false;
            }
        } else if (op == "log") {
            std::string a;
            if (!input_expr(node, a)) {
                return false;
            }
            if (const JsonValue* label = nested_value_field(node, "label"); is_string(label)) {
                out_ += "print(";
                out_ += lua_quote(label->text);
                out_ += ", ";
                out_ += a;
                out_ += ")\n";
            } else {
                out_ += "print(";
                out_ += a;
                out_ += ")\n";
            }
            expr = a;
        } else {
            return set_error("unsupported graph op '" + op + "'");
        }

        out_ += "local ";
        out_ += var;
        out_ += " = ";
        out_ += expr;
        out_ += "\n";
        vars_.emplace(id_value->text, var);
        return true;
    }
};

std::string_view repl_payload(std::string_view line) noexcept {
    usize pos = 0;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    for (std::string_view command :
         {std::string_view{":graph"}, std::string_view{":vscript"}, std::string_view{":visual"}}) {
        if (line.substr(pos, command.size()) == command) {
            const usize end = pos + command.size();
            if (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
                continue;
            }
            pos += command.size();
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
                ++pos;
            }
            return line.substr(pos);
        }
    }
    return {};
}

}  // namespace

bool is_visual_graph_repl_command(std::string_view line) noexcept {
    usize pos = 0;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    return command_matches(line, pos, ":graph") || command_matches(line, pos, ":vscript") ||
           command_matches(line, pos, ":visual");
}

VisualCompileResult compile_visual_graph_repl(std::string_view line) {
    const std::string_view payload = repl_payload(line);
    if (payload.empty()) {
        VisualCompileResult result;
        result.diagnostic = "visual graph command requires a JSON payload";
        return result;
    }
    return compile_visual_graph(payload);
}

VisualCompileResult compile_visual_graph(std::string_view graph_json) {
    JsonValue root;
    JsonParser parser(graph_json);
    if (!parser.parse(root)) {
        VisualCompileResult result;
        result.diagnostic = parser.error();
        return result;
    }
    if (root.kind != JsonValue::Kind::Object) {
        VisualCompileResult result;
        result.diagnostic = "graph root must be an object";
        return result;
    }
    if (const JsonValue* graphs = root.field("graphs")) {
        if (!is_array(graphs)) {
            VisualCompileResult result;
            result.diagnostic = "graph root graphs field must be an array";
            return result;
        }

        VisualCompileResult result;
        result.ok = true;
        result.lua_source = "-- generated by Psynder visual graph compiler\n";
        for (const JsonValue& graph : graphs->array) {
            VisualCompileResult compiled = GraphCompiler{}.compile(graph);
            if (!compiled.ok) {
                return compiled;
            }
            result.lua_source += compiled.lua_source;
        }
        return result;
    }
    return GraphCompiler{}.compile(root);
}

bool is_visual_graph_name(std::string_view name) noexcept {
    return ends_with_ci(name, ".vsg") || ends_with_ci(name, ".psygraph");
}

bool has_visual_graph_marker(std::string_view source) noexcept {
    std::string line;
    const usize eol = source.find('\n');
    if (eol == std::string_view::npos) {
        line = trim_copy(source);
    } else {
        line = trim_copy(source.substr(0, eol));
    }
    return line == "#psynder-graph" || line == "# psynder-graph" || line == "// psynder-graph" ||
           line == "-- psynder-graph";
}

std::string_view strip_visual_graph_marker(std::string_view source) noexcept {
    if (!has_visual_graph_marker(source)) {
        return source;
    }
    const usize eol = source.find('\n');
    if (eol == std::string_view::npos) {
        return {};
    }
    return source.substr(eol + 1);
}

}  // namespace psynder::script::detail
