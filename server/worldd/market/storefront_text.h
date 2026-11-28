// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  market/storefront_text.h
//
//  The text rules of the Marketplace's search and storefront descriptions, as plain functions over UTF-8 text:
//
//    length          the client counts a text in UTF-16 code units (a search of 1 to 50, the edit box's cut at 50), so the server counts the same: a character outside the first
//                    plane is two.  A text that is not well-formed UTF-8, or that holds a control character, is no text a client sends.
//    matching        a search finds a text inside an item's name, whatever the case of either (ASCII letters; the item names the game shows are ASCII).
//    the bad words   the game's own filter finds each word of its English list inside a text without regard to case, so the word may be anywhere in the text, inside another word
//                    too.  (A text with item links has the links left out of the search; a description is plain text, so the whole of it is searched.)  The list is content.db's
//                    bad_word table; none is written in this source.
//    the default     what a storefront shows when its seller gave it no description.
//  Pure: no database and no session.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aoc::worldd::market {

/// The longest search and the longest description, in UTF-16 code units (the client's own limits).
inline constexpr size_t kTextMax = 50;

/// The length of `text` in UTF-16 code units; nullopt when it is not well-formed UTF-8 (an overlong form, a surrogate, a value past U+10FFFF, a cut sequence).
inline std::optional<size_t> utf16_length(std::string_view text) {
    size_t units = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return std::nullopt;
        }
        if (extra > 0 && i + extra >= text.size()) return std::nullopt;   // the sequence is cut
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0u) != 0x80u) return std::nullopt;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
        units += cp >= 0x10000 ? 2 : 1;
        i += extra + 1;
    }
    return units;
}

/// Whether `text` holds a control character (below U+0020, or the delete character): the line breaks and the tab the client's edit box takes out never reach the server.
inline bool has_control_character(std::string_view text) {
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

/// The ASCII letter in lower case; every other byte as it is.
inline char fold_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = fold_ascii(c);
    return out;
}

/// Whether `needle` is inside `haystack`, whatever the case of the ASCII letters of either; an empty needle is inside every text.
inline bool contains_ignoring_case(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
        size_t k = 0;
        while (k < needle.size() && fold_ascii(haystack[at + k]) == fold_ascii(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

/// `needle_folded` (already in lower case, see folded) inside `haystack`, whatever the case of the haystack: the search's inner loop, which folds its text once.
inline bool contains_folded(std::string_view haystack, std::string_view needle_folded) {
    if (needle_folded.empty()) return true;
    if (needle_folded.size() > haystack.size()) return false;
    for (size_t at = 0; at + needle_folded.size() <= haystack.size(); ++at) {
        size_t k = 0;
        while (k < needle_folded.size() && fold_ascii(haystack[at + k]) == needle_folded[k]) ++k;
        if (k == needle_folded.size()) return true;
    }
    return false;
}

/// The description a storefront shows when its seller gave it none.
inline std::string default_description(const std::string& seller_name) { return seller_name + "'s Storefront"; }

/// The client's English bad-word list, as the game's own filter searches it: a word is found anywhere in a text, whatever the case.
class BadWords {
public:
    BadWords() = default;
    explicit BadWords(std::vector<std::string> words) { set(std::move(words)); }

    /// Replaces the list.  A word is kept in lower case (the search folds the text, not the word again); an empty word is dropped.
    void set(std::vector<std::string> words) {
        folded_.clear();
        folded_.reserve(words.size());
        for (const std::string& w : words) {
            if (!w.empty()) folded_.push_back(folded(w));
        }
    }

    size_t size() const { return folded_.size(); }
    bool empty() const { return folded_.empty(); }

    /// Whether any word of the list is inside `text`.
    bool contains(std::string_view text) const {
        for (const std::string& w : folded_) {
            if (contains_folded(text, w)) return true;
        }
        return false;
    }

private:
    std::vector<std::string> folded_;   // lower case
};

}  // namespace aoc::worldd::market
