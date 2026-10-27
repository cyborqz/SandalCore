// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Objects.Engine.Curves;
using CUE4Parse.UE4.Objects.UObject;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// What a creature notices and how far it follows, read from the game's containers: the settings of each creature Blueprint's intelligence component along the Blueprint's
/// parents (a child sets only what it changes, the rest is its parent's, down to the base pawn), and whether its state tree saves a leash location when a fight begins.
/// `npc_senses.tsv` has one line per creature Blueprint (as npc_motion.tsv names it):
///   archetype  aggro_radius  aggro_curve  link_radius  sight  sight_radius  sight_vertical  sight_cone  hearing  hearing_radius  hearing_vertical  threat_cooldown  pc_interval  leash_distance  detect_players  leash_anchor
/// Distances are centimetres, `sight` / `hearing` / `detect_players` are 0 or 1, `aggro_curve` the name of the curve that scales the aggro radius by the creature's level, and
/// `leash_anchor` is "combat_start" when the creature's state tree saves the place where a fight began as its leash location (the creature gives up a chase measured from there),
/// else empty. An empty cell is a setting no Blueprint on the chain makes: the game's own default for it stands.
/// `npc_aggro_curves.tsv` has the factor of each of those curves at the levels 1 to 100:  curve  level  factor.
/// </summary>
public static partial class Reader
{
    public const string SensesFile = "npc_senses.tsv";
    public const string AggroCurvesFile = "npc_aggro_curves.tsv";
    public const string SensesHeader = "archetype\taggro_radius\taggro_curve\tlink_radius\tsight\tsight_radius\tsight_vertical\tsight_cone\thearing\thearing_radius\thearing_vertical\tthreat_cooldown\tpc_interval\tleash_distance\tdetect_players\tleash_anchor";
    public const string AggroCurvesHeader = "curve\tlevel\tfactor";
    public const int AggroCurveLevels = 100;

    public sealed class SensesRow
    {
        public string Archetype = "", AggroCurve = "", LeashAnchor = "";
        // -1 / null: no Blueprint on the chain sets it
        public float AggroRadius = -1, LinkRadius = -1, SightRadius = -1, SightVertical = -1, SightCone = -1, HearingRadius = -1, HearingVertical = -1, ThreatCooldown = -1, PcInterval = -1, LeashDistance = -1;
        public int Sight = -1, Hearing = -1, DetectPlayers = -1;
    }

    public sealed class AggroCurve
    {
        public string Name = "";
        public float[] Factors = new float[AggroCurveLevels];   // index 0 = level 1
    }

    static bool FloatOf(List<UObject> comps, string name, out float value)
    {
        foreach (var c in comps)
            if (c.TryGetValue(out float v, name)) { value = v; return true; }
        value = -1;
        return false;
    }

    static int BoolOf(List<UObject> comps, string name)
    {
        foreach (var c in comps)
            if (c.TryGetValue(out bool v, name)) return v ? 1 : 0;
        return -1;
    }

