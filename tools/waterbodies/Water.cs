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
using Mapc;

namespace WaterBodies;

/// <summary>
/// The water a line can be cast into (fishing, milestone 1).  The map's Water grid holds the lakes (BP_WaterLake_C) and rivers (BP_WaterSpline_C) as actors.  A body that
/// names fishing data (a GatheringInteractionData: the rod, the certification, the length of a catch and the reward table of that water) can be fished; the others cannot.
/// An actor holds the swim volumes the game generated for it (SwimVolumeTransforms: boxes of a unit of 100 x 100 x 50 cm, in the actor's own frame), and a lake a closed
/// spline, its outline (a Hermite curve through the points, as UE draws it).
///
/// `waterbodies read` writes dist/content/fishing_water.tsv:
///
///   # fishing_water v1 ...
///   body <TAB> id <TAB> class <TAB> actor <TAB> fishing_guid <TAB> x <TAB> y <TAB> z                  the actor's place (cm): z is the surface of a lake
///   outline <TAB> id <TAB> x,y <TAB> x,y ...                                                          a lake's outline, world cm, closed (the first point is not repeated)
///   box <TAB> id <TAB> cx <TAB> cy <TAB> cz <TAB> hx <TAB> hy <TAB> hz <TAB> yaw                     a swim volume: its centre, half sizes (cm) and yaw (degrees)
///
/// worldd answers "is this point on water, and which fishing data does it name" from the outline of a lake and from the volumes of a river.
/// </summary>
public static class Water
{
    public const string FileName = "fishing_water.tsv";
    public const double StepCm = 200.0;

    public sealed record Box(double Cx, double Cy, double Cz, double Hx, double Hy, double Hz, double YawDeg);

    public sealed class Body
    {
        public string Cell = "", Actor = "", Class = "";
        public ulong Fishing;
        public double X, Y, Z;
        public List<(double X, double Y)> Outline = new();
        public List<Box> Boxes = new();
    }

    public sealed class Stats
    {
        public int Cells, Unreadable, Actors, Lakes, Rivers;
        public SortedDictionary<ulong, int> FishingData = new();
        public List<string> Warnings = new();
    }

    static readonly string[] Classes = { "BP_WaterLake_C", "BP_ProcLake_C", "BP_WaterSpline_C", "BP_ProcSpline_C" };

    public static List<Body> Collect(Game g, Scope scope, out Stats stats)
    {
        stats = new Stats();
        var found = new List<Body>();
        var cells = Cells.Describe(g, scope).Where(c => c.Grid == "Water" && c.Size > 0).OrderBy(c => c.Id, StringComparer.Ordinal).ToList();
        foreach (var c in cells)
        {
            string path = Cells.PathOf(c.Id);
            var pkg = g.Load(path);
            try
            {
                if (pkg is not IoPackage io) { stats.Unreadable++; continue; }
                stats.Cells++;
                ReadCell(io, c.Id, found, stats);
            }
            finally { g.Forget(path); }
        }
        foreach (var w in stats.Warnings) Console.WriteLine("  warning: " + w);
        // the same order whatever the order the cells were read in
        return found.OrderBy(b => b.Actor, StringComparer.Ordinal).ThenBy(b => b.Cell, StringComparer.Ordinal).ToList();
    }

