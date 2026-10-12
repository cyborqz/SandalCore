// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>
/// What each pawn Blueprint under /Game says about the creature it makes (read from its class default object along the Blueprint's parents): the
/// facts a server needs to open an NPC of an archetype no retail capture opened (plan E1, stage 2 fix).  One line per Blueprint whose class descends
/// from NPCPawn_C, in path order:
///   archetype  parents  default_scale  starting_alignments  half_height  radius  max_walk_speed  max_run_speed  max_acceleration  rotation_rate_yaw
///   move_mode  fly_speed  tags  mesh  anim  placeholder  hidden  lists  dist  elite  rewards
/// The last four (plan E1, stage 3, the spawn filter) say whether the Blueprint draws at all: the SkeletalMesh and AnimClass its CharacterMesh0
/// resolves along the Blueprint's archetype chain ("" for none: a humanoid's body is merged at run time from its NPCAppearance, a creature's is the
/// Blueprint's own), whether a material it overrides is the engine's placeholder ("PinkMaterial"), and whether the actor or its mesh is hidden.
/// And the NPC's combat facts (plan E1, stage 3): `lists` the stat initializer lists its StatsComponent names, "base;list1;list2" as record guids
/// (CombatStats; NPC_Base and NPC_Elite_Scalar on NPCPawn itself; a child that sets none inherits), `dist` the StatDistribution* weights
/// "Constitution,Strength,Dexterity,Mentality,Intelligence,Wisdom,MaxHealth,MaxMana", `elite` the EliteLevelRanks "level:rank,..." and `rewards`
/// the RewardTable guids of its Rewards, comma separated.
/// </summary>
public static class NpcBlueprints
{
    public sealed class Row
    {
        public string Archetype = "";
        public string Parents = "";              // the Blueprint's parent classes up to NPCPawn_C, nearest first, comma separated
        public double Scale = 1.0;
        public List<long> Alignments = new();
        public double HalfHeight = 88, Radius = 34, Walk = 600, Run = 600, Accel = 2048, Yaw = 360;
        public string MoveMode = "";             // DefaultCharacterMovementMode / DefaultLandMovementMode, the last the Blueprints set
        public double FlySpeed = 0;
        public string Tags = "";
        public string Mesh = "", Anim = "";
        public string Lists = "", Dist = "", Elite = "", Rewards = "";
        public bool Placeholder = false, Hidden = false;
    }

    public const string Header = "archetype\tparents\tdefault_scale\tstarting_alignments\thalf_height\tradius\tmax_walk_speed\tmax_run_speed\tmax_acceleration\trotation_rate_yaw\tmove_mode\tfly_speed\ttags\tmesh\tanim\tplaceholder\thidden\tlists\tdist\telite\trewards";

    public static bool IsPawnPath(string p) =>
        p.StartsWith("AOC/Content/DesignAssets/", StringComparison.OrdinalIgnoreCase) || p.StartsWith("AOC/Content/ThirdPersonCPP/", StringComparison.OrdinalIgnoreCase);

    /// <summary>Every package that may hold a pawn Blueprint (the design assets and NPCPawn's own folder), in path order.</summary>
    public static IEnumerable<string> AllPaths(Game g) =>
        g.Provider.Files.Keys.Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && IsPawnPath(k)).OrderBy(k => k, StringComparer.Ordinal);
