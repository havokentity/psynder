// SPDX-License-Identifier: MIT
// Psynder script lane -- native `.psy` compiler.

#include "PsyCompiler.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>

namespace psynder::script::detail {

namespace {

enum class TokenKind : u8 {
    End,
    Identifier,
    Number,
    String,
    Let,
    Print,
    Return,
    True,
    False,
    Nil,
    Equal,
    Plus,
    Minus,
    Star,
    Slash,
    LParen,
    RParen,
    Semicolon,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    f64 number = 0.0;
    usize byte = 0;
};

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

class Lexer {
   public:
    explicit Lexer(std::string_view source) : source_(source) {}

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    Token next() {
        skip_ws_and_comments();
        Token token;
        token.byte = pos_;
        if (pos_ >= source_.size()) {
            token.kind = TokenKind::End;
            return token;
        }

        const char c = source_[pos_++];
        switch (c) {
            case '=':
                token.kind = TokenKind::Equal;
                return token;
            case '+':
                token.kind = TokenKind::Plus;
                return token;
            case '-':
                token.kind = TokenKind::Minus;
                return token;
            case '*':
                token.kind = TokenKind::Star;
                return token;
            case '/':
                token.kind = TokenKind::Slash;
                return token;
            case '(':
                token.kind = TokenKind::LParen;
                return token;
            case ')':
                token.kind = TokenKind::RParen;
                return token;
            case ';':
                token.kind = TokenKind::Semicolon;
                return token;
            case '"':
                token.kind = TokenKind::String;
                read_string(token.text);
                return token;
            default:
                break;
        }

        const unsigned char ch = static_cast<unsigned char>(c);
        if (std::isalpha(ch) || c == '_') {
            token.text.push_back(c);
            while (pos_ < source_.size()) {
                const unsigned char next_ch = static_cast<unsigned char>(source_[pos_]);
                if (!(std::isalnum(next_ch) || source_[pos_] == '_')) {
                    break;
                }
                token.text.push_back(source_[pos_++]);
            }
            token.kind = keyword_kind(token.text);
            return token;
        }

        if (std::isdigit(ch) || c == '.') {
            --pos_;
            read_number(token);
            return token;
        }

        fail_at("unexpected character", token.byte);
        token.kind = TokenKind::End;
        return token;
    }

   private:
    std::string_view source_;
    usize pos_ = 0;
    std::string error_;

    void fail(std::string_view msg) { fail_at(msg, pos_); }

    void fail_at(std::string_view msg, usize byte) {
        if (error_.empty()) {
            error_ = std::string{msg} + " at byte " + std::to_string(byte);
        }
    }

    void skip_ws_and_comments() noexcept {
        while (pos_ < source_.size()) {
            const unsigned char ch = static_cast<unsigned char>(source_[pos_]);
            if (std::isspace(ch)) {
                ++pos_;
                continue;
            }
            if (source_.substr(pos_, 2) == "//") {
                pos_ += 2;
                while (pos_ < source_.size() && source_[pos_] != '\n') {
                    ++pos_;
                }
                continue;
            }
            break;
        }
    }

    static TokenKind keyword_kind(std::string_view text) noexcept {
        if (text == "let") {
            return TokenKind::Let;
        }
        if (text == "print") {
            return TokenKind::Print;
        }
        if (text == "return") {
            return TokenKind::Return;
        }
        if (text == "true") {
            return TokenKind::True;
        }
        if (text == "false") {
            return TokenKind::False;
        }
        if (text == "nil") {
            return TokenKind::Nil;
        }
        return TokenKind::Identifier;
    }

    void read_string(std::string& out) {
        out.clear();
        while (pos_ < source_.size()) {
            const char c = source_[pos_++];
            if (c == '"') {
                return;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= source_.size()) {
                fail("unterminated string escape");
                return;
            }
            const char esc = source_[pos_++];
            switch (esc) {
                case '"':
                case '\\':
                    out.push_back(esc);
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
                default:
                    fail("bad string escape");
                    return;
            }
        }
        fail("unterminated string");
    }

