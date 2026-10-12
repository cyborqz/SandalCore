// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  protocol/repl/value.cpp
//
//  Value's kind and equality (the parts that need ElementState complete).
// ============================================================================
#include "protocol/repl/value.h"

namespace aoc { namespace protocol { namespace repl {

bool ArrayValue::operator==(const ArrayValue& o) const { return elements == o.elements; }

ValueKind Value::kind() const {
    if (std::holds_alternative<InlineObject>(v_)) return ValueKind::Object;
    switch (v_.index()) {
        case 0:  return ValueKind::None;
        case 1:  return ValueKind::Bool;
        case 2:  return ValueKind::Int;
        case 3:  return ValueKind::UInt;
        case 4:  return ValueKind::Float;
        case 5:  return ValueKind::Double;
        case 6:  return ValueKind::String;
        case 7:  return ValueKind::Name;
        case 8:  return ValueKind::Text;
        case 9:  return ValueKind::Object;
        case 10: return ValueKind::SoftObject;
        case 11: return ValueKind::Array;
        default: return ValueKind::Struct;   // Vec3d ... CharacterCustomization
    }
}

bool Value::operator==(const Value& o) const {
    if (v_.index() != o.v_.index()) return false;
    if (const float* a = std::get_if<float>(&v_)) return same_bits(*a, std::get<float>(o.v_));
    if (const double* a = std::get_if<double>(&v_)) return same_bits(*a, std::get<double>(o.v_));
    return v_ == o.v_;
}

}}}  // namespace aoc::protocol::repl