// -- Cyb x C | O | A --

    /// <summary>The class chain of a Blueprint's default object: the default objects of its parents, nearest first (each is an archetype of the next).</summary>
    static List<string> Chain(UObject cdo, out List<UObject> cdos)
    {
        var names = new List<string>();
        cdos = new List<UObject>();
        UObject? o = cdo;
        for (int i = 0; o != null && i < 12; i++)
        {
            cdos.Add(o);
            var n = o.Name;
            if (n.StartsWith("Default__", StringComparison.Ordinal)) n = n.Substring(9);
            names.Add(n);
            o = o.Template?.Object?.Value;
        }
        return names;
    }

    public static List<Row> Collect(Game g, IEnumerable<string> packagePaths, out int packages, out int failed)
    {
        var rows = new List<Row>();
        packages = 0; failed = 0;
        foreach (var path in packagePaths.OrderBy(k => k, StringComparer.Ordinal))
        {
            packages++;
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { failed++; continue; }
            if (pkg == null) { failed++; continue; }
            UObject? cdo = null;
            foreach (var e in pkg.GetExports())
                if (e.Name.StartsWith("Default__", StringComparison.Ordinal) && e.Name.EndsWith("_C", StringComparison.Ordinal)) { cdo = e; break; }
            if (cdo == null) continue;
            var chain = Chain(cdo, out var cdos);
            if (!chain.Contains("NPCPawn_C")) continue;
            var r = new Row();
            string pkgPath = "/Game/" + path.Substring("AOC/Content/".Length, path.Length - "AOC/Content/".Length - ".uasset".Length);
            r.Archetype = pkgPath + "." + cdo.Name;
            r.Parents = string.Join(",", chain.Skip(1));
            foreach (var c in cdos) { if (c.TryGetValue(out float sc, "DefaultScale")) { r.Scale = sc; break; } }
            foreach (var c in cdos)
            {
                var prop = c.Properties.FirstOrDefault(x => x.Name.Text == "StartingAlignments");
                if (prop?.Tag?.GenericValue is UScriptArray al)
                {
                    foreach (var item in al.Properties)
                        if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf && (sf.TryGetValue(out ulong guid, "Guid") ? true : sf.TryGetValue(out long gl, "Guid") && (guid = (ulong)gl) == guid)) r.Alignments.Add((long)guid);
                    break;
                }
            }
            foreach (var c in cdos) { if (c.TryGetValue(out FName mm, "DefaultCharacterMovementMode")) { r.MoveMode = mm.Text; break; } }
            foreach (var c in cdos) { if (c.TryGetValue(out FName[] tags, "GameplayTags") && tags != null) { r.Tags = string.Join(",", tags.Select(t => t.Text)); break; } }
            UObject? capsule = null, move = null, skel = null;
            foreach (var e in pkg.GetExports())
            {
                if (e.Outer != cdo) continue;
                if (e.ExportType == "CapsuleComponent") capsule = e;
                else if (e.ExportType == "AoCCharacterMovement" || e.ExportType == "CharacterMovementComponent") move = e;
                else if (e.Name == "CharacterMesh0" && e.ExportType.Contains("SkeletalMeshComponent", StringComparison.Ordinal)) skel = e;
            }
            r.HalfHeight = Lv.TryProp<float>(capsule, "CapsuleHalfHeight", out var h) ? h : 88.0;
            r.Radius = Lv.TryProp<float>(capsule, "CapsuleRadius", out var rad) ? rad : 34.0;
            r.Walk = Lv.TryProp<float>(move, "MaxWalkSpeed", out var w) ? w : 600.0;
            r.Run = Lv.TryProp<float>(move, "MaxRunSpeed", out var ru) ? ru : r.Walk;
            r.Accel = Lv.TryProp<float>(move, "MaxAcceleration", out var a2) ? a2 : 2048.0;
            r.Yaw = Lv.TryProp<FRotator>(move, "RotationRate", out var rr) ? rr.Yaw : 360.0;
            if (r.MoveMode.Length == 0 && Lv.TryProp<FName>(move, "DefaultLandMovementMode", out var dl)) r.MoveMode = dl.Text;
            r.FlySpeed = Lv.TryProp<float>(move, "MaxFlySpeed", out var fs) ? fs : 0;
            // does it draw: the mesh component's properties along its own archetype chain (the parents' packages), the actor's along the class chain
            if (skel != null)
            {
                if (Lv.TryProp<FPackageIndex>(skel, "SkeletalMesh", out var sk) && !sk.IsNull) r.Mesh = Lv.PathOf(sk);
                else if (Lv.TryProp<FPackageIndex>(skel, "SkinnedAsset", out var sa) && !sa.IsNull) r.Mesh = Lv.PathOf(sa);
                if (Lv.TryProp<FPackageIndex>(skel, "AnimClass", out var ac) && !ac.IsNull) r.Anim = Lv.PathOf(ac);
                if (Lv.TryProp<UScriptArray>(skel, "OverrideMaterials", out var om) && om != null)
                    foreach (var item in om.Properties)
                        if (item.GenericValue is FPackageIndex mi && !mi.IsNull && Lv.PathOf(mi).Contains("PinkMaterial", StringComparison.OrdinalIgnoreCase)) r.Placeholder = true;
                if (Lv.TryProp<bool>(skel, "bHiddenInGame", out var mh) && mh) r.Hidden = true;
                if (Lv.TryProp<bool>(skel, "bVisible", out var mv) && !mv) r.Hidden = true;
            }
            foreach (var c in cdos)
            {
                if (c.TryGetValue(out bool bh, "bHidden") && bh) { r.Hidden = true; break; }
                if (c.TryGetValue(out bool bg, "bActorHiddenInGame") && bg) { r.Hidden = true; break; }
            }
            // what the NPC fights with: the stat lists of its StatsComponent, the stat weights, the elite ranks and the reward tables
            UObject? stats = null;
            foreach (var e in pkg.GetExports())
                if (e.Outer == cdo && e.Name == "StatsComponent") { stats = e; break; }
            r.Lists = GuidOf(stats, "BaseStatInitializer") + ";" + GuidOf(stats, "StatInitializerList1") + ";" + GuidOf(stats, "StatInitializerList2");
            var dist = new List<string>();
            foreach (var name in new[] { "Constitution", "Strength", "Dexterity", "Mentality", "Intelligence", "Wisdom", "MaxHealth", "MaxMana" })
            {
                int v = 0;
                foreach (var c in cdos) { if (c.TryGetValue(out int d, "StatDistribution" + name)) { v = d; break; } }
                dist.Add(v.ToString(CultureInfo.InvariantCulture));
            }
            r.Dist = string.Join(",", dist);
            foreach (var c in cdos)
            {
                var prop = c.Properties.FirstOrDefault(x => x.Name.Text == "EliteLevelRanks");
                if (prop?.Tag?.GenericValue is UScriptArray ea)
                {
                    var pairs = new List<string>();
                    foreach (var item in ea.Properties)
                        if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf && sf.TryGetValue(out int lv, "Level") && sf.TryGetValue(out int rk, "Rank"))
                            pairs.Add(lv.ToString(CultureInfo.InvariantCulture) + ":" + rk.ToString(CultureInfo.InvariantCulture));
                    r.Elite = string.Join(",", pairs);
                    break;
                }
            }
            foreach (var c in cdos)
            {
                var prop = c.Properties.FirstOrDefault(x => x.Name.Text == "Rewards");
                if (prop?.Tag?.GenericValue is UScriptArray ra)
                {
                    var guids = new List<string>();
                    foreach (var item in ra.Properties)
                        if (item.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback sf && (sf.TryGetValue(out long gl, "Guid") ? true : sf.TryGetValue(out ulong gu, "Guid") && (gl = (long)gu) == gl))
                            guids.Add(gl.ToString(CultureInfo.InvariantCulture));
                    r.Rewards = string.Join(",", guids);
                    break;
                }
            }
            rows.Add(r);
        }
        rows.Sort((x, y) => string.CompareOrdinal(x.Archetype, y.Archetype));
        return rows;
    }