    void read_number(Token& token) {
        const usize start = pos_;
        bool saw_digit = false;
        while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
            saw_digit = true;
            ++pos_;
        }
        if (pos_ < source_.size() && source_[pos_] == '.') {
            ++pos_;
            while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                saw_digit = true;
                ++pos_;
            }
        }
        if (!saw_digit) {
            fail("bad number");
            token.kind = TokenKind::End;
            return;
        }
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= source_.size() || !std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                fail("bad number exponent");
                token.kind = TokenKind::End;
                return;
            }
            while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                ++pos_;
            }
        }

        const std::string text{source_.substr(start, pos_ - start)};
        char* end = nullptr;
        const f64 value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') {
            fail("bad number");
            token.kind = TokenKind::End;
            return;
        }
        token.kind = TokenKind::Number;
        token.text = text;
        token.number = value;
    }
};

class Parser {
   public:
    Parser(std::string_view source, std::string_view name) : lexer_(source), name_(name) {
        advance();
    }

    PsyCompileResult finish() {
        while (current_.kind != TokenKind::End && error_.empty()) {
            statement();
        }
        if (!lexer_.error().empty() && error_.empty()) {
            error_ = lexer_.error();
        }
        if (!error_.empty()) {
            PsyCompileResult result;
            result.diagnostic = std::string{name_} + ": " + error_;
            return result;
        }

        emit(PsyOp::PushConst, add_const(PsyValue::nil()));
        emit(PsyOp::Return);

        PsyCompileResult result;
        result.ok = true;
        result.program = std::move(program_);
        return result;
    }

   private:
    Lexer lexer_;
    std::string_view name_;
    Token current_;
    Token previous_;
    PsyProgram program_;
    std::unordered_map<std::string, u32> locals_;
    std::string error_;

    void advance() {
        previous_ = std::move(current_);
        current_ = lexer_.next();
        if (!lexer_.error().empty() && error_.empty()) {
            error_ = lexer_.error();
        }
    }

