// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  dialogue/expression.h
//
//  The small expression language the design data uses in a conversation's conditions ("Node_IsPlayerCitizen(GetPlayer()) && GetCurrentNode(GetPlayer()) != $#8574414081702566619:6064630651307950121$"):
//
//    program  := (`const` name `=` expr `;`)* expr [`;`]
//    expr     := the C operators ! - * / % + - < <= > >= == != && || over numbers, `true`, `false`, strings, scoped names (EBuildingState::UnderConstruction), a record
//                reference ($#<type>:<guid>$, which reads as the guid), the names a `const` gave, parentheses and calls  name(args)
//
//  Every value is an integer (a bool is 0 or 1, an object is the number its function gave it) with the text it was written as, which an enum or a string keeps
//  (two values with a text are equal when the texts are).  The functions are the caller's (Facts::call): a call it does not know ends the evaluation with no value,
//  and the caller treats the condition as not holding.  An empty text is 0.
// ============================================================================
#pragma once

#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aoc::worldd::dialogue {

/// One value of the expression language.
struct ExprValue {
    int64_t     i = 0;
    std::string text;   // set for an enum constant (EAoCExpansionState::Vote) or a string literal: it is what such a value is compared by
    bool operator==(const ExprValue&) const = default;
    static ExprValue of(int64_t v) { ExprValue e; e.i = v; return e; }
};

/// The functions the language calls and the dice it rolls: nullopt from `call` is "I do not know that function" (or its arguments).
class ExprHost {
public:
    virtual ~ExprHost() = default;
    virtual std::optional<ExprValue> call(const std::string& name, const std::vector<ExprValue>& args) const = 0;
    /// A whole number in [lo, hi] (RandIntRange).
    virtual int64_t random_range(int64_t lo, int64_t hi) const = 0;
};

namespace expr_detail {

enum class Tok { End, Number, Name, Text, Record, Op };

struct Token {
    Tok         kind = Tok::End;
    std::string text;
    int64_t     number = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    /// The tokens, or nullopt for a character the language does not have.
    std::optional<std::vector<Token>> run() {
        std::vector<Token> out;
        while (true) {
            skip_space();
            if (pos_ >= s_.size()) break;
            const char c = s_[pos_];
            Token t;
            if (std::isdigit(static_cast<unsigned char>(c))) {
                int64_t v = 0;
                bool real = false;
                while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) v = v * 10 + (s_[pos_++] - '0');
                if (pos_ < s_.size() && s_[pos_] == '.') {
                    real = true;
                    ++pos_;
                    while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
                }
                if (real) return std::nullopt;   // no fractions in the language this reads
                t.kind = Tok::Number;
                t.number = v;
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                const size_t b = pos_;
                while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || (s_[pos_] == ':' && pos_ + 2 < s_.size() && s_[pos_ + 1] == ':' && (std::isalpha(static_cast<unsigned char>(s_[pos_ + 2])) || s_[pos_ + 2] == '_')))) {
                    if (s_[pos_] == ':') pos_ += 2; else ++pos_;
                }
                t.kind = Tok::Name;
                t.text = std::string(s_.substr(b, pos_ - b));
            } else if (c == '"') {
                ++pos_;
                const size_t b = pos_;
                while (pos_ < s_.size() && s_[pos_] != '"') ++pos_;
                if (pos_ >= s_.size()) return std::nullopt;
                t.kind = Tok::Text;
                t.text = std::string(s_.substr(b, pos_ - b));
                ++pos_;
            } else if (c == '$' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '#') {
                // $#<type id>:<guid>$ -- a reference to a record: its value is the guid
                size_t p = pos_ + 2;
                while (p < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p]))) ++p;
                if (p >= s_.size() || s_[p] != ':') return std::nullopt;
                ++p;
                int64_t g = 0;
                const size_t gb = p;
                while (p < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p]))) g = g * 10 + (s_[p++] - '0');
                if (p == gb || p >= s_.size() || s_[p] != '$') return std::nullopt;
                pos_ = p + 1;
                t.kind = Tok::Record;
                t.number = g;
            } else {
                static const char* const two[] = {"&&", "||", "==", "!=", ">=", "<="};
                bool matched = false;
                for (const char* o : two) {
                    if (s_.compare(pos_, 2, o) == 0) { t.kind = Tok::Op; t.text = o; pos_ += 2; matched = true; break; }
                }
                if (!matched) {
                    if (std::string_view("!<>+-*/%(),;=").find(c) == std::string_view::npos) return std::nullopt;
                    t.kind = Tok::Op;
                    t.text = std::string(1, c);
                    ++pos_;
                }
            }
            out.push_back(std::move(t));
        }
        return out;
    }

private:
    void skip_space() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    std::string_view s_;
    size_t           pos_ = 0;
};

class Parser {
public:
    Parser(const std::vector<Token>& t, const ExprHost& host) : t_(t), host_(host) {}

    std::optional<ExprValue> program() {
        std::optional<ExprValue> last = ExprValue::of(0);
        while (pos_ < t_.size()) {
            if (is_name("const")) {
                ++pos_;
                if (pos_ >= t_.size() || t_[pos_].kind != Tok::Name) return std::nullopt;
                const std::string name = t_[pos_++].text;
                if (!is_op("=")) return std::nullopt;
                ++pos_;
                const auto v = or_expr();
                if (!v) return std::nullopt;
                consts_[name] = *v;
                last = ExprValue::of(0);
            } else {
                last = or_expr();
                if (!last) return std::nullopt;
            }
            if (is_op(";")) { ++pos_; continue; }
            break;
        }
        if (pos_ != t_.size()) return std::nullopt;   // something the grammar did not use
        return last;
    }

private:
    bool is_op(const char* o) const { return pos_ < t_.size() && t_[pos_].kind == Tok::Op && t_[pos_].text == o; }
    bool is_name(const char* n) const { return pos_ < t_.size() && t_[pos_].kind == Tok::Name && t_[pos_].text == n; }

