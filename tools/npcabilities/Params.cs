// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;
using CUE4Parse.FileProvider;
using CUE4Parse.FileProvider.Vfs;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Assets.Readers;
using CUE4Parse.UE4.IO.Objects;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Objects.StructUtils;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.UE4.Readers;
using CUE4Parse.UE4.Versions;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// The parameters of a creature's state tree.  The tree declares them with a default each, and a creature Blueprint may override some in the property bag of its
/// state tree reference.  The file reader of the pinned parser skips the values of a property bag, so a package is read here through an archive that notes the
/// block it skips, and the block is decoded by the format the game's files use for it (an unversioned header, a zero mask, then the values that are not zero).
/// </summary>
static class Parameters
{
    /// <summary>A byte archive that notes the forward jumps the parsers make (a skipped block).</summary>
    sealed class SkipArchive : FByteArchive
    {
        public readonly byte[] Data;
        public readonly List<(long From, long To)> Skips;
        long _pos;
        public SkipArchive(string name, byte[] data, VersionContainer versions, List<(long From, long To)> skips) : base(name, data, versions) { Data = data; Skips = skips; }
        public override long Position
        {
            get => _pos;
            set { if (value - _pos >= 8 && value <= Data.Length) Skips.Add((_pos, value)); _pos = value; }
        }
        public override object Clone() => new SkipArchive(Name, Data, Versions, Skips) { _pos = _pos };
    }

    sealed class Loaded
    {
        public IoPackage Package = null!;
        public byte[] Data = Array.Empty<byte>();
        public List<(long From, long To)> Skips = new();
    }

    /// <summary>One parameter: its id, name and value (a bool, a number or a string); the value is null when the decoder could not reach it.</summary>
    public sealed class Param
    {
        public FGuid Id;
        public string Name = "";
        public object? Value;
    }

    /// <summary>The parameters a package declares in the bag of its first state tree reference (`StateTreeComponent`) or of its state tree (`StateTree`), with the ids it overrides.</summary>
    public sealed class Bag
    {
        public List<Param> Params = new();
        public HashSet<FGuid> Overrides = new();
    }

