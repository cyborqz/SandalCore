// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  fishing/predicate.cpp (see predicate.h)
// ============================================================================
#include "fishing/predicate.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <vector>

namespace aoc::worldd::fishing {

namespace {

struct Token {
    enum Kind { End, Number, String, Name, Op } kind = End;
    std::string text;
    double      value = 0;
};

/// The tokens of the text; false when a character is none the shapes use.
bool lex(std::string_view s, std::vector<Token>& out) {
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {   // a comment to the end of the line
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            double v = 0;
            const auto r = std::from_chars(s.data() + i, s.data() + s.size(), v);
            if (r.ec != std::errc()) return false;
            out.push_back({Token::Number, {}, v});
            i = static_cast<size_t>(r.ptr - s.data());
        } else if (c == '"') {
            const size_t end = s.find('"', i + 1);
            if (end == std::string_view::npos) return false;
            out.push_back({Token::String, std::string(s.substr(i + 1, end - i - 1)), 0});
            i = end + 1;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || (s[j] == ':' && j + 1 < s.size() && s[j + 1] == ':') || (s[j] == ':' && j > 0 && s[j - 1] == ':'))) ++j;
            out.push_back({Token::Name, std::string(s.substr(i, j - i)), 0});
            i = j;
        } else {
            // operators: two-character ones first
            static const char* const two[] = {">=", "<=", "==", "!=", "&&", "||"};
            bool matched = false;
            for (const char* op : two) {
                if (s.compare(i, 2, op) == 0) {
                    out.push_back({Token::Op, op, 0});
                    i += 2;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (c == '>' || c == '<' || c == '!' || c == '(' || c == ')' || c == ',' || c == '+' || c == '-') {
                out.push_back({Token::Op, std::string(1, c), 0});
                ++i;
            } else {
                return false;
            }
        }
    }
    out.push_back({Token::End, {}, 0});
    return true;
}

class Parser {
public:
    Parser(const std::vector<Token>& t, const Facts& f) : t_(t), f_(f) {}

    std::optional<double> run() {
        const std::optional<double> v = or_();
        if (!v || ok_ == false || t_[at_].kind != Token::End) return std::nullopt;
        return v;
    }

private:
    const Token& peek() const { return t_[at_]; }
    bool op(const char* s) const { return peek().kind == Token::Op && peek().text == s; }

    std::optional<double> or_() {
        std::optional<double> l = and_();
        while (l && op("||")) {
            ++at_;
            const std::optional<double> r = and_();
            if (!r) return std::nullopt;
            l = (*l != 0 || *r != 0) ? 1.0 : 0.0;
        }
        return l;
    }
    std::optional<double> and_() {
        std::optional<double> l = not_();
        while (l && op("&&")) {
            ++at_;
            const std::optional<double> r = not_();
            if (!r) return std::nullopt;
            l = (*l != 0 && *r != 0) ? 1.0 : 0.0;
        }
        return l;
    }
    std::optional<double> not_() { return cmp(); }   // (the `!` is in primary(): it binds tighter than a comparison, as in C)
    std::optional<double> cmp() {
        const std::optional<double> l = sum();
        if (!l) return std::nullopt;
        static const char* const ops[] = {">=", "<=", "==", "!=", ">", "<"};
        for (const char* o : ops) {
            if (!op(o)) continue;
            ++at_;
            const std::optional<double> r = sum();
            if (!r) return std::nullopt;
            const std::string k = o;
            const bool b = k == ">=" ? *l >= *r : k == "<=" ? *l <= *r : k == "==" ? *l == *r : k == "!=" ? *l != *r : k == ">" ? *l > *r : *l < *r;
            return b ? 1.0 : 0.0;
        }
        return l;
    }
    std::optional<double> sum() {
        std::optional<double> l = primary();
        while (l && (op("+") || op("-"))) {
            const bool plus = op("+");
            ++at_;
            const std::optional<double> r = primary();
            if (!r) return std::nullopt;
            l = plus ? *l + *r : *l - *r;
        }
        return l;
    }
    /// Skips the tokens of one call argument (balanced parentheses, to the next top-level comma or the closing parenthesis).
    void skip_argument() {
        int depth = 0;
        while (peek().kind != Token::End) {
            if (op("(")) ++depth;
            else if (op(")")) {
                if (depth == 0) return;
                --depth;
            } else if (op(",") && depth == 0) return;
            ++at_;
        }
    }
    std::optional<double> primary() {
        const Token tk = peek();
        if (tk.kind == Token::Number) {
            ++at_;
            return tk.value;
        }
        if (op("(")) {
            ++at_;
            const std::optional<double> v = or_();
            if (!v || !op(")")) return std::nullopt;
            ++at_;
            return v;
        }
        if (op("-")) {
            ++at_;
            const std::optional<double> v = primary();
            return v ? std::optional<double>(-*v) : std::nullopt;
        }
        if (op("!")) {
            ++at_;
            const std::optional<double> v = primary();
            return v ? std::optional<double>(*v == 0 ? 1.0 : 0.0) : std::nullopt;
        }
        if (tk.kind != Token::Name) return std::nullopt;
        ++at_;
        if (tk.text == "true") return 1.0;
        if (tk.text == "false") return 0.0;
        if (!op("(")) return std::nullopt;   // a bare name (an enum, a variable) is no value of these shapes
        ++at_;
        if (tk.text == "GetPlayerProfessionCertificationLevel") {   // (the player, "Profession")
            skip_argument();
            if (!op(",")) return std::nullopt;
            ++at_;
            if (peek().kind != Token::String) return std::nullopt;
            const std::string profession = peek().text;
            ++at_;
            if (!op(")")) return std::nullopt;
            ++at_;
            return f_.certification ? std::optional<double>(f_.certification(profession)) : std::nullopt;
        }
        if (tk.text == "GetEquipmentCertificationTier") {   // (the actor, EEquipSlot::Slot)
            skip_argument();
            if (!op(",")) return std::nullopt;
            ++at_;
            if (peek().kind != Token::Name) return std::nullopt;
            std::string slot = peek().text;
            const size_t colons = slot.rfind("::");
            if (colons != std::string::npos) slot = slot.substr(colons + 2);
            ++at_;
            if (!op(")")) return std::nullopt;
            ++at_;
            return f_.equipment_tier ? std::optional<double>(f_.equipment_tier(slot)) : std::nullopt;
        }
        if (tk.text == "Player_HasObjective") {   // the world events' fish: no objective is tracked
            skip_argument();
            while (op(",")) {
                ++at_;
                skip_argument();
            }
            if (!op(")")) return std::nullopt;
            ++at_;
            return 0.0;
        }
        if (tk.text == "GetPlayerGUID" || tk.text == "GetInstigator" || tk.text == "GetPlayer") {   // only ever an argument of the two functions; as a value it is none
            return std::nullopt;
        }
        ok_ = false;   // any other function: the text is not one of these shapes
        return std::nullopt;
    }

    const std::vector<Token>& t_;
    const Facts&              f_;
    size_t                    at_ = 0;
    bool                      ok_ = true;
};

}  // namespace

std::optional<double> evaluate_predicate(std::string_view text, const Facts& facts) {
    std::vector<Token> tokens;
    if (!lex(text, tokens)) return std::nullopt;
    return Parser(tokens, facts).run();
}

}  // namespace aoc::worldd::fishing
