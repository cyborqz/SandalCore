// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.UObject;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// What a humanoid Blueprint says about how its people look and are called (the class defaults the server uses when the population data names no look of its own):
/// one line per Blueprint of the pawn kind whose class defaults, or its parents', set an NPCAppearanceRecordId, in path order:
///   archetype  appearance  name  race  gender  info_name
/// `archetype` is the Blueprint's default object as npc_motion.tsv names it, `appearance` the NPCAppearance record guid, `name` the name the Blueprint carries
/// ("" when it has none: its CharacterName, else its NPCName), `race` and `gender` the ECharacterRace and ECharacterGender names its BaseCharacterInfo sets ("" when neither it nor a parent
/// sets one: the default, Kaelar and Male), `info_name` the CharacterName its BaseCharacterInfo component carries itself (the open leaves the name out when it equals that).
/// </summary>
public static class Looks
{
    public const string File = "npc_looks.tsv";
    public const string Header = "archetype\tappearance\tname\trace\tgender\tinfo_name";

    /// <summary>
    /// What each pawn Blueprint's state tree has tasks for (npc_trees.tsv): one line per Blueprint that plays a tree, in path order:
    ///   archetype  tree  wander  patrol
    /// `tree` is the tree asset the Blueprint plays (its own reference, else the nearest parent's), `wander` and `patrol` are 1 when the tree (or a tree it links to) has a task that
    /// wanders or patrols, 0 when it has none.  A creature can only do what its tree has states for: a tree with neither lets the NPC stand at its post whatever its population
    /// instance says (the quest givers').
    /// </summary>
    public const string TreesFile = "npc_trees.tsv";
    public const string TreesHeader = "archetype\ttree\twander\tpatrol";

    /// <summary>The class chain of a default object: its parents' default objects, nearest first (each is the archetype of the next).</summary>
    static List<UObject> Chain(UObject cdo, out bool pawn)
    {
        var cdos = new List<UObject>();
        pawn = false;
        UObject? o = cdo;
        for (int i = 0; o != null && i < 12; i++)
        {
            cdos.Add(o);
            var n = o.Name;
            if (n.StartsWith("Default__", StringComparison.Ordinal)) n = n.Substring(9);
            if (n == "NPCPawn_C") pawn = true;
            o = o.Template?.Object?.Value;
        }
        return cdos;
    }

    /// <summary>The enum value a property of a component holds, searched along the component's archetype chain ("Female" of ECharacterGender::Female); "" when none sets it.</summary>
    static string EnumOf(UObject? comp, string name)
    {
        for (int depth = 0; comp != null && depth < 12; depth++)
        {
            var prop = comp.Properties.FirstOrDefault(p => p.Name.Text == name);
            if (prop?.Tag?.GenericValue != null)
            {
                var s = prop.Tag.GenericValue.ToString() ?? "";
                int i = s.LastIndexOf("::", StringComparison.Ordinal);
                return i >= 0 ? s.Substring(i + 2) : s;
            }
            comp = comp.Template?.Object?.Value;
        }
        return "";
    }

    /// <summary>A string property of a component, searched along its archetype chain ("" when none sets it).</summary>
    static string StringOf(UObject? comp, string name)
    {
        for (int depth = 0; comp != null && depth < 12; depth++)
        {
            if (comp.TryGetValue(out string s, name) && s != null) return s;
            comp = comp.Template?.Object?.Value;
        }
        return "";
    }

    static string Flat(string s) => s.Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ').Trim();

    public static string Collect(Game g, out int packages, out int failed, out int rows, out string trees, out int treeRows)
    {
        var lines = new List<(string Archetype, string Line)>();
        var treeLines = new List<(string Archetype, string Line)>();
        var treeCache = new Dictionary<string, Reader.TreeKind?>();
        packages = 0; failed = 0;
        foreach (var path in NpcBlueprints.AllPaths(g))
        {
            packages++;
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { failed++; continue; }
            if (pkg == null) { failed++; continue; }
            UObject? cdo = null;
            foreach (var e in pkg.GetExports())
                if (e.Name.StartsWith("Default__", StringComparison.Ordinal) && e.Name.EndsWith("_C", StringComparison.Ordinal)) { cdo = e; break; }
            if (cdo == null) continue;
            var cdos = Chain(cdo, out bool pawn);
            if (!pawn) continue;
            string pkgPath = "/Game/" + path.Substring("AOC/Content/".Length, path.Length - "AOC/Content/".Length - ".uasset".Length);
            string archetype = pkgPath + "." + cdo.Name;
            // what its state tree has tasks for (every pawn Blueprint, humanoid or not)
            var kind = Reader.TreeKindOf(g, cdo, treeCache);
            if (kind != null) treeLines.Add((archetype, archetype + "\t" + kind.Tree + "\t" + (kind.Wander ? "1" : "0") + "\t" + (kind.Patrol ? "1" : "0")));
            long appearance = 0;
            foreach (var c in cdos)
            {
                if (c.TryGetValue(out FStructFallback st, "NPCAppearanceRecordId") && st != null)
                {
                    if (st.TryGetValue(out long gl, "Guid")) appearance = gl;
                    else if (st.TryGetValue(out ulong gu, "Guid")) appearance = (long)gu;
                    break;
                }
            }
            if (appearance == 0) continue;
            // the name the Blueprint gives its people: the nearest class default that sets a CharacterName (else an NPCName; an empty one counts, it overrides a parent's), "0" = none.
            // The server copies it into the NPC's info, and the open carries it only when it differs from the info component's own default (`info_name`).
            string name = "";
            foreach (var c in cdos)
            {
                if (c.TryGetValue(out string nm, "CharacterName") && nm != null) { name = nm; break; }
                if (c.TryGetValue(out string np, "NPCName") && np != null) { name = np; break; }
            }
            if (name.Trim() == "0") name = "";
            UObject? info = null;
            foreach (var e in pkg.GetExports())
                if (e.Outer == cdo && e.Name == "BaseCharacterInfo") { info = e; break; }
            string race = EnumOf(info, "Race"), gender = EnumOf(info, "Gender");
            string infoName = StringOf(info, "CharacterName");
            name = Flat(name);
            infoName = Flat(infoName);
            lines.Add((archetype, archetype + "\t" + appearance.ToString(CultureInfo.InvariantCulture) + "\t" + name + "\t" + race + "\t" + gender + "\t" + infoName));
        }
        lines.Sort((x, y) => string.CompareOrdinal(x.Archetype, y.Archetype));
        rows = lines.Count;
        var sb = new StringBuilder();
        sb.Append(Header).Append('\n');
        foreach (var (_, line) in lines) sb.Append(line).Append('\n');
        treeLines.Sort((x, y) => string.CompareOrdinal(x.Archetype, y.Archetype));
        treeRows = treeLines.Count;
        var tb = new StringBuilder();
        tb.Append(TreesHeader).Append('\n');
        foreach (var (_, line) in treeLines) tb.Append(line).Append('\n');
        trees = tb.ToString();
        return sb.ToString();
    }
}
