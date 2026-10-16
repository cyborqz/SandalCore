// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Objects.Engine.Curves;

namespace Mapc;

/// <summary>
/// The profession level-up curves (Gathering stage 2, "Gathering data"): how much experience a profession level costs.
/// The design database (CacheDB.dbc) does not hold them. Each of the 22 profession LevelingTypeDefs keeps a placeholder LevelUpCurve in the data
/// (0 at level 0, 50,000 at level 50) and names the real one as its ExternalCurve, a pak asset: Unreal's FRuntimeFloatCurve uses the external
/// curve and ignores its own keys when one is set. The StatCurve rows Gathering_LevelUpCurve, Processing_LevelUpCurve and Crafting_LevelUpCurve (the
/// ones the GM level scrolls' formula `EvalCurve(Processing_LevelUpCurve, 9.0)` names) are empty for the same reason: their keys are these assets'.
///
/// `mapc artisan-curves` reads the three packages (no map: a few seconds) and writes dist/content/artisan_curves.tsv, one line per key, in package
/// path order:
///   curve  ord  time  value  interp  arrive_tangent  leave_tangent  pre_extrap  post_extrap
/// `curve` is the asset's package path as the data names it in ExternalCurve, without the object name after the dot
/// (/Game/DesignAssets/Artisanship/Gathering/GatheringLevelUpCurve); time is the profession level and value the experience total that level
/// needs. Floats are the assets' float32, written round-trip ("R"). contentc reads the file by the `--artisan-curves` flag (default: beside its
/// output) and stores the keys in stat_curve and the levels in leveling_curve.
/// </summary>
public static class ArtisanCurves
{
    public const string FileName = "artisan_curves.tsv";
    public const string Header = "curve\tord\ttime\tvalue\tinterp\tarrive_tangent\tleave_tangent\tpre_extrap\tpost_extrap";

    /// <summary>The curve packages, by their path in the containers (the object path in the data is "/Game/" + the part after "AOC/Content/").</summary>
    public static readonly string[] Packages =
    {
        "AOC/Content/DesignAssets/Artisanship/Crafting/CraftingLevelUpCurve.uasset",
        "AOC/Content/DesignAssets/Artisanship/Gathering/GatheringLevelUpCurve.uasset",
        "AOC/Content/DesignAssets/Artisanship/Processing/ProcessingLevelUpCurve.uasset",
    };

    public sealed class Key
    {
        public string Interp = "", PreExtrap = "", PostExtrap = "";
        public float Time, Value, Arrive, Leave;
    }

    public sealed class Curve
    {
        public string Name = "";                       // /Game/DesignAssets/Artisanship/Gathering/GatheringLevelUpCurve
        public List<Key> Keys = new();
    }

    /// <summary>The package path the data's ExternalCurve names (before the dot) for a container path.</summary>
    public static string ObjectPathOf(string containerPath) =>
        "/Game/" + containerPath.Substring("AOC/Content/".Length, containerPath.Length - "AOC/Content/".Length - ".uasset".Length);

    public static List<Curve> Collect(Game g, out int packages, out int failed)
    {
        var rows = new List<Curve>();
        packages = 0; failed = 0;
        foreach (var path in Packages.OrderBy(k => k, StringComparer.Ordinal))
        {
            packages++;
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { failed++; continue; }
            if (pkg == null) { failed++; continue; }
            UObject? export = null;
            foreach (var e in pkg.GetExports())
                if (e.ExportType == "CurveFloat") { export = e; break; }
            if (export == null || !export.TryGetValue(out FRichCurve fc, "FloatCurve") || fc == null) { failed++; continue; }
            var c = new Curve { Name = ObjectPathOf(path) };
            string pre = fc.PreInfinityExtrap.ToString(), post = fc.PostInfinityExtrap.ToString();
            foreach (var k in fc.Keys)
                c.Keys.Add(new Key { Time = k.Time, Value = k.Value, Arrive = k.ArriveTangent, Leave = k.LeaveTangent, Interp = k.InterpMode.ToString().Replace("RCIM_", ""), PreExtrap = pre.Replace("RCCE_", ""), PostExtrap = post.Replace("RCCE_", "") });
            rows.Add(c);
        }
        return rows;
    }
// [C O A + Cyb]

    public static string ToTsv(IEnumerable<Curve> curves)
    {
        var sb = new StringBuilder();
        sb.Append(Header).Append('\n');
        foreach (var c in curves)
        {
            int ord = 0;
            foreach (var k in c.Keys)
            {
                sb.Append(c.Name).Append('\t').Append(ord++).Append('\t').Append(F(k.Time)).Append('\t').Append(F(k.Value)).Append('\t').Append(k.Interp).Append('\t')
                  .Append(F(k.Arrive)).Append('\t').Append(F(k.Leave)).Append('\t').Append(k.PreExtrap).Append('\t').Append(k.PostExtrap).Append('\n');
            }
        }
        return sb.ToString();
    }

    static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);
}
