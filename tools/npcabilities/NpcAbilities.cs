// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Animation;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Readers;
using CUE4Parse.UE4.Objects.StateTree;
using CUE4Parse.UE4.Objects.StructUtils;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.Utils;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// What a creature does when it attacks, read from the game's containers: which abilities a creature Blueprint uses and, for every animation montage
/// beside it, how long the montage plays and when its hit notifies land.
///
/// A creature Blueprint does not list its abilities itself: its StateTree does.  Every "use ability" task of the tree names one ability record, with the
/// time the creature waits before it uses that ability again, and sits in a state that the creature enters only when the state's conditions hold.  A
/// Blueprint that sets no tree of its own plays its parent's.  `npc_abilities.tsv` has one line per (Blueprint, task):
///   archetype  ability  task  cooldown  stop_movement  order  min_distance  max_distance  max_angle  gates  unknown  fight  conds  why  starts  rotate
/// `archetype` is the Blueprint's default object as npc_motion.tsv names it, `ability` the ability record's guid, `cooldown` seconds and `stop_movement`
/// whether the creature stands still while the ability plays.  `order` is the task's state in the tree (the creature tries the states in this order and
/// uses the first whose conditions hold).  The conditions of the states above the task, from the root down: `min_distance` the target must be farther than
/// that, `max_distance` and `max_angle` it must be nearer and within that angle of the creature's facing (-1: none), `gates` the cooldown names that must
/// be over (';'), and `unknown` 1 when a condition is of a kind this tool does not say: the task then has conditions the table does not say, and `why` names them.
/// `fight` is 1 for a task in the state the creature fights in (a task of its idle or opening states is not an attack of the fight).  `conds` is every other
/// condition, said as atoms (see Conditions.cs): one group per state, ';'-free, groups joined by '|'.  A task below a state the designers switched off (a debug or
/// retired branch) is left out; `why` is "off" for a task a tree parameter switches off.  `starts` are the cooldowns the creature starts when it takes the task, besides its own
/// (`cooldown` under the name `task`): the tasks of the states above it may start cooldowns of their own ("name=seconds", joined by ';').  `rotate` is "speed:duration" of the
/// "Rotate To" task (turn towards the target) that runs with the ability, in its state or in a state above it: the creature keeps turning to its target at `speed` degrees a second
/// (-1: the task names none, the creature's own rate) for `duration` seconds (0: until the ability is over); empty when no such task runs with the ability.
///
/// The settings a tree takes from its parameters (the distance of a bite, whether the creature may pounce) are read from the Blueprint's reference to the tree and the
/// tree's own defaults (Params.cs).
///
/// The montages are the animation assets of the Blueprint's folder.  `npc_montages.tsv` has one line per montage:
///   montage  guid  package  package_guid  length  rate_scale  sections  hits  root_motion  root_unreadable
/// `montage` is the asset's object path as an ability record names it ("/Game/.../Name.Name"), `length` the montage's length in seconds at a play rate of 1
/// (the time a montage runs is length / rate_scale / the play rate), `sections` "name:start:next" separated by ';' and `hits` "time:key" separated by
/// ';' for every hit notify of the montage, in time order.  `key` picks the hit record of the ability the notify fires (the ability's hits are keyed the
/// same way); a montage with one hit and no key uses key 0.  `root_motion` is how far the montage carries the creature (the animation's root bone
/// moves, and the creature moves with it): "time:x:y:z" separated by ';', centimetres from where the creature stood when the montage began, x ahead and y
/// to the right of where it faces, in montage seconds at a play rate of 1; empty for a montage that stays in place.  `root_unreadable` is 1 for a montage whose
/// animation moves the creature in a form this tool cannot read: its `root_motion` is empty then, and it does not mean "stays in place".  `guid` and `package_guid` are the ids
/// the network layer gives the montage object and its package: a static object's id is the 64-bit CityHash of "Class ObjectPath" with the lowest bit set.
///
/// Both files are written in sorted order with round-trip floats, so a second read of the same game files is byte for byte the same.
/// </summary>
public static partial class Reader
{
    public const string AbilitiesFile = "npc_abilities.tsv";
    public const string MontagesFile = "npc_montages.tsv";
    public const string AbilitiesHeader = "archetype\tability\ttask\tcooldown\tstop_movement\torder\tmin_distance\tmax_distance\tmax_angle\tgates\tunknown\tfight\tconds\twhy\tstarts\trotate";
    public const string MontagesHeader = "montage\tguid\tpackage\tpackage_guid\tlength\trate_scale\tsections\thits\troot_motion\troot_unreadable";

