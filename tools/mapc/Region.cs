// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;

namespace Mapc;

/// <summary>
/// The Riverlands as rectangles (G1 stage 1): <c>tools/mapc/region_rects.tsv</c>, written by <c>tools/capture/gen_region.py</c> from the exact ZOI footprint (401 disjoint
/// rectangles [x0,x1) x [y0,y1), world cm).  mapc reads the cells and placements that touch the footprint plus its margin.
/// </summary>
public sealed class Region
{
    public readonly List<(double X0, double Y0, double X1, double Y1)> Rects = new();
    public double MinX = double.MaxValue, MinY = double.MaxValue, MaxX = double.MinValue, MaxY = double.MinValue;
    public string Source = "";
    public ulong Hash;       // FNV-1a of the rectangles' text: which footprint a file was cut for

    public const string FileName = "region_rects.tsv";

    /// <summary>Where the file is when no --region is given: next to the project's sources (mapc is run from the repository root), else beside the program.</summary>
    public static string DefaultPath()
    {
        var a = Path.Combine("tools", "mapc", FileName);
        if (File.Exists(a)) return a;
        var b = Path.Combine(AppContext.BaseDirectory, FileName);
        if (File.Exists(b)) return b;
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            var c = Path.Combine(d.FullName, "tools", "mapc", FileName);
            if (File.Exists(c)) return c;
        }
        return a;
    }

    public static Region Load(string path)
    {
        if (!File.Exists(path)) throw new FileNotFoundException("the region file " + path + " is missing (python tools/capture/gen_region.py writes it)");
        var r = new Region { Source = path };
        ulong h = 1469598103934665603ul;
        foreach (var line in File.ReadAllLines(path))
        {
            if (line.StartsWith("rect\t", StringComparison.Ordinal))
            {
                var p = line.Split('\t');
                double x0 = double.Parse(p[1], CultureInfo.InvariantCulture), y0 = double.Parse(p[2], CultureInfo.InvariantCulture);
                double x1 = double.Parse(p[3], CultureInfo.InvariantCulture), y1 = double.Parse(p[4], CultureInfo.InvariantCulture);
                if (!(x1 > x0 && y1 > y0)) throw new InvalidDataException(path + ": an empty rectangle: " + line);
                r.Rects.Add((x0, y0, x1, y1));
                r.MinX = Math.Min(r.MinX, x0); r.MinY = Math.Min(r.MinY, y0); r.MaxX = Math.Max(r.MaxX, x1); r.MaxY = Math.Max(r.MaxY, y1);
                foreach (char c in line) { h ^= c; h *= 1099511628211ul; }
            }
        }
        if (r.Rects.Count == 0) throw new InvalidDataException(path + " holds no rectangles");
        r.Hash = h;
        return r;
    }

    /// <summary>The smallest distance from (x, y) to the union of the rectangles (0 inside).</summary>
    public double DistanceTo(double x, double y, double limit = double.MaxValue)
    {
        double best = double.MaxValue;
        foreach (var (x0, y0, x1, y1) in Rects)
        {
            double dx = Math.Max(Math.Max(x0 - x, 0), x - x1), dy = Math.Max(Math.Max(y0 - y, 0), y - y1);
            if (dx > limit || dy > limit) continue;
            double d = Math.Sqrt(dx * dx + dy * dy);
            if (d < best) { best = d; if (best == 0) return 0; }
        }
        return best;
    }
// Cyb with C | O | A

    /// <summary>Whether the box [minX,maxX] x [minY,maxY] touches a rectangle grown by <paramref name="margin"/> on every side.</summary>
    public bool BoxHits(double minX, double minY, double maxX, double maxY, double margin)
    {
        if (maxX < MinX - margin || minX > MaxX + margin || maxY < MinY - margin || minY > MaxY + margin) return false;
        foreach (var (x0, y0, x1, y1) in Rects)
            if (minX < x1 + margin && maxX > x0 - margin && minY < y1 + margin && maxY > y0 - margin) return true;
        return false;
    }

    public double AreaKm2() => Rects.Sum(q => (q.X1 - q.X0) * (q.Y1 - q.Y0)) / 1e10;
}

/// <summary>
/// What the map read is cut for: E1's circle (--center/--radius: the old mode, kept for the E1 tests) or the Riverlands' rectangles.  One margin grows either
/// (the geometry margin: a cell or a placement within it of the area is read, so that the navmesh's tiles at the border have what they need).
/// </summary>
public sealed class Scope
{
    public readonly Region? Region;
    public readonly double Cx, Cy, Radius, Margin;
    public bool IsRegion => Region != null;

    public Scope(double cx, double cy, double radius, double margin) { Cx = cx; Cy = cy; Radius = radius; Margin = margin; }
    public Scope(Region region, double cx, double cy, double radius, double margin) : this(cx, cy, radius, margin) { Region = region; }

    /// <summary>The point stands in the area itself (no margin): in a rectangle, or inside the circle.</summary>
    public bool Contains(double x, double y)
    {
        if (Region != null)
        {
            if (x < Region.MinX || x >= Region.MaxX || y < Region.MinY || y >= Region.MaxY) return false;
            foreach (var (x0, y0, x1, y1) in Region.Rects) if (x >= x0 && x < x1 && y >= y0 && y < y1) return true;
            return false;
        }
        double dx = x - Cx, dy = y - Cy;
        return dx * dx + dy * dy <= Radius * Radius;
    }

    /// <summary>A streaming cell's content bounds touch the area (the circle's square, or the rectangles) grown by the margin.</summary>
    public bool CellHit(double minX, double minY, double maxX, double maxY)
    {
        if (Region != null) return Region.BoxHits(minX, minY, maxX, maxY, Margin);
        double R = Radius + Margin;
        return minX < Cx + R && maxX > Cx - R && minY < Cy + R && maxY > Cy - R;
    }

    /// <summary>A component whose origin is this far outside the area (plus its own size, tested later) is not worth loading.</summary>
    public bool CullFar(double x, double y, double slack)
    {
        if (Region != null) return Region.DistanceTo(x, y, Margin + slack) > Margin + slack;
        double R = Radius + Margin;
        return Math.Abs(x - Cx) > R + slack || Math.Abs(y - Cy) > R + slack;
    }

    /// <summary>A placement whose origin stands at (x, y) with a reach of <paramref name="reach"/> is within the margin of the area.</summary>
    public bool PlacementIn(double x, double y, double reach)
    {
        if (Region != null) return Region.DistanceTo(x, y, Margin + reach) <= Margin + reach;
        double dx = x - Cx, dy = y - Cy;
        return Math.Sqrt(dx * dx + dy * dy) <= Radius + Margin + reach;
    }

    public string Describe() => Region != null
        ? $"the region {Region.Source}: {Region.Rects.Count} rectangles, {Region.AreaKm2():F2} km2, bounding box x {Region.MinX:F0}..{Region.MaxX:F0}, y {Region.MinY:F0}..{Region.MaxY:F0}, margin {Margin:F0} cm"
        : $"the circle of {Radius:F0} cm around ({Cx:F1}, {Cy:F1}), margin {Margin:F0} cm";
}
