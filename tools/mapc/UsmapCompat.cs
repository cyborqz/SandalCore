// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;

namespace Mapc;

/// <summary>
/// CUE4Parse reads usmap versions up to 3; the game's aoc.usmap is version 4 (ExplicitEnumValues: every enum entry is an int64 value, then a name index).
/// DowngradeToV3 rewrites the file as version 3: an enum entry is then only a name, put at the index of its value (a gap gets the placeholder name
/// "__gap"), so that "value -> name" still holds for every enum whose values are small and distinct; any other enum keeps its names in order.
/// Layout (uncompressed): u16 magic 0x30C4, u8 version, i32 hasVersioning (0), u8 compression (0), u32 compressedSize, u32 decompressedSize, then the body:
/// u32 name count and the names (u16 length, bytes), u32 enum count and the enums (u32 name, u16 count, entries), then the structs, copied as they are.
/// </summary>
public static class UsmapCompat
{
    const string GapName = "__gap";

    public static byte[] DowngradeToV3(byte[] src)
    {
        if (src.Length < 16 || BitConverter.ToUInt16(src, 0) != 0x30C4) throw new InvalidDataException("not a usmap file");
        if (src[2] != 4) return src;                                              // another version: as it is
        int o = 3;
        if (BitConverter.ToInt32(src, o) != 0) throw new NotSupportedException("a usmap with package versioning");
        o += 4;
        if (src[o++] != 0) throw new NotSupportedException("a compressed usmap");
        o += 8;                                                                    // the two sizes: rewritten below
        int bodyStart = o;

        // the names: copied, plus the placeholder
        uint nameCount = BitConverter.ToUInt32(src, o); o += 4;
        for (uint i = 0; i < nameCount; i++) o += 2 + BitConverter.ToUInt16(src, o);
        var body = new MemoryStream(src.Length);
        var w = new BinaryWriter(body);
        w.Write(nameCount + 1);
        w.Write(src, bodyStart + 4, o - bodyStart - 4);
        var gap = Encoding.Latin1.GetBytes(GapName);
        w.Write((ushort)gap.Length); w.Write(gap);
        uint gapIndex = nameCount;

        // the enums: value and name become a name at the value's place
        uint enumCount = BitConverter.ToUInt32(src, o); o += 4;
        w.Write(enumCount);
        for (uint e = 0; e < enumCount; e++)
        {
            uint enumName = BitConverter.ToUInt32(src, o); o += 4;
            int count = BitConverter.ToUInt16(src, o); o += 2;
            var values = new long[count];
            var names = new uint[count];
            for (int j = 0; j < count; j++)
            {
                values[j] = BitConverter.ToInt64(src, o); o += 8;
                names[j] = BitConverter.ToUInt32(src, o); o += 4;
            }
            long max = count == 0 ? -1 : values.Max();
            bool dense = values.All(v => v >= 0) && values.Distinct().Count() == count && max < 4096 && max + 1 >= count;
            w.Write(enumName);
            if (dense)
            {
                var slots = new uint[max + 1];
                Array.Fill(slots, gapIndex);
                for (int j = 0; j < count; j++) slots[values[j]] = names[j];
                w.Write((ushort)slots.Length);
                foreach (var s in slots) w.Write(s);
            }
            else
            {
                w.Write((ushort)count);
                foreach (var s in names) w.Write(s);
            }
        }
        w.Write(src, o, src.Length - o);                                           // the structs
        w.Flush();
// ~ C-O-A - C_Y_B ~

        var outMs = new MemoryStream(body.Capacity + 16);
        var ow = new BinaryWriter(outMs);
        ow.Write((ushort)0x30C4); ow.Write((byte)3);
        ow.Write(0); ow.Write((byte)0);
        ow.Write((uint)body.Length); ow.Write((uint)body.Length);
        ow.Write(body.GetBuffer(), 0, (int)body.Length);
        ow.Flush();
        return outMs.ToArray();
    }
}
