# Self-Registering Replication Struct Codecs

This directory holds self-registering `NetSerialize` struct codecs for `libaocnet` (`aoc::protocol::repl`).

New replicated structs can be added here with **zero edits** to `protocol/repl/value.h`, `protocol/repl/codecs.h`, `protocol/repl/codecs.cpp`, or `libs/aocnet/CMakeLists.txt`.

## File Convention

Add one pair of files for a feature module:

- `libs/aocnet/protocol/repl/codecs/<feature>_codec.h`
  - Defines the C++ value struct(s) carried inside `aoc::protocol::repl::Value`.
  - Every value struct must be equality-comparable (`const T& == const T& -> bool`). For `float` and `double` fields, compare by bit pattern using `same_bits()` from `protocol/repl/value.h` so `RepShadow` comparisons match wire bits.
- `libs/aocnet/protocol/repl/codecs/<feature>_codec.cpp`
  - Implements the wire reader, writer, bounds validator (`accepts`), and optional `zero` constructor.
  - Defines `void register_<feature>_codecs()` inside `namespace aoc::protocol::repl`, calling `register_struct_codec("FStructName", StructCodecOps{...})` for each struct in the module.

The `<feature>` stem must be a valid C++ identifier matching `^[A-Za-z_][A-Za-z0-9_]*$`.

## How Auto-Registration Works

1. `libs/aocnet/CMakeLists.txt` discovers `protocol/repl/codecs/*_codec.cpp` and `*_codec.h` via `file(GLOB CONFIGURE_DEPENDS ...)`, sorts them deterministically, validates each `<feature>` stem, adds them to `aocnet`, and generates `protocol/repl/registered_feature_codecs.inc` in the build tree.
2. `protocol/repl/struct_codec_registry.cpp` invokes `register_all_feature_codecs()` once via `std::call_once` on the first registry lookup.
3. Because `codecs.cpp` directly references `find_registered_struct_codec`, which directly references `register_all_feature_codecs`, static-library linking retains every `<feature>_codec.obj` without relying on static initializers or whole-archive linker flags.

## Minimal Example

`libs/aocnet/protocol/repl/codecs/example_codec.h`:

```cpp
#pragma once

#include "protocol/repl/value.h"

#include <cstdint>
#include <string>

namespace aoc { namespace protocol { namespace repl {

struct ExamplePayload {
    int32_t     id = 0;
    float       weight = 0.0f;
    std::string label;

    bool operator==(const ExamplePayload& o) const {
        return id == o.id && same_bits(weight, o.weight) && label == o.label;
    }
};

}}}  // namespace aoc::protocol::repl
```

`libs/aocnet/protocol/repl/codecs/example_codec.cpp`:

```cpp
#include "protocol/repl/codecs/example_codec.h"

#include "protocol/repl/struct_codec_registry.h"

#include <bit>

namespace aoc { namespace protocol { namespace repl {

namespace {

bool accepts_example(const Value& v) {
    const auto* p = v.get_if<ExamplePayload>();
    return p != nullptr && fstring_fits(p->label);
}

std::optional<Value> zero_example() {
    return Value(ExamplePayload{});
}

std::optional<Value> read_example(wire::PacketReader& r, const CodecOptions&) {
    ExamplePayload p;
    p.id = static_cast<int32_t>(r.read_uint32());
    p.weight = std::bit_cast<float>(r.read_uint32());
    auto label = read_fstring(r);
    if (!label || overran(r)) return std::nullopt;
    p.label = std::move(*label);
    return Value(std::move(p));
}

bool write_example(const Value& v, emit::BunchWriter& out, const CodecOptions&) {
    const auto* p = v.get_if<ExamplePayload>();
    if (!p) return false;
    out.write_int32(p->id);
    out.write_uint32(std::bit_cast<uint32_t>(p->weight));
    return write_fstring(out, p->label);
}

}  // namespace

void register_example_codecs() {
    register_struct_codec("FExamplePayload", StructCodecOps{
        .accepts = &accepts_example,
        .zero    = &zero_example,
        .read    = &read_example,
        .write   = &write_example,
    });
}

}}}  // namespace aoc::protocol::repl
```

## Guidelines

- `Value` stores custom structs as immutable shared values; `v.get_if<MyStruct>()` always returns `const MyStruct*`, preserving `RepShadow` immutability.
- `accepts`, `read`, and `write` must all be non-null (`register_struct_codec` throws `std::invalid_argument` otherwise).
- Leave `zero` as `nullptr` unless the struct's wire zero form has been verified.
- Check `overran(r)` and validate all wire counts/lengths in `read`; `read_struct` also enforces `overran(r)` as a fail-closed backstop.
- Never shadow a built-in struct name or register the same struct name twice (`register_struct_codec` throws `std::logic_error`).