    static bool truthy(const ExprValue& v) { return v.i != 0; }
    static ExprValue boolean(bool b) { return ExprValue::of(b ? 1 : 0); }

    std::optional<ExprValue> or_expr() {
        auto l = and_expr();
        while (l && is_op("||")) {
            ++pos_;
            auto r = and_expr();
            if (!r) return std::nullopt;
            l = boolean(truthy(*l) || truthy(*r));
        }
        return l;
    }
    std::optional<ExprValue> and_expr() {
        auto l = eq_expr();
        while (l && is_op("&&")) {
            ++pos_;
            auto r = eq_expr();
            if (!r) return std::nullopt;
            l = boolean(truthy(*l) && truthy(*r));
        }
        return l;
    }
    std::optional<ExprValue> eq_expr() {
        auto l = rel_expr();
        while (l && (is_op("==") || is_op("!="))) {
            const bool eq = t_[pos_].text == "==";
            ++pos_;
            auto r = rel_expr();
            if (!r) return std::nullopt;
            const bool same = (!l->text.empty() || !r->text.empty()) ? l->text == r->text : l->i == r->i;
            l = boolean(eq ? same : !same);
        }
        return l;
    }
    std::optional<ExprValue> rel_expr() {
        auto l = add_expr();
        while (l && (is_op("<") || is_op("<=") || is_op(">") || is_op(">="))) {
            const std::string op = t_[pos_].text;
            ++pos_;
            auto r = add_expr();
            if (!r) return std::nullopt;
            l = boolean(op == "<" ? l->i < r->i : op == "<=" ? l->i <= r->i : op == ">" ? l->i > r->i : l->i >= r->i);
        }
        return l;
    }
    std::optional<ExprValue> add_expr() {
        auto l = mul_expr();
        while (l && (is_op("+") || is_op("-"))) {
            const bool plus = t_[pos_].text == "+";
            ++pos_;
            auto r = mul_expr();
            if (!r) return std::nullopt;
            l = ExprValue::of(plus ? l->i + r->i : l->i - r->i);
        }
        return l;
    }
    std::optional<ExprValue> mul_expr() {
        auto l = unary();
        while (l && (is_op("*") || is_op("/") || is_op("%"))) {
            const char op = t_[pos_].text[0];
            ++pos_;
            auto r = unary();
            if (!r) return std::nullopt;
            if (op == '*') l = ExprValue::of(l->i * r->i);
            else if (r->i == 0) return std::nullopt;
            else l = ExprValue::of(op == '/' ? l->i / r->i : l->i % r->i);
        }
        return l;
    }
    std::optional<ExprValue> unary() {
        if (is_op("!")) { ++pos_; auto v = unary(); if (!v) return std::nullopt; return boolean(!truthy(*v)); }
        if (is_op("-")) { ++pos_; auto v = unary(); if (!v) return std::nullopt; return ExprValue::of(-v->i); }
        return primary();
    }
    std::optional<ExprValue> primary() {
        if (pos_ >= t_.size()) return std::nullopt;
        const Token& t = t_[pos_];
        if (t.kind == Tok::Number) { ++pos_; return ExprValue::of(t.number); }
        if (t.kind == Tok::Record) { ++pos_; return ExprValue::of(t.number); }
        if (t.kind == Tok::Text) { ++pos_; ExprValue v; v.text = t.text; return v; }
        if (is_op("(")) {
            ++pos_;
            auto v = or_expr();
            if (!v || !is_op(")")) return std::nullopt;
            ++pos_;
            return v;
        }
        if (t.kind != Tok::Name) return std::nullopt;
        ++pos_;
        if (t.text == "true") return ExprValue::of(1);
        if (t.text == "false") return ExprValue::of(0);
        if (is_op("(")) {
            ++pos_;
            std::vector<ExprValue> args;
            if (!is_op(")")) {
                while (true) {
                    auto a = or_expr();
                    if (!a) return std::nullopt;
                    args.push_back(std::move(*a));
                    if (is_op(",")) { ++pos_; continue; }
                    break;
                }
            }
            if (!is_op(")")) return std::nullopt;
            ++pos_;
            if (t.text == "RandIntRange" && args.size() == 2) return ExprValue::of(host_.random_range(args[0].i, args[1].i));
            return host_.call(t.text, args);
        }
        if (const auto it = consts_.find(t.text); it != consts_.end()) return it->second;
        if (t.text.find("::") != std::string::npos) { ExprValue v; v.text = t.text; return v; }   // an enum constant
        return std::nullopt;   // a name nothing defines
    }

    const std::vector<Token>&          t_;
    const ExprHost&                    host_;
    size_t                             pos_ = 0;
    std::map<std::string, ExprValue>   consts_;
};

}  // namespace expr_detail

/// Evaluates `text`; nullopt when it does not read or calls something `host` does not know.
inline std::optional<ExprValue> evaluate_expression(std::string_view text, const ExprHost& host) {
    expr_detail::Lexer lex(text);
    const auto tokens = lex.run();
    if (!tokens) return std::nullopt;
    if (tokens->empty()) return ExprValue::of(0);
    expr_detail::Parser p(*tokens, host);
    return p.program();
}

}  // namespace aoc::worldd::dialogue
