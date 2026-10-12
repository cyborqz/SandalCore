// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Mapc;

/// <summary>
/// What each mount Blueprint under /Game/GameBlueprints/Mounts says about how fast its mount moves (the mounts milestone, stage 1): the movement
/// component's MaxWalkSpeed, MaxRunSpeed, MaxFlySpeed and MaxSwimSpeed, read from the class default object along the Blueprint's parents (a child Blueprint
/// keeps the defaults of Mount_Grounded_Base / Mount_Flying_Base / Mount_Aquatic_Base / Mount_Gliding_Base and of Mount_Base, and sets what it changes).
/// A ground mount's speed is its MaxRunSpeed, not its MaxWalkSpeed: the walk is 220 (300 for two) in every Blueprint, and the 165 % terror bird sets
/// MaxRunSpeed 990, the number capture 959 measures.  `mapc mount-speeds` reads only these 141 packages (no map, a few seconds) and writes
/// dist/content/mount_speeds.tsv, one line per Blueprint that has a class default object, in path order:
///   blueprint  kind  parents  max_walk_speed  max_run_speed  max_fly_speed  max_swim_speed  defaults
/// `kind` is the first of Grounded, Flying, Aquatic, Gliding the parents name ("" for a class that is none: a coin, a state table); `parents` the class
/// chain up to the Blueprint root, nearest first; `defaults` lists which of walk, run, fly and swim no Blueprint of the chain sets (the value is then the engine's
/// default for walk (600), fly (600) and swim (300), and the walk speed for run, as the NPC table does).  A package with no movement component anywhere in its chain has empty speeds.
/// The same pass reads each Blueprint's rider AnimBP (stage 3 fix 2) into mount_riders.tsv, `blueprint  rider_anim`: the class path of the AnimBP its InteractableComponent's
/// interaction point (Mount_Interaction) names as AttachmentAnimation, along the Blueprint's parents (a child keeps its parent's component data).  The server names it in the
/// mount's open and exports it; the client puts the rider's mesh on it, which is what makes the rider sit.
/// The speeds are the mount's own at MoveSpeedMult 1: the server multiplies them (a road adds +0.10 to +0.30, the dash x1.35).  The item text's
/// percentages are the same numbers over the base (165 % ground speed is 990 = 600 x 1.65; the flying 215 % is 1,290 and the water 230 % is 920 = 400 x 2.30
/// for an aquatic mount, whose water base is 400): services/cmd/contentc's real test holds the two to each other and lists where they disagree.
/// </summary>
public static class MountSpeeds
{
    public const string Folder = "AOC/Content/GameBlueprints/Mounts/";
    public const string FileName = "mount_speeds.tsv";
    public const string RidersFileName = "mount_riders.tsv";
    public const string RidersHeader = "blueprint\trider_anim";
    public const string Header = "blueprint\tkind\tparents\tmax_walk_speed\tmax_run_speed\tmax_fly_speed\tmax_swim_speed\tdefaults";

    // UCharacterMovementComponent's defaults (UE 5.6): the value when no Blueprint of the chain sets the property
    const double DefaultWalk = 600, DefaultFly = 600, DefaultSwim = 300;

    public sealed class Row
    {
        public string Blueprint = "", Kind = "", Parents = "";
        public bool HasMovement;
        public double Walk, Run, Fly, Swim;
        public string Defaults = "";
        public string RiderAnim = "";   // "/Game/Characters/Animations/Mounts_Rider/Swiftclaw/ABP_PC_MNT_Swiftclaw.ABP_PC_MNT_Swiftclaw_C"; "" when no component of the chain names one
    }

    /// <summary>Every package of the mounts folder, in path order.</summary>
    public static IEnumerable<string> AllPaths(Game g) =>
        g.Provider.Files.Keys.Where(k => k.StartsWith(Folder, StringComparison.OrdinalIgnoreCase) && k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase))
            .OrderBy(k => k, StringComparer.Ordinal);

    static bool IsMovement(UObject e) => e.ExportType == "AoCCharacterMovement" || e.ExportType == "CharacterMovementComponent" || e.Name == "CharMoveComp";

    /// <summary>The movement component of a Blueprint chain, nearest first: the package's own, else the parents' (a default object is the Outer of its components).</summary>
    static UObject? MovementOf(List<UObject> cdos)
    {
        foreach (var cdo in cdos)
        {
            if (cdo.Owner == null) continue;
            foreach (var e in cdo.Owner.GetExports())
                if (e.Outer == cdo && IsMovement(e)) return e;
        }
        return null;
    }