    public sealed class AbilityRow
    {
        public string Archetype = "", Task = "", Gates = "", Conds = "", Why = "", Starts = "", Rotate = "";
        public long Ability;
        public float Cooldown;
        public bool StopMovement, Unknown, Fight;
        public int Order;
        public float MinDistance = -1f, MaxDistance = -1f, MaxAngle = -1f;
    }

    public sealed class MontageRow
    {
        public string Montage = "", Package = "";
        public ulong Guid, PackageGuid;
        public float Length, RateScale = 1f;
        public bool RootUnreadable;
        public List<(string Name, float Start, string Next)> Sections = new();
        public List<(float Time, int Key)> Hits = new();
        public List<(float Time, float X, float Y, float Z)> RootMotion = new();
    }

    /// <summary>The id the network layer gives a static object: CityHash64 of "Class ObjectPath" (UTF-8), lowest bit set.</summary>
    public static ulong StaticId(string className, string path) => CityHash.CityHash64(Encoding.UTF8.GetBytes(className + " " + path)) | 1UL;

    static string PackagePathOf(string containerPath) => "/Game/" + containerPath.Substring("AOC/Content/".Length, containerPath.Length - "AOC/Content/".Length - ".uasset".Length);

    /// <summary>The container path of a package named "/Game/...".</summary>
    static string ContainerPathOf(string packageName) => "AOC/Content/" + packageName.Substring("/Game/".Length) + ".uasset";

    /// <summary>The class chain of a Blueprint's default object, nearest first, with each parent's default object.</summary>
    static List<UObject> CdoChain(UObject cdo)
    {
        var list = new List<UObject>();
        UObject? o = cdo;
        for (int i = 0; o != null && i < 12; i++)
        {
            list.Add(o);
            o = o.Template?.Object?.Value;
        }
        return list;
    }

    static string ClassName(UObject cdo) => cdo.Name.StartsWith("Default__", StringComparison.Ordinal) ? cdo.Name.Substring(9) : cdo.Name;