    static void ReadCell(IoPackage io, string cell, List<Body> found, Stats stats)
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
        for (int i = 0; i < n; i++)
        {
            string cls = Classes.FirstOrDefault(k => names[i].StartsWith(k, StringComparison.Ordinal)) ?? "";
            if (cls == "") continue;
            stats.Actors++;
            UObject? actor;
            try { actor = io.ExportsLazy[i].Value; }
            catch (Exception e) { stats.Warnings.Add($"{cell}: {names[i]} does not deserialize: {e.Message}"); continue; }
            if (actor == null) { stats.Warnings.Add($"{cell}: {names[i]} is null"); continue; }
            bool river = cls.EndsWith("Spline_C", StringComparison.Ordinal);
            if (river) stats.Rivers++; else stats.Lakes++;
            ulong fishing = Roads.GuidOf(actor, "FishingData");
            if (fishing == 0) continue;   // water nobody can fish
            var body = new Body { Cell = cell, Actor = names[i], Class = river ? "river" : "lake", Fishing = fishing };
            // the actor's frame
            var rootIndex = actor.GetOrDefault<FPackageIndex>("RootComponent");
            var root = Lv.Resolve(rootIndex);
            if (root == null) { stats.Warnings.Add($"{cell}: {names[i]} has no root component"); continue; }
            var frame = Lv.World(root);
            var origin = frame.Apply(0, 0, 0);
            body.X = origin.x; body.Y = origin.y; body.Z = origin.z;
            // the swim volumes
            if (actor.GetOrDefault<UScriptArray?>("SwimVolumeTransforms", null) is { } volumes)
            {
                foreach (var item in volumes.Properties)
                {
                    if (item.GenericValue is not FScriptStruct ss) continue;
                    FTransform t;
                    if (ss.StructType is FTransform direct) t = direct;
                    else if (ss.StructType is FStructFallback f) t = new FTransform(f.GetOrDefault<FQuat>("Rotation"), f.GetOrDefault<FVector>("Translation"), f.GetOrDefault<FVector>("Scale3D", FVector.OneVector));
                    else continue;
                    var local = Xf.FromTRS(t.Translation.X, t.Translation.Y, t.Translation.Z, (t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W), t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z);
                    var world = frame.Mul(local);
                    var c = world.Apply(0, 0, 0);
                    // the game's swimming volume is a box of half sizes 50 x 50 x 25 cm at scale 1
                    body.Boxes.Add(new Box(c.x, c.y, c.z, 50.0 * world.ScaleX, 50.0 * world.ScaleY, 25.0 * world.ScaleZ, Math.Atan2(world.M10, world.M00) * 180.0 / Math.PI));
                }
            }
            // a lake's outline: the closed spline
            if (!river)
            {
                var spline = FindChild(io, outer, names, i, "Spline");
                string? why = "no Spline component";
                if (spline != null && Outline(spline, out var outline, out why)) body.Outline = outline;
                else stats.Warnings.Add($"{cell}: {names[i]} has no outline ({why}): its swim volumes stand for it");
            }
            if (body.Boxes.Count == 0 && body.Outline.Count == 0) { stats.Warnings.Add($"{cell}: {names[i]} names fishing data but has neither swim volumes nor an outline: left out"); continue; }
            stats.FishingData[fishing] = stats.FishingData.GetValueOrDefault(fishing) + 1;
            found.Add(body);
        }
    }

    static UObject? FindChild(IoPackage io, int[] outer, string[] names, int parent, string name)
    {
        for (int j = 0; j < names.Length; j++)
            if (outer[j] == parent && names[j] == name)
            {
                try { return io.ExportsLazy[j].Value; } catch { return null; }
            }
        return null;
    }

    /// <summary>The closed spline's outline in world XY: the Hermite through SplineCurves.Position.Points (a segment leaves a point with its LeaveTangent and arrives at the next with its
    /// ArriveTangent), the last point joined to the first, a point about every StepCm, transformed by the component's world transform.</summary>
    static bool Outline(UObject spline, out List<(double X, double Y)> poly, out string? why)
    {
        poly = new(); why = null;
        var curves = spline.GetOrDefault<FStructFallback?>("SplineCurves", null);
        var pos = curves?.GetOrDefault<FStructFallback?>("Position", null);
        var pts = pos?.GetOrDefault<UScriptArray?>("Points", null);
        if (pts == null || pts.Properties.Count < 3) { why = "no SplineCurves.Position.Points"; return false; }
        var P = new List<(FVector Out, FVector Arrive, FVector Leave)>();
        foreach (var item in pts.Properties)
        {
            if (item.GenericValue is not FScriptStruct ss || ss.StructType is not FStructFallback f) { why = "a spline point is no struct"; return false; }
            P.Add((f.GetOrDefault<FVector>("OutVal"), f.GetOrDefault<FVector>("ArriveTangent"), f.GetOrDefault<FVector>("LeaveTangent")));
        }
        var xf = Lv.World(spline);
        for (int i = 0; i < P.Count; i++)
        {
            var p0 = P[i]; var p1 = P[(i + 1) % P.Count];
            double chord = Math.Sqrt((p1.Out.X - p0.Out.X) * (p1.Out.X - p0.Out.X) + (p1.Out.Y - p0.Out.Y) * (p1.Out.Y - p0.Out.Y));
            int k = Math.Max(4, (int)Math.Ceiling(chord / StepCm));
            for (int j = 0; j < k; j++)
            {
                double s = (double)j / k, s2 = s * s, s3 = s2 * s;
                double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
                double x = h00 * p0.Out.X + h10 * p0.Leave.X + h01 * p1.Out.X + h11 * p1.Arrive.X;
                double y = h00 * p0.Out.Y + h10 * p0.Leave.Y + h01 * p1.Out.Y + h11 * p1.Arrive.Y;
                var w = xf.Apply(x, y, 0);
                poly.Add((w.x, w.y));
            }
        }
        return true;
    }

    /// <summary>waterbodies dump CELL NAME-PART [CHARS]: every export of the cell whose name holds NAME-PART, with its properties (diagnostic).</summary>
    public static int Dump(Game g, string cell, string part, int chars)
    {
        var pkg = g.Load(Cells.PathOf(cell));
        if (pkg == null) { Console.WriteLine("cell not loadable"); return 1; }
        foreach (var e in pkg.GetExports())
        {
            if (!e.Name.Contains(part, StringComparison.Ordinal) && !e.ExportType.Contains(part, StringComparison.Ordinal)) continue;
            Console.WriteLine($"-- {e.ExportType} {e.Name} (outer {e.Outer?.Name} {e.Outer?.ExportType})");
            try
            {
                var js = Newtonsoft.Json.JsonConvert.SerializeObject(e.Properties.ToDictionary(p => p.Name.Text, p => p.Tag?.GenericValue), Newtonsoft.Json.Formatting.None,
                    new Newtonsoft.Json.JsonSerializerSettings { ReferenceLoopHandling = Newtonsoft.Json.ReferenceLoopHandling.Ignore, Error = (s, a) => a.ErrorContext.Handled = true });
                Console.WriteLine(js.Length > chars ? js[..chars] + "..." : js);
            }
            catch (Exception ex) { Console.WriteLine("(props: " + ex.Message + ")"); }
        }
        return 0;
    }

    static string N(double v) => v.ToString("F2", CultureInfo.InvariantCulture);

    public static string ToTsv(IReadOnlyList<Body> bodies)
    {
        var sb = new StringBuilder();
        sb.Append("# fishing_water v1: the lakes and rivers of the Water grid's cells that touch the Riverlands and name fishing data (tools/waterbodies: waterbodies read). World cm.\n");
        sb.Append("# body\tid\tclass\tactor\tfishing_guid\tx\ty\tz\n# outline\tid\tx,y ...\n# box\tid\tcx\tcy\tcz\thx\thy\thz\tyaw\n");
        int id = 0;
        foreach (var b in bodies)
        {
            sb.Append("body\t").Append(id).Append('\t').Append(b.Class).Append('\t').Append(b.Actor).Append('\t').Append(b.Fishing.ToString(CultureInfo.InvariantCulture)).Append('\t')
              .Append(N(b.X)).Append('\t').Append(N(b.Y)).Append('\t').Append(N(b.Z)).Append('\n');
            if (b.Outline.Count > 0)
            {
                sb.Append("outline\t").Append(id);
                foreach (var (x, y) in b.Outline) sb.Append('\t').Append(N(x)).Append(',').Append(N(y));
                sb.Append('\n');
            }
            foreach (var x in b.Boxes)
                sb.Append("box\t").Append(id).Append('\t').Append(N(x.Cx)).Append('\t').Append(N(x.Cy)).Append('\t').Append(N(x.Cz)).Append('\t').Append(N(x.Hx)).Append('\t').Append(N(x.Hy)).Append('\t')
                  .Append(N(x.Hz)).Append('\t').Append(N(x.YawDeg)).Append('\n');
            id++;
        }
        return sb.ToString();
    }
}