    /// <summary>Reads the creature Blueprints under the container folders: each one's intelligence component settings and the curves they name.</summary>
    public static void CollectSenses(Game g, IEnumerable<string> folders, out List<SensesRow> rows, out List<AggroCurve> curves, out int packages, out int failed)
    {
        rows = new List<SensesRow>();
        packages = 0; failed = 0;
        var dirs = folders.Select(f => f.Replace('\\', '/').TrimEnd('/') + "/").ToList();
        var paths = g.Provider.Files.Keys.Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && dirs.Any(d => k.StartsWith(d, StringComparison.OrdinalIgnoreCase)))
                                          .OrderBy(k => k, StringComparer.Ordinal).ToList();
        var treeSaves = new Dictionary<string, bool>();
        var curvePackages = new SortedDictionary<string, string>(StringComparer.Ordinal);   // package name -> display name
        int notLoaded = 0;
        foreach (var path in paths)
        {
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { notLoaded++; continue; }   // (a package that is no Blueprint need not load)
            if (pkg == null) { notLoaded++; continue; }
            UObject? cdo = null;
            foreach (var e in pkg.GetExports())
                if (e.Name.StartsWith("Default__", StringComparison.Ordinal) && e.Name.EndsWith("_C", StringComparison.Ordinal)) { cdo = e; break; }
            if (cdo == null) continue;
            var chain = CdoChain(cdo);
            if (!chain.Any(c => ClassName(c) == "NPCPawn_C")) continue;
            packages++;
            var comps = new List<UObject>();
            foreach (var c in chain)
            {
                if (c.Owner is not IPackage cp) continue;
                foreach (var e in cp.GetExports())
                    if (e.ExportType == "AIIntelligenceComponent" && e.Name == "IntelligenceComponent" && e.Outer != null && e.Outer.Name == c.Name) { comps.Add(e); break; }
            }
            if (comps.Count == 0) { failed++; continue; }
            var r = new SensesRow { Archetype = PackagePathOf(path) + "." + cdo.Name };
            if (FloatOf(comps, "AggroRadius", out float f)) r.AggroRadius = f;
            if (FloatOf(comps, "AggroProximityLinkRadius", out f)) r.LinkRadius = f;
            if (FloatOf(comps, "SightRadius", out f)) r.SightRadius = f;
            if (FloatOf(comps, "SightVerticalTolerance", out f)) r.SightVertical = f;
            if (FloatOf(comps, "SightConeAngle", out f)) r.SightCone = f;
            if (FloatOf(comps, "HearingRadius", out f)) r.HearingRadius = f;
            if (FloatOf(comps, "HearingVerticalTolerance", out f)) r.HearingVertical = f;
            if (FloatOf(comps, "ThreatCooldownPeriod", out f)) r.ThreatCooldown = f;
            if (FloatOf(comps, "PCDetectionInterval", out f)) r.PcInterval = f;
            if (FloatOf(comps, "LeashDistance", out f)) r.LeashDistance = f;
            r.Sight = BoolOf(comps, "bEnableSight");
            r.Hearing = BoolOf(comps, "bEnableHearing");
            r.DetectPlayers = BoolOf(comps, "bDetectPlayers");
            foreach (var c in comps)
            {
                if (!c.TryGetValue(out FPackageIndex idx, "AggroRadiusNpcLevelCurve") || idx == null) continue;
                if (idx.IsNull) { r.AggroCurve = ""; break; }
                var target = idx.ResolvedObject;
                var pkgName = target?.Package?.Name;
                if (string.IsNullOrEmpty(pkgName)) break;
                string display = pkgName.Substring(pkgName.LastIndexOf('/') + 1);
                curvePackages[pkgName] = display;
                r.AggroCurve = display;
                break;
            }
            // the state tree saves the place a fight began as the leash location (its "Save Leash Location" task)
            var tree = StateTreeOf(g, chain, out _);
            if (tree != null)
            {
                if (!treeSaves.TryGetValue(tree.Name, out bool saves))
                    treeSaves[tree.Name] = saves = tree.GetExports().Any(x => x.ExportType == "AoCSTT_SaveLeashLocation");
                if (saves) r.LeashAnchor = "combat_start";
            }
            rows.Add(r);
        }
        rows = rows.OrderBy(x => x.Archetype, StringComparer.Ordinal).ToList();
        if (notLoaded > 0) Console.WriteLine($"npcabilities: {notLoaded} of {paths.Count} packages under the senses folders did not load (they are no creature Blueprints, or unreadable)");
        curves = new List<AggroCurve>();
        foreach (var (pkgName, display) in curvePackages)
        {
            IPackage? cp;
            try { cp = g.Provider.LoadPackage(pkgName); } catch { failed++; continue; }
            UObject? export = cp?.GetExports().FirstOrDefault(e => e.ExportType == "CurveFloat");
            if (export == null || !export.TryGetValue(out FRichCurve fc, "FloatCurve") || fc == null) { failed++; continue; }
            var curve = new AggroCurve { Name = display };
            for (int level = 1; level <= AggroCurveLevels; level++) curve.Factors[level - 1] = fc.Eval((float)level);
            curves.Add(curve);
        }
    }

    public static string SensesTsv(IEnumerable<SensesRow> rows)
    {
        static string N(float v) => v < 0 ? "" : v.ToString("R", CultureInfo.InvariantCulture);
        static string B(int v) => v < 0 ? "" : v.ToString(CultureInfo.InvariantCulture);
        var sb = new StringBuilder();
        sb.Append(SensesHeader).Append('\n');
        foreach (var r in rows)
            sb.Append(r.Archetype).Append('\t').Append(N(r.AggroRadius)).Append('\t').Append(r.AggroCurve).Append('\t').Append(N(r.LinkRadius)).Append('\t').Append(B(r.Sight)).Append('\t')
              .Append(N(r.SightRadius)).Append('\t').Append(N(r.SightVertical)).Append('\t').Append(N(r.SightCone)).Append('\t').Append(B(r.Hearing)).Append('\t').Append(N(r.HearingRadius)).Append('\t')
              .Append(N(r.HearingVertical)).Append('\t').Append(N(r.ThreatCooldown)).Append('\t').Append(N(r.PcInterval)).Append('\t').Append(N(r.LeashDistance)).Append('\t').Append(B(r.DetectPlayers)).Append('\t')
              .Append(r.LeashAnchor).Append('\n');
        return sb.ToString();
    }

    public static string AggroCurvesTsv(IEnumerable<AggroCurve> curves)
    {
        var sb = new StringBuilder();
        sb.Append(AggroCurvesHeader).Append('\n');
        foreach (var c in curves.OrderBy(x => x.Name, StringComparer.Ordinal))
            for (int level = 1; level <= AggroCurveLevels; level++)
                sb.Append(c.Name).Append('\t').Append(level.ToString(CultureInfo.InvariantCulture)).Append('\t').Append(c.Factors[level - 1].ToString("R", CultureInfo.InvariantCulture)).Append('\n');
        return sb.ToString();
    }
}