    static Loaded? Load(Game g, string packagePath)
    {
        try
        {
            string key = packagePath.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) ? packagePath : packagePath + ".uasset";
            if (!g.Provider.Files.TryGetValue(key, out var file) || file is not FIoStoreEntry entry) return null;
            var skips = new List<(long From, long To)>();
            var ar = new SkipArchive(file.Path, file.Read(), g.Provider.Versions, skips);
            var pkg = new IoPackage(ar, entry.IoStoreReader.ContainerHeader, (FArchive?)null, (FArchive?)null, (IVfsFileProvider)g.Provider);
            return new Loaded { Package = pkg, Data = ar.Data, Skips = skips };
        }
        catch { return null; }
    }

    /// <summary>The bag of the package's state tree reference (a Blueprint) or of its state tree asset; null when the package has none or cannot be read.</summary>
    public static Bag? Read(Game g, string packagePath, bool tree)
    {
        var loaded = Load(g, packagePath);
        if (loaded == null) return null;
        try
        {
            for (int i = 0; i < loaded.Package.ExportsLazy.Length; i++)
            {
                var export = loaded.Package.ExportsLazy[i]?.Value;
                if (export == null) continue;
                List<FPropertyTag> props;
                if (tree)
                {
                    if (export.ExportType != "StateTree") continue;
                    props = export.Properties;
                }
                else
                {
                    if (export.ExportType != "StateTreeComponent" || !export.TryGetValue(out FStructFallback reference, "StateTreeRef") || reference == null) continue;
                    props = reference.Properties;
                }
                var bagProp = props.FirstOrDefault(p => p.Name.Text == "Parameters");
                if (bagProp?.Tag?.GenericValue is not FScriptStruct ss || ss.StructType is not FInstancedPropertyBag bag || bag.PropertyDescs == null) continue;
                var result = new Bag();
                if (props.FirstOrDefault(p => p.Name.Text == "PropertyOverrides")?.Tag?.GenericValue is UScriptArray overrides)
                    foreach (var o in overrides.Properties) if (o.GenericValue is FScriptStruct ss2 && ss2.StructType is FGuid id) result.Overrides.Add(id);
                var skip = loaded.Skips.LastOrDefault(s => s.To - s.From == bag.SerialSize);
                var values = skip.To == 0 ? null : Decode(g, loaded, bag.PropertyDescs, (int)skip.From, bag.SerialSize);
                for (int k = 0; k < bag.PropertyDescs.Length; k++)
                    result.Params.Add(new Param { Id = bag.PropertyDescs[k].ID, Name = bag.PropertyDescs[k].Name.Text, Value = values != null && k < values.Length ? values[k] : null });
                return result;
            }
        }
        catch { }
        return null;
    }

    /// <summary>
    /// The value of each property of the bag, in order, null for one the decoder did not reach.  The block is the unversioned serialization of the struct the
    /// bag describes: 16-bit fragments (skip count, has-zeroes, last, value count) up to the last one, a zero mask with a bit per value of a fragment that has zeroes
    /// (a set bit: the value is zero or false, and not stored), then every stored value in order.  A true bool is stored as one byte.
    /// </summary>
    static object?[]? Decode(Game g, Loaded loaded, FPropertyBagPropertyDesc[] descs, int start, int length)
    {
        var data = loaded.Data;
        var result = new object?[descs.Length];
        int pos = start, end = start + length;
        var fragments = new List<(int Skip, bool Zeroes, bool Last, int Values)>();
        for (;;)
        {
            if (pos + 2 > end) return null;
            ushort f = BitConverter.ToUInt16(data, pos);
            pos += 2;
            var fragment = (Skip: f & 0x7F, Zeroes: (f & 0x80) != 0, Last: (f & 0x100) != 0, Values: f >> 9);
            fragments.Add(fragment);
            if (fragment.Last) break;
        }
        int maskBits = fragments.Where(x => x.Zeroes).Sum(x => x.Values);
        byte[] mask = Array.Empty<byte>();
        if (maskBits > 0)
        {
            int bytes = maskBits <= 8 ? 1 : maskBits <= 16 ? 2 : (maskBits + 31) / 32 * 4;
            if (pos + bytes > end) return null;
            mask = new byte[bytes];
            Array.Copy(data, pos, mask, 0, bytes);
            pos += bytes;
        }
        int maskIndex = 0, index = 0;
        bool lost = false;
        foreach (var fragment in fragments)
        {
            index += fragment.Skip;
            for (int v = 0; v < fragment.Values; v++, index++)
            {
                if (index >= descs.Length) return result;
                bool zero = false;
                if (fragment.Zeroes)
                {
                    zero = (mask[maskIndex >> 3] & (1 << (maskIndex & 7))) != 0;
                    maskIndex++;
                }
                if (lost) continue;
                var desc = descs[index];
                try
                {
                    if (desc.ContainerTypes.Types is { Length: > 0 } || desc.ContainerType != EPropertyBagContainerType.None)
                    {
                        // an array: its count, then its elements
                        if (zero) { result[index] = "(empty array)"; continue; }
                        int count = BitConverter.ToInt32(data, pos);
                        pos += 4;
                        int each = ElementSize(desc.ValueType);
                        if (each == 0 && count > 0) { lost = true; continue; }
                        pos += each * count;
                        result[index] = "(array)";
                        continue;
                    }
                    switch (desc.ValueType)
                    {
                        case EPropertyBagPropertyType.Bool:
                            if (zero) result[index] = false;
                            else { result[index] = data[pos] != 0; pos += 1; }
                            break;
                        case EPropertyBagPropertyType.Byte:
                        case EPropertyBagPropertyType.Enum:
                            if (zero) result[index] = 0.0; else { result[index] = (double)data[pos]; pos += 1; }
                            break;
                        case EPropertyBagPropertyType.Int32:
                            if (zero) result[index] = 0.0; else { result[index] = (double)BitConverter.ToInt32(data, pos); pos += 4; }
                            break;
                        case EPropertyBagPropertyType.UInt32:
                            if (zero) result[index] = 0.0; else { result[index] = (double)BitConverter.ToUInt32(data, pos); pos += 4; }
                            break;
                        case EPropertyBagPropertyType.Int64:
                        case EPropertyBagPropertyType.UInt64:
                            if (zero) result[index] = 0.0; else { result[index] = (double)BitConverter.ToInt64(data, pos); pos += 8; }
                            break;
                        case EPropertyBagPropertyType.Float:
                            if (zero) result[index] = 0.0; else { result[index] = (double)BitConverter.ToSingle(data, pos); pos += 4; }
                            break;
                        case EPropertyBagPropertyType.Double:
                            if (zero) result[index] = 0.0; else { result[index] = BitConverter.ToDouble(data, pos); pos += 8; }
                            break;
                        case EPropertyBagPropertyType.Name:
                            if (zero) result[index] = "None"; else { pos += 8; result[index] = "(name)"; }
                            break;
                        case EPropertyBagPropertyType.String:
                            if (zero) { result[index] = ""; break; }
                            int n = BitConverter.ToInt32(data, pos);
                            pos += 4;
                            if (n > 0) { result[index] = Encoding.ASCII.GetString(data, pos, n - 1); pos += n; }
                            else if (n < 0) { result[index] = Encoding.Unicode.GetString(data, pos, (-n - 1) * 2); pos += -n * 2; }
                            else result[index] = "";
                            break;
                        case EPropertyBagPropertyType.Struct:
                            if (zero) { result[index] = "(struct)"; break; }
                            {
                                string structName = desc.ValueTypeObject != null && !desc.ValueTypeObject.IsNull ? desc.ValueTypeObject.Name : "";
                                var ar = new FAssetArchive(new FByteArchive("bag", data, g.Provider.Versions) { Position = pos }, loaded.Package);
                                _ = new FStructFallback(ar, structName);
                                pos = (int)ar.Position;
                                result[index] = "(struct)";
                            }
                            break;
                        default:
                            lost = true;
                            break;
                    }
                }
                catch { lost = true; }
            }
        }
        // a block that does not end where the format says it does was not read right: nothing from it is used
        return lost || pos != end ? null : result;
    }

    static int ElementSize(EPropertyBagPropertyType t) => t switch
    {
        EPropertyBagPropertyType.Name or EPropertyBagPropertyType.Double or EPropertyBagPropertyType.Int64 or EPropertyBagPropertyType.UInt64 => 8,
        EPropertyBagPropertyType.Float or EPropertyBagPropertyType.Int32 or EPropertyBagPropertyType.UInt32 => 4,
        EPropertyBagPropertyType.Byte or EPropertyBagPropertyType.Enum => 1,
        _ => 0
    };

    /// <summary>
    /// The values a creature's tree runs with, by parameter name: the tree's defaults, with the ones the Blueprint's reference overrides (a parameter that is in its bag and
    /// whose id it lists).  A parameter whose value could not be read is left out.
    /// </summary>
    public static Dictionary<string, object> Effective(Bag? tree, Bag? blueprint)
    {
        var result = new Dictionary<string, object>(StringComparer.Ordinal);
        if (tree == null) return result;
        foreach (var p in tree.Params)
        {
            object? value = p.Value;
            var over = blueprint?.Params.FirstOrDefault(b => b.Id == p.Id);
            if (over != null && blueprint!.Overrides.Contains(p.Id)) value = over.Value;
            if (value != null) result[p.Name] = value;
        }
        return result;
    }
}
