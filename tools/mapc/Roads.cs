// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Assets.Objects.Properties;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;
using Newtonsoft.Json;

namespace Mapc;

/// <summary>
/// The roads of the area (the mounts milestone, stage 4).  A road is a placed BP_RoadActor_C whose root is an IntrepidSplineComponent: the actor's data names its
/// RoadPresetRecord and the ZOI that owns it, the spline is a cubic Hermite through its points.  Only those two exports of a cell are deserialized (the export
/// map is filtered by name and by outer, the rest of the cell is never parsed), the splines are sampled every ~200 cm into XY polylines and written raw:
///
///   # roads_raw v1 ...
///   road <TAB> preset_guid <TAB> zoi_guid <TAB> active <TAB> in_node <TAB> cell <TAB> actor <TAB> x,y <TAB> x,y ...
///
/// contentc (`contentc roads`) resolves the preset's tier and width for a node level and writes the file worldd loads (dist/content/roads.tsv).
/// </summary>
public static class Roads
{
    public const string RawFileName = "roads_raw.tsv";
    public const double StepCm = 200.0;

    public sealed class Road
    {
        public string Cell = "", Actor = "";
        public ulong Preset, Zoi;
        public bool Active = true, InNode;
        public List<(double X, double Y)> Poly = new();
        public double LengthCm;
        public int Points;
    }

    public sealed class Result
    {
        public List<Road> Roads = new();
        public int CellsRead, CellsWithRoads, CellsMissing, ActorsSeen, ActorsDecoded, Duplicates;   // CellsMissing: a cell the map names and the containers do not hold (an empty cell: nothing to read)
        public List<string> Warnings = new();
        public double Seconds;
        public long PeakWorkingSetBytes;
    }

    static string GuidText(ulong v) => v.ToString(CultureInfo.InvariantCulture);
// -- C | O | A x C Y B --

    // ---- the export map, without deserializing anything ------------------------------------------------------------------------------------------

    sealed record Names(string[] Name, int[] Outer);

    static Names ReadNames(IoPackage io)
    {
        int n = io.ExportMap.Length;
        var names = new string[n];
        var outer = new int[n];
        for (int i = 0; i < n; i++)
        {
            names[i] = io.CreateFNameFromMappedName(io.ExportMap[i].ObjectName).Text;
            var o = io.ExportMap[i].OuterIndex;
            outer[i] = o.IsExport ? (int)o.AsExport : -1;
        }
        return new Names(names, outer);
    }

    // ---- reading one actor and its spline ---------------------------------------------------------------------------------------------------------

    static bool TryGuidObj(object? v, out ulong g, int depth = 0)
    {
        g = 0;
        if (v == null || depth > 6) return false;
        switch (v)
        {
            case FPropertyTagType t: return TryGuidObj(t.GenericValue, out g, depth + 1);
            case FScriptStruct ss: return TryGuidObj(ss.StructType, out g, depth + 1);
            case FStructFallback sf:
                foreach (var p in sf.Properties)
                    if (TryGuidObj(p.Tag, out g, depth + 1)) return true;
                return false;
            case ulong u: g = u; return true;
            case long l: g = unchecked((ulong)l); return true;
            default: return false;
        }
    }

    /// <summary>The first 64-bit guid a record-guid property holds, however CUE4Parse shaped it (a struct of one or two ints, a uint64, a struct with a Guid field).</summary>
    public static ulong GuidOf(UObject actor, string name)
    {
        var prop = actor.Properties.FirstOrDefault(p => p.Name.Text == name);
        if (prop?.Tag == null) return 0;
        return TryGuidObj(prop.Tag, out var g) ? g : 0;
    }