// -- Cyb x C.O.A --

    /// <summary>The rider AnimBP the Blueprint's interaction point names, nearest Blueprint first: every InteractableComponent of the chain's packages (the Blueprint's own
    /// template, the default object's instance), the point whose PlayerInteractDefinition is Mount_Interaction (else the first point that names an AnimBP).</summary>
    static string RiderAnimOf(List<UObject> cdos)
    {
        foreach (var cdo in cdos)
        {
            if (cdo.Owner == null) continue;
            foreach (var e in cdo.Owner.GetExports())
            {
                if (!e.Name.StartsWith("InteractableComponent", StringComparison.Ordinal)) continue;
                var prop = e.Properties.FirstOrDefault(p => p.Name.Text == "InteractionPoints");
                if (prop?.Tag?.GenericValue == null) continue;
                JToken tok;
                try { tok = JToken.Parse(JsonConvert.SerializeObject(prop.Tag.GenericValue, Formatting.None, new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s, a) => a.ErrorContext.Handled = true })); }
                catch { continue; }
                if (tok is not JArray points) continue;
                string first = "";
                foreach (var pt in points)
                {
                    if (pt is not JObject point) continue;
                    var anim = point["AttachmentAnimation"] as JObject;
                    string path = anim?["ObjectPath"]?.ToString() ?? "", name = anim?["ObjectName"]?.ToString() ?? "";
                    int q1 = name.IndexOf('\''), q2 = name.LastIndexOf('\'');
                    if (path.Length == 0 || q1 < 0 || q2 <= q1) continue;
                    string klass = name.Substring(q1 + 1, q2 - q1 - 1);
                    int dot = path.LastIndexOf('.');
                    string package = dot > path.LastIndexOf('/') ? path.Substring(0, dot) : path;   // "/Game/.../ABP_PC_MNT_Swiftclaw.1": the export index goes
                    string cls = package + "." + klass;
                    if ((point["PlayerInteractDefinition"] as JObject)?["Name"]?.ToString() == "Mount_Interaction") return cls;
                    if (first.Length == 0) first = cls;
                }
                if (first.Length != 0) return first;
            }
        }
        return "";
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
            var names = new List<string>();
            var cdos = new List<UObject>();
            UObject? o = cdo;
            for (int i = 0; o != null && i < 12; i++)
            {
                cdos.Add(o);
                var n = o.Name;
                if (n.StartsWith("Default__", StringComparison.Ordinal)) n = n.Substring(9);
                names.Add(n);
                o = o.Template?.Object?.Value;
            }
            var r = new Row { Blueprint = path.Substring(Folder.Length, path.Length - Folder.Length - ".uasset".Length), Parents = string.Join(",", names.Skip(1)) };
            foreach (var kind in new[] { "Grounded", "Flying", "Aquatic", "Gliding" })
                if (names.Any(n => n.StartsWith("Mount_" + kind + "_Base", StringComparison.Ordinal))) { r.Kind = kind; break; }
            r.RiderAnim = RiderAnimOf(cdos);
            var move = MovementOf(cdos);
            if (move != null)
            {
                r.HasMovement = true;
                var defaults = new List<string>();
                if (Lv.TryProp<float>(move, "MaxWalkSpeed", out var w)) r.Walk = w; else { r.Walk = DefaultWalk; defaults.Add("walk"); }
                if (Lv.TryProp<float>(move, "MaxRunSpeed", out var ru)) r.Run = ru; else { r.Run = r.Walk; defaults.Add("run"); }
                if (Lv.TryProp<float>(move, "MaxFlySpeed", out var f)) r.Fly = f; else { r.Fly = DefaultFly; defaults.Add("fly"); }
                if (Lv.TryProp<float>(move, "MaxSwimSpeed", out var s)) r.Swim = s; else { r.Swim = DefaultSwim; defaults.Add("swim"); }
                r.Defaults = string.Join(",", defaults);
            }
            rows.Add(r);
        }
        rows.Sort((x, y) => string.CompareOrdinal(x.Blueprint, y.Blueprint));
        return rows;
    }

    public static string ToTsv(IEnumerable<Row> rows)
    {
        var sb = new StringBuilder();
        sb.Append(Header).Append('\n');
        foreach (var r in rows)
        {
            sb.Append(r.Blueprint).Append('\t').Append(r.Kind).Append('\t').Append(r.Parents).Append('\t');
            if (r.HasMovement) sb.Append(F(r.Walk)).Append('\t').Append(F(r.Run)).Append('\t').Append(F(r.Fly)).Append('\t').Append(F(r.Swim)).Append('\t').Append(r.Defaults);
            else sb.Append("\t\t\t\t");
            sb.Append('\n');
        }
        return sb.ToString();
    }
// built with CoA and C-Y-B

    public static string ToRidersTsv(IEnumerable<Row> rows)
    {
        var sb = new StringBuilder();
        sb.Append(RidersHeader).Append('\n');
        foreach (var r in rows) sb.Append(r.Blueprint).Append('\t').Append(r.RiderAnim).Append('\n');
        return sb.ToString();
    }

    static string F(double v) => v.ToString("0.###", CultureInfo.InvariantCulture);
}