    static IEnumerable<FStructFallback> StructsOf(UObject o, string name)
    {
        var prop = o.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is UScriptArray arr)
            foreach (var item in arr.Properties)
                if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf) yield return sf;
    }

    static long GuidOf(FStructFallback st)
    {
        if (st.TryGetValue(out long gl, "Guid")) return gl;
        if (st.TryGetValue(out ulong gu, "Guid")) return (long)gu;
        return 0;
    }

    /// <summary>The state tree a Blueprint plays: its own StateTreeComponent's reference, else the nearest parent's; with the package that holds that reference.</summary>
    static IPackage? StateTreeOf(Game g, List<UObject> chain, out string referencePackage)
    {
        referencePackage = "";
        foreach (var c in chain)
        {
            if (c.Owner is not IPackage pkg) continue;
            foreach (var e in pkg.GetExports())
            {
                if (e.ExportType != "StateTreeComponent") continue;
                if (!e.TryGetValue(out FStructFallback tree, "StateTreeRef") || tree == null) continue;
                if (!tree.TryGetValue(out FPackageIndex idx, "StateTree") || idx == null || idx.IsNull) continue;
                var target = idx.ResolvedObject;
                var pkgName = target?.Package?.Name;
                if (string.IsNullOrEmpty(pkgName)) continue;
                try
                {
                    var loaded = g.Provider.LoadPackage(pkgName);
                    referencePackage = pkg.Name;
                    return loaded;
                }
                catch { }
            }
        }
        return null;
    }

    /// <summary>
    /// The package whose state tree reference holds the parameters a Blueprint runs with: the nearest one on the class chain that sets a bag of its own (a Blueprint that only
    /// changes a few parameters keeps the tree its parent names, and the bag is the one it saved).
    /// </summary>
    static string ParametersPackageOf(List<UObject> chain)
    {
        foreach (var c in chain)
        {
            if (c.Owner is not IPackage pkg) continue;
            foreach (var e in pkg.GetExports())
            {
                if (e.ExportType != "StateTreeComponent") continue;
                if (!e.TryGetValue(out FStructFallback reference, "StateTreeRef") || reference == null) continue;
                if (reference.Properties.Any(p => p.Name.Text == "Parameters")) return pkg.Name;
            }
        }
        return "";
    }

    // ---- root motion -----------------------------------------------------------------------------------------------------------------------------

    static IEnumerable<FStructFallback> StructsIn(FStructFallback owner, string name)
    {
        var prop = owner.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is UScriptArray arr)
            foreach (var item in arr.Properties)
                if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf) yield return sf;
    }

    /// <summary>The translation keys of the animation's root bone (seconds, cm); null when the animation does not move its root or cannot be read (`why` says which of the two).</summary>
    static List<(float Time, FVector Pos)>? RootTranslation(UAnimSequence seq, out string why)
    {
        why = "";
        if (!(seq.TryGetValue(out bool enabled, "bEnableRootMotion") && enabled)) return null;
        if (seq.CompressedDataStructure is not FUECompressedAnimData cd || cd.KeyEncodingFormat != AnimationKeyFormat.AKF_PerTrackCompression) { why = "the animation is stored in a form this tool does not read"; return null; }
        int track = -1;
        var map = seq.CompressedTrackToSkeletonMapTable;
        for (int i = 0; i < map.Length; i++) if (map[i].BoneTreeIndex == 0) { track = i; break; }
        if (track < 0 || track * 2 >= cd.CompressedTrackOffsets.Length) { why = "no root bone track"; return null; }
        int offset = cd.CompressedTrackOffsets[track * 2];
        if (offset < 0) return null;   // the root bone keeps its pose: no motion
        var ar = new FByteArchive("animation", cd.CompressedByteStream);
        ar.Position = offset;
        uint packed = ar.Read<uint>();
        var format = (AnimationCompressionFormat)(packed >> 28);
        int mask = (int)((packed >> 24) & 0xF), count = (int)(packed & 0xFFFFFF);
        bool timed = (mask & 8) != 0;
        var mins = FVector.ZeroVector; var ranges = FVector.ZeroVector;
        if (format == AnimationCompressionFormat.ACF_IntervalFixed32NoW)
        {
            if ((mask & 1) != 0) { mins.X = ar.Read<float>(); ranges.X = ar.Read<float>(); }
            if ((mask & 2) != 0) { mins.Y = ar.Read<float>(); ranges.Y = ar.Read<float>(); }
            if ((mask & 4) != 0) { mins.Z = ar.Read<float>(); ranges.Z = ar.Read<float>(); }
        }
        var keys = new FVector[count];
        for (int k = 0; k < count; k++)
        {
            switch (format)
            {
                case AnimationCompressionFormat.ACF_None:
                case AnimationCompressionFormat.ACF_Float96NoW:
                    keys[k] = (mask & 7) != 0 ? new FVector((mask & 1) != 0 ? ar.Read<float>() : 0, (mask & 2) != 0 ? ar.Read<float>() : 0, (mask & 4) != 0 ? ar.Read<float>() : 0) : ar.Read<FVector>();
                    break;
                case AnimationCompressionFormat.ACF_IntervalFixed32NoW:
                    keys[k] = ar.ReadVectorIntervalFixed32(mins, ranges);
                    break;
                case AnimationCompressionFormat.ACF_Fixed48NoW:
                    keys[k] = new FVector((mask & 1) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0,
                                          (mask & 2) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0,
                                          (mask & 4) != 0 ? AnimationCompressionUtils.DecodeFixed48_PerTrackComponent(ar.Read<ushort>(), 7) : 0);
                    break;
                case AnimationCompressionFormat.ACF_Identity:
                    keys[k] = FVector.ZeroVector;
                    break;
                default:
                    why = "translation keys in format " + format;
                    return null;
            }
        }
        ar.Position = (ar.Position + 3) & ~3L;
        var frames = new int[count];
        if (timed && count > 1)
        {
            for (int k = 0; k < count; k++) frames[k] = seq.NumFrames < 256 ? ar.Read<byte>() : ar.Read<ushort>();
        }
        else
        {
            for (int k = 0; k < count; k++) frames[k] = k;
        }
        double perFrame = seq.NumFrames > 1 ? seq.SequenceLength / (double)(seq.NumFrames - 1) : 0;
        var list = new List<(float, FVector)>(count);
        for (int k = 0; k < count; k++) list.Add(((float)(frames[k] * perFrame), keys[k]));
        return list;
    }

    static FVector Interpolate(List<(float Time, FVector Pos)> keys, float time)
    {
        if (time <= keys[0].Time) return keys[0].Pos;
        for (int i = 1; i < keys.Count; i++)
        {
            if (time > keys[i].Time) continue;
            float span = keys[i].Time - keys[i - 1].Time;
            float f = span > 0 ? (time - keys[i - 1].Time) / span : 1f;
            return keys[i - 1].Pos + (keys[i].Pos - keys[i - 1].Pos) * f;
        }
        return keys[^1].Pos;
    }

    /// <summary>
    /// How far a montage moves the creature: the root bone's translation of the animation its first segment plays, from the segment's start, in montage time.  `unreadable` is set
    /// when the montage moves the creature in a way this reads wrong or not at all (an animation stored in a form it does not know, a looping or composite segment, a second segment
    /// that moves the root too): the result is empty then and does not mean that the montage stays in place.
    /// </summary>
    static List<(float Time, float X, float Y, float Z)> ReadRootMotion(UObject montage, string name, out bool unreadable)
    {
        unreadable = false;
        var result = new List<(float, float, float, float)>();
        bool first = true;
        foreach (var slot in StructsOf(montage, "SlotAnimTracks"))
        {
            if (!slot.TryGetValue(out FStructFallback track, "AnimTrack") || track == null) continue;
            foreach (var seg in StructsIn(track, "AnimSegments"))
            {
                if (!seg.TryGetValue(out FPackageIndex reference, "AnimReference") || reference == null || reference.IsNull) { first = false; continue; }
                var resolved = Lv.Resolve(reference);
                if (!first)
                {
                    // a further segment: it must not move the root, or its travel is missing
                    if (resolved is UAnimSequence further && further.TryGetValue(out bool furtherMoves, "bEnableRootMotion") && furtherMoves) unreadable = true;
                    else if (resolved != null && resolved is not UAnimSequence) unreadable = true;
                    continue;
                }
                first = false;
                float start = seg.TryGetValue(out float s, "AnimStartTime") ? s : 0f, end = seg.TryGetValue(out float en, "AnimEndTime") ? en : 0f;
                float rate = seg.TryGetValue(out float r, "AnimPlayRate") && r > 0f ? r : 1f;
                int loops = IntOf(seg, "LoopingCount", 1);
                if (resolved is not UAnimSequence sequence)
                {
                    unreadable = resolved != null;   // (an animation of another kind)
                    continue;
                }
                if (loops != 1)
                {
                    unreadable = sequence.TryGetValue(out bool loopMoves, "bEnableRootMotion") && loopMoves;
                    continue;
                }
                var keys = RootTranslation(sequence, out string why);
                if (keys == null || keys.Count == 0)
                {
                    if (why.Length > 0)
                    {
                        Console.Error.WriteLine($"root motion of {name}: {why}");
                        unreadable = true;
                    }
                    continue;
                }
                var origin = Interpolate(keys, start);
                var samples = new List<(float, float, float, float)>();
                void Add(float seqTime)
                {
                    var d = Interpolate(keys, seqTime) - origin;
                    samples.Add(((seqTime - start) / rate, d.X, d.Y, d.Z));
                }
                Add(start);
                foreach (var k in keys) if (k.Time > start && k.Time < end) Add(k.Time);
                Add(end);
                float most = samples.Max(x => Math.Max(Math.Abs(x.Item2), Math.Max(Math.Abs(x.Item3), Math.Abs(x.Item4))));
                if (most >= 1f) result = samples;
            }
        }
        if (unreadable) return new List<(float, float, float, float)>();
        return result;
    }

    // ---- the state tree --------------------------------------------------------------------------------------------------------------------------

    static FStructFallback[] Container(List<FPropertyTag> props, string name)
    {
        var prop = props.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is not FScriptStruct ss) return Array.Empty<FStructFallback>();
        if (ss.StructType is FInstancedStructContainer c) return c.Structs ?? Array.Empty<FStructFallback>();
        return Array.Empty<FStructFallback>();
    }

    static FStructFallback[] InstanceStructs(UObject tree, string name)
    {
        var prop = tree.Properties.FirstOrDefault(x => x.Name.Text == name);
        if (prop?.Tag?.GenericValue is not FScriptStruct ss || ss.StructType is not FStateTreeInstanceData data || data.Data == null) return Array.Empty<FStructFallback>();
        return Container(data.Data.Properties, "InstanceStructs");
    }

    static int IntOf(FStructFallback? s, string name, int fallback = -1)
    {
        if (s == null) return fallback;
        if (s.TryGetValue(out int i, name)) return i;
        if (s.TryGetValue(out ushort u, name)) return u;
        if (s.TryGetValue(out byte by, name)) return by;
        if (s.TryGetValue(out short sh, name)) return sh;
        if (s.TryGetValue(out uint ui, name)) return (int)ui;
        if (s.TryGetValue(out sbyte sb, name)) return sb;
        return fallback;
    }

    static string ClassOf(FStructFallback node, string property) => node.TryGetValue(out FPackageIndex idx, property) && idx != null && !idx.IsNull ? idx.Name : "";

    /// <summary>The object a node's template index points at in the tree's instance data (null for a node that keeps its settings in plain structs).</summary>
    static UObject? InstanceOf(FStructFallback node, FStructFallback[] instances)
    {
        if (!node.TryGetValue(out FStructFallback at, "InstanceTemplateIndex")) return null;
        int i = IntOf(at, "Value");
        if (i < 0 || i >= instances.Length || instances[i] == null) return null;
        return instances[i].TryGetValue(out FPackageIndex obj, "InstanceObject") && obj != null && !obj.IsNull ? Lv.Resolve(obj) : null;
    }

    static string NameOf(UObject o, string property) => o.TryGetValue(out FName n, property) ? n.Text : o.TryGetValue(out string s, property) ? s : "";

    /// <summary>
    /// The use-ability tasks of a state tree with the conditions of the states they sit in (the state's own and every parent's, as the creature checks them on the way down).
    /// `prm` holds the values of the tree's parameters for the creature the rows are made for.
    /// </summary>
    public static List<AbilityRow> UseAbilityRows(UObject tree, Dictionary<string, object> prm)
    {
        var rows = new List<AbilityRow>();
        var info = LoadTree(tree);
        var states = info.States;
        var nodes = info.Nodes;
        var taskData = info.Defaults;
        for (int si = 0; si < states.Length; si++)
        {
            int tasksBegin = IntOf(states[si], "TasksBegin", 0), tasksNum = IntOf(states[si], "TasksNum", 0);
            for (int k = tasksBegin; k < tasksBegin + tasksNum && k < nodes.Length; k++)
            {
                if (nodes[k] == null || ClassOf(nodes[k], "TaskClass") != "AoCSTT_UseAbility") continue;
                if (nodes[k].TryGetValue(out bool taskEnabled, "bTaskEnabled") && !taskEnabled) continue;
                var t = InstanceOf(nodes[k], taskData);
                if (t == null || !t.TryGetValue(out FStructFallback id, "AbilityId") || id == null || GuidOf(id) == 0) continue;
                var row = new AbilityRow { Ability = GuidOf(id), Order = si, Task = NameOf(t, "TaskIdentifier") };
                row.Cooldown = t.TryGetValue(out float cd, "Cooldown") ? cd : 0f;
                row.StopMovement = t.TryGetValue(out bool sm, "bShouldStopMovement") && sm;
                // the state and every state above it, from the root down
                var path = new List<int>();
                for (int p = si, guard = 0; p >= 0 && p < states.Length && guard < 64; guard++)
                {
                    path.Insert(0, p);
                    p = states[p].TryGetValue(out FStructFallback par, "Parent") ? IntOf(par, "Index") : -1;
                }
                // a state the designers switched off is never entered, nor is anything below it
                if (path.Any(ps => states[ps].TryGetValue(out bool enabled, "bEnabled") && !enabled)) continue;
                row.Fight = InFight(info, path);
                // the cooldowns that the tasks of these states start when the creature takes the task
                var starts = new List<string>();
                foreach (int ps in path)
                {
                    int tb = IntOf(states[ps], "TasksBegin", 0), tn = IntOf(states[ps], "TasksNum", 0);
                    for (int j = tb; j < tb + tn && j < nodes.Length; j++)
                    {
                        if (j == k || nodes[j] == null) continue;
                        if (nodes[j].TryGetValue(out bool otherEnabled, "bTaskEnabled") && !otherEnabled) continue;
                        var other = InstanceOf(nodes[j], taskData);
                        if (other == null) continue;
                        // a Rotate To task (towards the target) that runs with the ability: the deepest one on the way down names the turning
                        if (string.Equals(ClassOf(nodes[j], "TaskClass"), "AocSTT_RotateTo", StringComparison.OrdinalIgnoreCase)
                            && (!other.TryGetValue(out FName rotationType, "RotationType") || rotationType.Text.EndsWith("RotateToTarget", StringComparison.Ordinal)))
                        {
                            float speed = other.TryGetValue(out float rotationSpeed, "RotationSpeed") ? rotationSpeed : -1f;
                            float duration = other.TryGetValue(out float rotationDuration, "RotationDuration") ? rotationDuration : 0f;
                            row.Rotate = F(speed) + ":" + F(duration);
                        }
                        string identifier = NameOf(other, "TaskIdentifier");
                        float seconds = other.TryGetValue(out float otherCooldown, "Cooldown") ? otherCooldown : 0f;
                        if (identifier.Length > 0 && seconds > 0f && identifier != row.Task && !identifier.Contains(';') && !identifier.Contains('=')) starts.Add(identifier + "=" + F(seconds));
                    }
                }
                row.Starts = string.Join(";", starts.Distinct());
                var onPath = new HashSet<int>(path);
                var gates = new List<string>();
                var groups = new List<string>();
                var why = new SortedSet<string>(StringComparer.Ordinal);
                bool off = false;
                foreach (int ps in path)
                {
                    int cb = IntOf(states[ps], "EnterConditionsBegin", 0), cn = IntOf(states[ps], "EnterConditionsNum", 0);
                    if (cn <= 0) continue;
                    var tokens = new List<(string Operand, int Delta, Atom Atom)>();
                    for (int c = cb; c < cb + cn && c < nodes.Length; c++)
                    {
                        string operand = tokens.Count == 0 ? "Copy" : TextOf(nodes[c], "Operand");
                        tokens.Add((operand, IntOf(nodes[c], "DeltaIndent", 0), Encode(info, nodes[c], onPath, prm)));
                    }
                    foreach (var tk in tokens) if (tk.Atom.Unknown) { row.Unknown = true; why.Add(Uri.UnescapeDataString(tk.Atom.Text.Substring(1))); }
                    bool plain = tokens.Skip(1).All(x => x.Operand == "And") && tokens.All(x => x.Delta == 0);
                    if (plain)
                    {
                        // a list of conditions that all have to hold: the table's own columns carry distance, angle and cooldown, `conds` the rest
                        var rest = new List<string>();
                        foreach (var (_, _, atom) in tokens)
                        {
                            if (atom.Unknown) continue;
                            switch (atom.Kind)
                            {
                                case "dist":
                                    if (atom.Inverse) row.MinDistance = Math.Max(row.MinDistance, (float)atom.Value);
                                    else row.MaxDistance = row.MaxDistance < 0 ? (float)atom.Value : Math.Min(row.MaxDistance, (float)atom.Value);
                                    break;
                                case "ang":
                                    row.MaxAngle = row.MaxAngle < 0 ? (float)atom.Value : Math.Min(row.MaxAngle, (float)atom.Value);
                                    break;
                                case "cd":
                                    gates.Add(atom.Name);
                                    break;
                                default:
                                    if (atom.Constant == "F") off = true;
                                    else if (atom.Constant != "T") rest.Add(atom.Text);
                                    break;
                            }
                        }
                        if (rest.Count > 0) groups.Add(string.Join(" ", rest.Select((a, i) => (i == 0 ? "c0:" : "a0:") + a)));
                    }
                    else
                    {
                        // brackets or an `Or`: the group is kept whole, in the order the tree has it
                        groups.Add(string.Join(" ", tokens.Select(x => (x.Operand == "Or" ? "o" : x.Operand == "And" ? "a" : "c") + (x.Delta > 0 ? "+" : "") + x.Delta.ToString(CultureInfo.InvariantCulture) + ":" + x.Atom.Text)));
                    }
                }
                row.Gates = string.Join(";", gates);
                row.Conds = string.Join("|", groups);
                row.Why = row.Unknown ? string.Join(",", why) : off ? "off" : "";
                rows.Add(row);
            }
        }
        return rows;
    }

    static void ReadMontage(UObject e, string containerPath, List<MontageRow> rows)
    {
        var pkgPath = PackagePathOf(containerPath);
        var r = new MontageRow { Package = pkgPath };
        r.Montage = pkgPath + "." + e.Name;
        r.Guid = StaticId("AnimMontage", r.Montage);
        r.PackageGuid = StaticId("Package", pkgPath);
        if (e.TryGetValue(out float len, "SequenceLength")) r.Length = len;
        if (e.TryGetValue(out float rate, "RateScale") && rate != 0f) r.RateScale = rate;
        foreach (var s in StructsOf(e, "CompositeSections"))
        {
            string name = s.TryGetValue(out FName n, "SectionName") ? n.Text : "";
            string next = s.TryGetValue(out FName nx, "NextSectionName") ? nx.Text : "None";
            float start = s.TryGetValue(out float lv, "LinkValue") ? lv : 0f;
            r.Sections.Add((name, start, next));
        }
        foreach (var nt in StructsOf(e, "Notifies"))
        {
            if (!nt.TryGetValue(out FPackageIndex idx, "Notify") || idx == null || idx.IsNull) continue;
            var obj = Lv.Resolve(idx);
            if (obj == null || obj.ExportType != "AoCAnimNotify_AbilityHit") continue;
            float time = nt.TryGetValue(out float lv, "LinkValue") ? lv : 0f;
            if (nt.TryGetValue(out float off, "TriggerTimeOffset")) time += off;
            int key = obj.TryGetValue(out int hk, "HitKey") ? hk : 0;
            r.Hits.Add((time, key));
        }
        r.Hits.Sort((a, b) => a.Time != b.Time ? a.Time.CompareTo(b.Time) : a.Key.CompareTo(b.Key));
        r.RootMotion = ReadRootMotion(e, r.Montage, out bool unreadable);
        r.RootUnreadable = unreadable;
        rows.Add(r);
    }

    /// <summary>Reads every package under the container folders: the creature Blueprints (their state trees' ability tasks) and the montages.</summary>
    public static void Collect(Game g, IEnumerable<string> folders, out List<AbilityRow> abilities, out List<MontageRow> montages, out int packages, out int failed)
    {
        abilities = new List<AbilityRow>();
        montages = new List<MontageRow>();
        packages = 0; failed = 0;
        var dirs = folders.Select(f => f.Replace('\\', '/').TrimEnd('/') + "/").ToList();
        var paths = g.Provider.Files.Keys.Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && dirs.Any(d => k.StartsWith(d, StringComparison.OrdinalIgnoreCase)))
                                          .OrderBy(k => k, StringComparer.Ordinal).ToList();
        var treeCache = new Dictionary<string, List<AbilityRow>>();
        var bags = new Dictionary<string, Parameters.Bag?>();
        Parameters.Bag? BagOf(string containerPath, bool tree)
        {
            string key = (tree ? "T|" : "B|") + containerPath;
            if (!bags.TryGetValue(key, out var bag)) bags[key] = bag = Parameters.Read(g, containerPath, tree);
            return bag;
        }
        foreach (var path in paths)
        {
            packages++;
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { failed++; continue; }
            if (pkg == null) { failed++; continue; }
            var exports = pkg.GetExports().ToList();
            foreach (var e in exports.Where(x => x.ExportType == "AnimMontage")) ReadMontage(e, path, montages);
            var cdo = exports.FirstOrDefault(x => x.Name.StartsWith("Default__", StringComparison.Ordinal) && x.Name.EndsWith("_C", StringComparison.Ordinal));
            if (cdo == null) continue;
            var chain = CdoChain(cdo);
            if (!chain.Any(c => ClassName(c) == "NPCPawn_C")) continue;
            var tree = StateTreeOf(g, chain, out string referencePackage);
            if (tree == null) continue;
            string archetype = PackagePathOf(path) + "." + cdo.Name;
            var treeObject = tree.GetExports().FirstOrDefault(x => x.ExportType == "StateTree");
            if (treeObject == null) continue;
            // the parameters this creature's tree runs with: the tree's defaults, and what the Blueprint's reference overrides
            string parametersPackage = ParametersPackageOf(chain);
            var prm = Parameters.Effective(BagOf(ContainerPathOf(tree.Name), true), parametersPackage.Length > 0 ? BagOf(ContainerPathOf(parametersPackage), false) : null);
            string signature = tree.Name + "|" + string.Join(";", prm.OrderBy(x => x.Key, StringComparer.Ordinal).Select(x => x.Key + "=" + (x.Value is double d ? d.ToString("R", CultureInfo.InvariantCulture) : x.Value.ToString())));
            if (!treeCache.TryGetValue(signature, out var treeRows)) treeCache[signature] = treeRows = UseAbilityRows(treeObject, prm);
            foreach (var r in treeRows)
            {
                abilities.Add(new AbilityRow
                {
                    Archetype = archetype, Ability = r.Ability, Task = r.Task, Gates = r.Gates, Cooldown = r.Cooldown, StopMovement = r.StopMovement, Unknown = r.Unknown, Order = r.Order,
                    MinDistance = r.MinDistance, MaxDistance = r.MaxDistance, MaxAngle = r.MaxAngle, Fight = r.Fight, Conds = r.Conds, Why = r.Why, Starts = r.Starts, Rotate = r.Rotate
                });
            }
        }
        abilities = abilities.OrderBy(a => a.Archetype, StringComparer.Ordinal).ThenBy(a => a.Order).ThenBy(a => a.Ability).ToList();
        montages = montages.OrderBy(m => m.Montage, StringComparer.Ordinal).ToList();
    }

    static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);

    public static string AbilitiesTsv(IEnumerable<AbilityRow> rows)
    {
        var sb = new StringBuilder();
        sb.Append(AbilitiesHeader).Append('\n');
        foreach (var r in rows)
            sb.Append(r.Archetype).Append('\t').Append(r.Ability.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(r.Task).Append('\t').Append(F(r.Cooldown)).Append('\t').Append(r.StopMovement ? "1" : "0").Append('\t')
              .Append(r.Order.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(F(r.MinDistance)).Append('\t').Append(F(r.MaxDistance)).Append('\t').Append(F(r.MaxAngle)).Append('\t')
              .Append(r.Gates).Append('\t').Append(r.Unknown ? "1" : "0").Append('\t').Append(r.Fight ? "1" : "0").Append('\t').Append(r.Conds).Append('\t').Append(r.Why).Append('\t').Append(r.Starts).Append('\t').Append(r.Rotate).Append('\n');
        return sb.ToString();
    }

    public static string MontagesTsv(IEnumerable<MontageRow> rows)
    {
        var sb = new StringBuilder();
        sb.Append(MontagesHeader).Append('\n');
        foreach (var r in rows)
        {
            sb.Append(r.Montage).Append('\t').Append(r.Guid.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(r.Package).Append('\t').Append(r.PackageGuid.ToString(CultureInfo.InvariantCulture)).Append('\t')
              .Append(F(r.Length)).Append('\t').Append(F(r.RateScale)).Append('\t')
              .Append(string.Join(";", r.Sections.Select(s => s.Name + ":" + F(s.Start) + ":" + s.Next))).Append('\t')
              .Append(string.Join(";", r.Hits.Select(h => F(h.Time) + ":" + h.Key.ToString(CultureInfo.InvariantCulture)))).Append('\t')
              .Append(string.Join(";", r.RootMotion.Select(m => F(m.Time) + ":" + F(m.X) + ":" + F(m.Y) + ":" + F(m.Z)))).Append('\t')
              .Append(r.RootUnreadable ? "1" : "0").Append('\n');
        }
        return sb.ToString();
    }
}