// built by CoA with C-Y-B

    /// <summary>The record guid a struct property of a component holds ({"Guid": n}), searched along the component's archetype chain; 0 when it has none.</summary>
    static string GuidOf(UObject? comp, string name)
    {
        for (int depth = 0; comp != null && depth < 8; depth++)
        {
            if (comp.TryGetValue(out FStructFallback st, name) && st != null)
            {
                if (st.TryGetValue(out long gl, "Guid")) return gl.ToString(CultureInfo.InvariantCulture);
                if (st.TryGetValue(out ulong gu, "Guid")) return ((long)gu).ToString(CultureInfo.InvariantCulture);
                return "0";
            }
            comp = comp.Template?.Object?.Value;
        }
        return "0";
    }

    public static string ToTsv(IEnumerable<Row> rows)
    {
        var sb = new StringBuilder();
        sb.Append(Header).Append('\n');
        foreach (var r in rows)
        {
            sb.Append(r.Archetype).Append('\t').Append(r.Parents).Append('\t').Append(F(r.Scale)).Append('\t').Append(string.Join(",", r.Alignments)).Append('\t')
              .Append(F(r.HalfHeight)).Append('\t').Append(F(r.Radius)).Append('\t').Append(F(r.Walk)).Append('\t').Append(F(r.Run)).Append('\t')
              .Append(F(r.Accel)).Append('\t').Append(F(r.Yaw)).Append('\t').Append(r.MoveMode).Append('\t').Append(F(r.FlySpeed)).Append('\t').Append(r.Tags).Append('\t').Append(r.Mesh).Append('\t').Append(r.Anim).Append('\t')
              .Append(r.Placeholder ? "1" : "0").Append('\t').Append(r.Hidden ? "1" : "0").Append('\t').Append(r.Lists).Append('\t').Append(r.Dist).Append('\t')
              .Append(r.Elite).Append('\t').Append(r.Rewards).Append('\n');
        }
        return sb.ToString();
    }

    static string F(double v) => v.ToString("0.###", CultureInfo.InvariantCulture);
}
