// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Buffers.Binary;

namespace NpcAbilities;

/// <summary>
/// Reads one translation track out of an animation that the game stores with the Animation Compression Library (ACL, by Nicholas Frechette and
/// contributors, MIT license, github.com/nfrechette/acl): the layout of its version 2.1, which the client reads back with the same library.  Only
/// what a root bone's travel needs is read: the stream's headers, the kind of the track's translation sub-track (default, constant or animated) and
/// the animated samples.  A stream that uses something else (another version, a database, stripped keyframes) is not read, and the caller says so.
///
/// The layout, little endian, offsets from the start of the stream (`h` is the transform header at 32; the offsets inside it count from `h`):
///   0   size u32, hash u32
///   8   tag u32 (0xAC11AC11), version u16 (10 = 2.1.0), algorithm u8 (0 = uniformly sampled), track type u8 (12 = transforms), track count u32,
///       sample count u32, sample rate f32, flags u32: bit 0 has scale, bit 3 translations quantized (else raw floats), bits 4-7 rotation format
///       (3 = quantized), bit 8 has database, bit 10 has stripped keyframes, bit 30 wraps
///   32  h: segment count, animated sub-track count (the rotations padded to a multiple of 4, then translations, then scales), animated rotation /
///       translation / scale counts, constant rotation / translation / scale counts, database offset, segment headers offset, sub-track types offset,
///       constant data offset, clip range data offset (13 u32)
///   h+52  the first sample of every segment (u32 each, only when there are several segments)
/// The sub-track types are 2 bits a track, 16 tracks to a u32, the first track in the top bits: first all rotations, then all translations, then the
/// scales (when there are any); 0 = default, 1 = constant, 2 = animated.  A segment header (16 bytes) has the bits of a whole animated pose, of its
/// rotations and of its translations, and the offset of the segment's data.  A segment's data is one byte a quantized animated sub-track with its bits
/// a component (rotations first, padded to 4, then translations), then (several segments only) 6 bytes a quantized sub-track of segment range (3 min,
/// 3 extent, a byte each), then the samples: one pose after another, each pose its animated rotations then its animated translations, each component
/// packed MSB first with no padding.  The clip range data has 24 bytes a quantized animated sub-track: 3 min and 3 extent floats (rotations first).
/// A component with 0 bits is constant in the segment (its 16 bit value is in the segment range data), with 31 bits it is a raw float; any other
/// width is a fraction of 2^bits - 1 that the segment range and then the clip range scale into place.
/// </summary>
static class Acl
{
    const int TransformHeader = 32, TransformHeaderSize = 52, SegmentHeaderSize = 16;
    const int RawBits = 31;   // the bit count that marks a raw float (version 2.1.0)

    static uint U32(byte[] s, int at) => BinaryPrimitives.ReadUInt32LittleEndian(s.AsSpan(at, 4));
    static ushort U16(byte[] s, int at) => BinaryPrimitives.ReadUInt16LittleEndian(s.AsSpan(at, 2));
    static float F32(byte[] s, int at) => BinaryPrimitives.ReadSingleLittleEndian(s.AsSpan(at, 4));

    /// <summary>`count` (up to 32) bits from `bit` on, the first bit being the top bit of the first byte; bytes past the end read as zero.</summary>
    static uint Bits(byte[] s, int baseAt, long bit, int count)
    {
        long at = baseAt + (bit >> 3);
        int shift = (int)(bit & 7);
        ulong word = 0;
        for (int i = 0; i < 5; i++) word = (word << 8) | (at + i < s.Length ? s[at + i] : 0u);
        return (uint)((word >> (40 - shift - count)) & ((1UL << count) - 1));
    }

    /// <summary>
    /// The translation of track `track` at every sample of the stream (the stream's units), `sampleRate` samples a second.  Null with an empty `why` when the
    /// track's translation never moves (a default or constant sub-track); null with a `why` when the stream cannot be read.
    /// </summary>
    public static (float X, float Y, float Z)[]? ReadTranslation(byte[] s, int track, out float sampleRate, out string why)
    {
        sampleRate = 0f; why = "";
        try { return Read(s, track, out sampleRate, out why); }
        catch (Exception ex) when (ex is IndexOutOfRangeException || ex is ArgumentOutOfRangeException || ex is OverflowException)
        {
            why = "the animation stream is shorter than its headers say";
            return null;
        }
    }