    /// <summary>The spline's world-space XY samples every StepCm: the Hermite through SplineCurves.Position.Points, transformed by the component's world transform.</summary>
    public static bool Sample(UObject spline, out List<(double X, double Y)> poly, out double length, out int points, out string why)
    {
        poly = new(); length = 0; points = 0; why = "";
        var polyL = new List<(double X, double Y)>();
        double lengthL = 0;
        var curves = spline.GetOrDefault<FStructFallback?>("SplineCurves", null);
        var pos = curves?.GetOrDefault<FStructFallback?>("Position", null);
        var pts = pos?.GetOrDefault<UScriptArray?>("Points", null);
        if (pts == null || pts.Properties.Count < 2) { why = "no SplineCurves.Position.Points"; return false; }
        var P = new List<(double X, double Y, double Z, double Ax, double Ay, double Az, double Lx, double Ly, double Lz)>();
        foreach (var item in pts.Properties)
        {
            if (item.GenericValue is not FScriptStruct ss || ss.StructType is not FStructFallback f) { why = "a spline point is no struct"; return false; }
            var o = f.GetOrDefault<FVector>("OutVal");
            var a = f.GetOrDefault<FVector>("ArriveTangent");
            var l = f.GetOrDefault<FVector>("LeaveTangent");
            P.Add((o.X, o.Y, o.Z, a.X, a.Y, a.Z, l.X, l.Y, l.Z));
        }
        points = P.Count;
        var xf = Lv.World(spline);
        double X(double x, double y, double z) => xf.M00 * x + xf.M01 * y + xf.M02 * z + xf.M03;
        double Y(double x, double y, double z) => xf.M10 * x + xf.M11 * y + xf.M12 * z + xf.M13;
        double lastX = 0, lastY = 0;
        bool first = true;
        void Emit(double x, double y)
        {
            double wx = X(x, y, 0), wy = Y(x, y, 0);
            if (!first) lengthL += Math.Sqrt((wx - lastX) * (wx - lastX) + (wy - lastY) * (wy - lastY));
            first = false;
            lastX = wx; lastY = wy;
            polyL.Add((wx, wy));
        }
        for (int i = 0; i + 1 < P.Count; i++)
        {
            var p0 = P[i]; var p1 = P[i + 1];
            // the Hermite basis of UE's FInterpCurve: a segment leaves p0 with its LeaveTangent and arrives at p1 with its ArriveTangent
            double chord = Math.Sqrt((p1.X - p0.X) * (p1.X - p0.X) + (p1.Y - p0.Y) * (p1.Y - p0.Y));
            int k = Math.Max(2, (int)Math.Ceiling(chord / StepCm));
            for (int j = 0; j < k; j++)
            {
                double s = (double)j / k, s2 = s * s, s3 = s2 * s;
                double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
                double x = h00 * p0.X + h10 * p0.Lx + h01 * p1.X + h11 * p1.Ax;
                double y = h00 * p0.Y + h10 * p0.Ly + h01 * p1.Y + h11 * p1.Ay;
                Emit(x, y);
            }
        }
        Emit(P[^1].X, P[^1].Y);
        poly = polyL;
        length = lengthL;
        return true;
    }

    // ---- one cell ----------------------------------------------------------------------------------------------------------------------------------