    bool check(TokenKind kind) const noexcept { return current_.kind == kind; }

    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
    }

    bool expect(TokenKind kind, std::string_view message) {
        if (check(kind)) {
            advance();
            return true;
        }
        return fail(message);
    }

    bool fail(std::string_view message) {
        if (error_.empty()) {
            error_ = std::string{message} + " at byte " + std::to_string(current_.byte);
        }
        return false;
    }

    u32 add_const(PsyValue value) {
        program_.constants.push_back(std::move(value));
        return static_cast<u32>(program_.constants.size() - 1);
    }

    void emit(PsyOp op, u32 arg = 0) { program_.code.push_back(PsyInstruction{op, arg}); }

    u32 declare_or_find_local(const std::string& name) {
        const auto found = locals_.find(name);
        if (found != locals_.end()) {
            return found->second;
        }
        const u32 slot = static_cast<u32>(program_.locals.size());
        program_.locals.push_back(name);
        locals_.emplace(name, slot);
        return slot;
    }

    bool find_local(const std::string& name, u32& slot) {
        const auto found = locals_.find(name);
        if (found == locals_.end()) {
            return fail("unknown local '" + name + "'");
        }
        slot = found->second;
        return true;
    }

    void statement() {
        if (match(TokenKind::Let)) {
            let_statement();
        } else if (match(TokenKind::Print)) {
            print_statement();
        } else if (match(TokenKind::Return)) {
            return_statement();
        } else if (check(TokenKind::Identifier)) {
            Token ident = current_;
            advance();
            if (match(TokenKind::Equal)) {
                assign_statement(ident.text);
            } else {
                primary_from_identifier(ident.text);
                term_tail();
                expression_tail();
                emit(PsyOp::Pop);
                expect(TokenKind::Semicolon, "expected ';' after expression");
            }
        } else {
            expression();
            emit(PsyOp::Pop);
            expect(TokenKind::Semicolon, "expected ';' after expression");
        }
    }

    void let_statement() {
        if (!check(TokenKind::Identifier)) {
            fail("expected local name");
            return;
        }
        const std::string name = current_.text;
        advance();
        if (!expect(TokenKind::Equal, "expected '=' after local name")) {
            return;
        }
        expression();
        const u32 slot = declare_or_find_local(name);
        emit(PsyOp::StoreLocal, slot);
        expect(TokenKind::Semicolon, "expected ';' after let");
    }

    void assign_statement(const std::string& name) {
        u32 slot = 0;
        if (!find_local(name, slot)) {
            return;
        }
        expression();
        emit(PsyOp::StoreLocal, slot);
        expect(TokenKind::Semicolon, "expected ';' after assignment");
    }

    void print_statement() {
        expression();
        emit(PsyOp::Print);
        expect(TokenKind::Semicolon, "expected ';' after print");
    }

    void return_statement() {
        expression();
        emit(PsyOp::Return);
        expect(TokenKind::Semicolon, "expected ';' after return");
    }

    void expression() {
        term();
        expression_tail();
    }

    void expression_tail() {
        while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
            const TokenKind op = previous_.kind;
            term();
            emit(op == TokenKind::Plus ? PsyOp::Add : PsyOp::Sub);
        }
    }

    void term() {
        unary();
        term_tail();
    }

    void term_tail() {
        while (match(TokenKind::Star) || match(TokenKind::Slash)) {
            const TokenKind op = previous_.kind;
            unary();
            emit(op == TokenKind::Star ? PsyOp::Mul : PsyOp::Div);
        }
    }

    void unary() {
        if (match(TokenKind::Minus)) {
            unary();
            emit(PsyOp::Neg);
            return;
        }
        primary();
    }

    void primary_from_identifier(const std::string& name) {
        u32 slot = 0;
        if (find_local(name, slot)) {
            emit(PsyOp::LoadLocal, slot);
        }
    }

    void primary() {
        if (match(TokenKind::Number)) {
            emit(PsyOp::PushConst, add_const(PsyValue::number_value(previous_.number)));
        } else if (match(TokenKind::String)) {
            emit(PsyOp::PushConst, add_const(PsyValue::string_value(previous_.text)));
        } else if (match(TokenKind::True)) {
            emit(PsyOp::PushConst, add_const(PsyValue::boolean_value(true)));
        } else if (match(TokenKind::False)) {
            emit(PsyOp::PushConst, add_const(PsyValue::boolean_value(false)));
        } else if (match(TokenKind::Nil)) {
            emit(PsyOp::PushConst, add_const(PsyValue::nil()));
        } else if (match(TokenKind::Identifier)) {
            primary_from_identifier(previous_.text);
        } else if (match(TokenKind::LParen)) {
            expression();
            expect(TokenKind::RParen, "expected ')'");
        } else {
            fail("expected expression");
        }
    }
};

std::string trim_first_line(std::string_view source) {
    const usize eol = source.find('\n');
    std::string_view line = eol == std::string_view::npos ? source : source.substr(0, eol);
    usize first = 0;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) {
        ++first;
    }
    usize last = line.size();
    while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1]))) {
        --last;
    }
    return std::string{line.substr(first, last - first)};
}

}  // namespace

PsyCompileResult compile_psy_source(std::string_view source, std::string_view name) {
    return Parser{strip_psy_script_marker(source), name.empty() ? "<psy>" : name}.finish();
}

bool is_psy_script_name(std::string_view name) noexcept {
    return ends_with_ci(name, ".psy");
}

bool is_psy_repl_command(std::string_view line) noexcept {
    usize pos = 0;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    constexpr std::string_view command = ":psy";
    if (line.substr(pos, command.size()) != command) {
        return false;
    }
    const usize end = pos + command.size();
    return end == line.size() || std::isspace(static_cast<unsigned char>(line[end]));
}

std::string_view psy_repl_payload(std::string_view line) noexcept {
    usize pos = 0;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    constexpr std::string_view command = ":psy";
    if (line.substr(pos, command.size()) != command) {
        return {};
    }
    pos += command.size();
    if (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
        return {};
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    return line.substr(pos);
}

bool has_psy_script_marker(std::string_view source) noexcept {
    const std::string line = trim_first_line(source);
    return line == "#psynder-script" || line == "# psynder-script" || line == "// psynder-script";
}

std::string_view strip_psy_script_marker(std::string_view source) noexcept {
    if (!has_psy_script_marker(source)) {
        return source;
    }
    const usize eol = source.find('\n');
    if (eol == std::string_view::npos) {
        return {};
    }
    return source.substr(eol + 1);
}

}  // namespace psynder::script::detail