    static (float X, float Y, float Z)[]? Read(byte[] s, int track, out float sampleRate, out string why)
    {
        sampleRate = 0f; why = "";
        if (s == null || s.Length < TransformHeader + TransformHeaderSize) { why = "the animation stream is too short"; return null; }
        if (U32(s, 0) != (uint)s.Length) { why = "the animation stream's size differs from its header"; return null; }
        if (U32(s, 8) != 0xAC11AC11u) { why = "the animation stream is not a track list"; return null; }
        int version = U16(s, 12);
        if (version != 10) { why = "the animation stream has version " + version + ", which this tool does not read"; return null; }
        if (s[14] != 0 || s[15] != 12) { why = "the animation stream is not uniformly sampled transforms"; return null; }
        uint trackCount = U32(s, 16), sampleCount = U32(s, 20);
        float rate = F32(s, 24);
        uint flags = U32(s, 28);
        if (((flags >> 8) & 1) != 0) { why = "the animation stream keeps frames in a database"; return null; }
        if (((flags >> 10) & 1) != 0) { why = "the animation stream has stripped keyframes"; return null; }
        if (((flags >> 30) & 1) != 0) { why = "the animation stream wraps"; return null; }
        if (track < 0 || track >= trackCount) { why = "the animation stream has no track " + track; return null; }
        if (sampleCount == 0 || !(rate > 0f)) { why = "the animation stream has no samples"; return null; }
        bool translationsQuantized = ((flags >> 3) & 1) != 0;
        bool rotationsQuantized = ((flags >> 4) & 15) == 3;

        int h = TransformHeader;
        uint segmentCount = U32(s, h), variableCount = U32(s, h + 4), rotationCount = U32(s, h + 8), translationCount = U32(s, h + 12);
        int segmentHeaders = h + (int)U32(s, h + 36), subTrackTypes = h + (int)U32(s, h + 40), clipRanges = h + (int)U32(s, h + 48);
        if (segmentCount == 0) { why = "the animation stream has no segments"; return null; }

        // the kind of every translation sub-track before this one and of this one: 0 default, 1 constant, 2 animated
        int entries = (int)((trackCount + 15) / 16);
        uint TypeOf(int kind, int t) => (U32(s, subTrackTypes + 4 * (kind * entries + t / 16)) >> ((15 - t % 16) * 2)) & 3;
        uint mine = TypeOf(1, track);
        if (mine == 0 || mine == 1) return null;   // the root keeps its pose
        if (mine != 2) { why = "the animation stream has a sub-track of a kind this tool does not know"; return null; }
        int index = 0;   // this sub-track's place among the animated translations
        for (int t = 0; t < track; t++) if (TypeOf(1, t) == 2) index++;
        if (index >= translationCount) { why = "the animation stream counts fewer animated translations than it has"; return null; }

        // the first sample of every segment
        var starts = new long[segmentCount];
        if (segmentCount > 1)
            for (int i = 0; i < segmentCount; i++) starts[i] = U32(s, h + TransformHeaderSize + 4 * i);
        if (starts[0] != 0) { why = "the animation stream's first segment does not start at the first sample"; return null; }
        for (int i = 1; i < segmentCount; i++)
            if (starts[i] <= starts[i - 1] || starts[i] >= sampleCount) { why = "the animation stream's segments are out of order"; return null; }

        int rotationPadded = ((int)rotationCount + 3) & ~3;
        var result = new (float X, float Y, float Z)[sampleCount];
        int segment = 0;
        // the segment's data: the bits of each sub-track, the segment ranges, the samples
        int formatAt = 0, rangeAt = 0, animatedAt = 0, bitsBefore = 0, poseBits = 0, rotationBits = 0;
        int bitsOfMine = 0;
        void Enter(int i)
        {
            int header = segmentHeaders + SegmentHeaderSize * i;
            poseBits = (int)U32(s, header);
            rotationBits = (int)U32(s, header + 4);
            int data = h + (int)U32(s, header + 12);
            int formats = data;
            int ranges = (formats + (int)variableCount + 1) & ~1;
            animatedAt = (ranges + (segmentCount > 1 ? 6 * (int)variableCount : 0) + 3) & ~3;
            formatAt = formats + (rotationsQuantized ? rotationPadded : 0);
            rangeAt = ranges + (rotationsQuantized ? 6 * rotationPadded : 0) + 6 * index;
            bitsBefore = 0;
            bitsOfMine = 0;
            if (translationsQuantized)
            {
                for (int k = 0; k < index; k++)
                {
                    int b = s[formatAt + k];
                    bitsBefore += 3 * (b == RawBits ? 32 : b);
                }
                bitsOfMine = s[formatAt + index];
            }
            else
            {
                bitsBefore = 96 * index;
                bitsOfMine = RawBits;
            }
        }
        Enter(0);
        int clip = clipRanges + (rotationsQuantized ? 24 * (int)rotationCount : 0) + 24 * index;

        Span<float> v = stackalloc float[3];
        for (int f = 0; f < sampleCount; f++)
        {
            while (segment + 1 < segmentCount && f >= starts[segment + 1]) { segment++; Enter(segment); }
            long bit = (long)(f - starts[segment]) * poseBits + rotationBits + bitsBefore;
            if (bitsOfMine == RawBits)
            {
                for (int c = 0; c < 3; c++) v[c] = BitConverter.UInt32BitsToSingle(Bits(s, animatedAt, bit + 32 * c, 32));
                result[f] = (v[0], v[1], v[2]);
                continue;
            }
            if (bitsOfMine == 0)
            {
                if (segmentCount == 1) { why = "the animation stream has a constant component and no segment ranges"; return null; }
                for (int c = 0; c < 3; c++) v[c] = U16(s, rangeAt + 2 * c) * (1f / 65535f);
            }
            else
            {
                if (bitsOfMine > 23) { why = "the animation stream packs a component in more than 23 bits"; return null; }
                float scale = 1f / ((1 << bitsOfMine) - 1);
                for (int c = 0; c < 3; c++) v[c] = Bits(s, animatedAt, bit + (long)bitsOfMine * c, bitsOfMine) * scale;
                if (segmentCount > 1)
                    for (int c = 0; c < 3; c++) v[c] = v[c] * (s[rangeAt + 3 + c] * (1f / 255f)) + s[rangeAt + c] * (1f / 255f);
            }
            for (int c = 0; c < 3; c++) v[c] = v[c] * F32(s, clip + 12 + 4 * c) + F32(s, clip + 4 * c);
            result[f] = (v[0], v[1], v[2]);
        }
        sampleRate = rate;
        return result;
    }
}