    public static void ReadCell(IPackage pkg, string cellId, Result res, Action<string>? log)
    {
        if (pkg is not IoPackage io) return;
        var names = ReadNames(io);
        var splines = new Dictionary<int, List<int>>();   // actor export index -> its IntrepidSplineComponent exports
        for (int i = 0; i < names.Name.Length; i++)
        {
            if (names.Name[i].StartsWith("IntrepidSplineComponent", StringComparison.Ordinal) && names.Outer[i] >= 0)
            {
                if (!splines.TryGetValue(names.Outer[i], out var l)) splines[names.Outer[i]] = l = new();
                l.Add(i);
            }
        }
        bool any = false;
        for (int i = 0; i < names.Name.Length; i++)
        {
            if (!names.Name[i].StartsWith("BP_RoadActor_C", StringComparison.Ordinal)) continue;
            any = true;
            res.ActorsSeen++;
            UObject? actor;
            try { actor = io.ExportsLazy[i].Value; }
            catch (Exception e) { res.Warnings.Add($"{cellId}: {names.Name[i]} does not deserialize: {e.Message}"); continue; }
            if (actor == null) { res.Warnings.Add($"{cellId}: {names.Name[i]} is null"); continue; }
            var road = new Road { Cell = cellId, Actor = names.Name[i] };
            road.Preset = GuidOf(actor, "RoadPreset");
            road.Zoi = GuidOf(actor, "OwnerZOI");
            road.Active = actor.GetOrDefault("bIsActiveRoad", true);
            road.InNode = actor.GetOrDefault("bIsRoadInNode", false);
            if (!splines.TryGetValue(i, out var sp) || sp.Count == 0) { res.Warnings.Add($"{cellId}: {names.Name[i]} has no IntrepidSplineComponent"); continue; }
            UObject? comp;
            try { comp = io.ExportsLazy[sp[0]].Value; }
            catch (Exception e) { res.Warnings.Add($"{cellId}: the spline of {names.Name[i]} does not deserialize: {e.Message}"); continue; }
            string why = "no component";
            if (comp == null || !Sample(comp, out var poly, out var lengthCm, out var pointCount, out why))
            {
                res.Warnings.Add($"{cellId}: the spline of {names.Name[i]}: {why}");
                continue;
            }
            road.Poly = poly;
            road.LengthCm = lengthCm;
            road.Points = pointCount;
            res.ActorsDecoded++;
            res.Roads.Add(road);
        }
        if (any) res.CellsWithRoads++;
    }

    // ---- the area ----------------------------------------------------------------------------------------------------------------------------------
// [C | O | A x Cyb]

    public static Result Collect(Game g, double cx, double cy, double radius, double margin, Action<string>? log) => Collect(g, new Scope(cx, cy, radius, margin), log);

    /// <summary>The MainGrid cells the scope touches (E1's circle, or the Riverlands' rectangles plus the margin), one at a time.</summary>
    public static Result Collect(Game g, Scope scope, Action<string>? log)
    {
        var sw = Stopwatch.StartNew();
        var res = new Result();
        var ids = Cells.Hit(g, scope, out var grids).Where(id => grids[id] == "MainGrid").ToList();
        log?.Invoke($"{ids.Count} MainGrid cells for {scope.Describe()}");
        var proc = Process.GetCurrentProcess();
        foreach (var id in ids)
        {
            string path = Cells.PathOf(id);
            var pkg = g.Load(path);
            try
            {
                if (pkg == null) { res.CellsMissing++; continue; }
                res.CellsRead++;
                ReadCell(pkg, id, res, log);
            }
            finally
            {
                g.Forget(path);
                proc.Refresh();
                res.PeakWorkingSetBytes = Math.Max(res.PeakWorkingSetBytes, proc.PeakWorkingSet64);
            }
        }
        // a road in two neighbouring cells is one road: the same preset, zone and first and last sample
        var seen = new HashSet<string>();
        var kept = new List<Road>();
        foreach (var r in res.Roads.OrderBy(r => r.Preset).ThenBy(r => r.Zoi).ThenBy(r => Math.Round(r.Poly[0].X)).ThenBy(r => Math.Round(r.Poly[0].Y)).ThenBy(r => r.Cell, StringComparer.Ordinal))
        {
            string key = $"{r.Preset}/{r.Zoi}/{Math.Round(r.Poly[0].X)}/{Math.Round(r.Poly[0].Y)}/{Math.Round(r.Poly[^1].X)}/{Math.Round(r.Poly[^1].Y)}";
            if (!seen.Add(key)) { res.Duplicates++; continue; }
            kept.Add(r);
        }
        res.Roads = kept;
        res.Seconds = sw.Elapsed.TotalSeconds;
        return res;
    }

    public static string ToTsv(Result res, double cx, double cy, double radius, double margin) => ToTsv(res, new Scope(cx, cy, radius, margin));

    public static string ToTsv(Result res, Scope scope)
    {
        var sb = new StringBuilder();
        var ic = CultureInfo.InvariantCulture;
        if (scope.Region != null)
            sb.Append("# roads_raw v1: the BP_RoadActor_C splines of the MainGrid cells that touch the region (").Append(scope.Region.Rects.Count.ToString(ic)).Append(" rectangles, hash ")
              .Append(scope.Region.Hash.ToString("X16", ic)).Append(") grown by ").Append(scope.Margin.ToString("F0", ic)).Append(" cm (tools/mapc: mapc roads). Samples every ")
              .Append(StepCm.ToString("F0", ic)).Append(" cm, world cm.\n");
        else
            sb.Append("# roads_raw v1: the BP_RoadActor_C splines of the MainGrid cells around (").Append(scope.Cx.ToString("F1", ic)).Append(", ").Append(scope.Cy.ToString("F1", ic)).Append("), ")
              .Append((scope.Radius + scope.Margin).ToString("F0", ic)).Append(" cm (tools/mapc: mapc roads). Samples every ").Append(StepCm.ToString("F0", ic)).Append(" cm, world cm.\n");
        sb.Append("# road\tpreset_guid\tzoi_guid\tactive\tin_node\tcell\tactor\tx,y ...\n");
        foreach (var r in res.Roads)
        {
            sb.Append("road\t").Append(GuidText(r.Preset)).Append('\t').Append(GuidText(r.Zoi)).Append('\t').Append(r.Active ? '1' : '0').Append('\t').Append(r.InNode ? '1' : '0')
              .Append('\t').Append(r.Cell).Append('\t').Append(r.Actor);
            foreach (var (x, y) in r.Poly) sb.Append('\t').Append(x.ToString("F1", ic)).Append(',').Append(y.ToString("F1", ic));
            sb.Append('\n');
        }
        return sb.ToString();
    }

    // ---- diagnostics ---------------------------------------------------------------------------------------------------------------------------------

    /// <summary>mapc roads --probe CELLID: what one cell's road actors and splines hold, as CUE4Parse reads them.</summary>
    public static void Probe(Game g, string cellId)
    {
        var pkg = g.Load(Cells.PathOf(cellId));
        if (pkg is not IoPackage io) { Console.WriteLine("cell not loadable"); return; }
        var names = ReadNames(io);
        for (int i = 0; i < names.Name.Length; i++)
        {
            if (!names.Name[i].StartsWith("BP_RoadActor_C", StringComparison.Ordinal)) continue;
            var actor = io.ExportsLazy[i].Value;
            Console.WriteLine($"-- actor {names.Name[i]} ({actor.ExportType}) props: {string.Join(",", actor.Properties.Select(p => p.Name.Text))}");
            foreach (var p in actor.Properties)
            {
                if (p.Name.Text is "RoadPreset" or "OwnerZOI" or "bIsActiveRoad" or "bIsRoadInNode" or "bIsStartIndexConnected" or "bIsEndIndexConnected")
                    Console.WriteLine($"   {p.Name.Text}: {p.Tag?.GetType().Name} {Short(p.Tag?.GenericValue)}");
            }
            Console.WriteLine($"   preset {GuidOf(actor, "RoadPreset")} zoi {GuidOf(actor, "OwnerZOI")}");
            for (int j = 0; j < names.Name.Length; j++)
            {
                if (names.Outer[j] != i || !names.Name[j].StartsWith("IntrepidSplineComponent", StringComparison.Ordinal)) continue;
                var comp = io.ExportsLazy[j].Value;
                Console.WriteLine($"   spline {names.Name[j]} props: {string.Join(",", comp.Properties.Select(p => p.Name.Text))}");
                if (Sample(comp, out var poly, out var len, out var pts, out var why)) Console.WriteLine($"   {pts} points, {poly.Count} samples, {len:F0} cm, first ({poly[0].X:F0}, {poly[0].Y:F0}) last ({poly[^1].X:F0}, {poly[^1].Y:F0})");
                else Console.WriteLine($"   not sampled: {why}");
            }
        }
    }

    static string Short(object? o)
    {
        try { var s = JsonConvert.SerializeObject(o, new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true }); return s.Length > 300 ? s[..300] + "..." : s; }
        catch { return o?.ToString() ?? "null"; }
    }
}
