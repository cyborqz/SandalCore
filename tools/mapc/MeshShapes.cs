// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.PhysicsEngine;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>A static mesh's pawn-blocking geometry as indexed triangles in the mesh's own space. Winding: the cross product (b-a) x (c-a) of
/// a triangle points out of the solid (UE coordinates, x forward, y right, z up, used as plain numbers).</summary>
public sealed class MeshGeo
{
    public string Name = "";
    public string Kind = "";            // "simple", "complex" or "none"
    public List<float> V = new();       // x y z ...
    public List<int> I = new();         // a b c ...
    public double MinX = 1e30, MinY = 1e30, MinZ = 1e30, MaxX = -1e30, MaxY = -1e30, MaxZ = -1e30;
    public int Shapes;
    public int HugeSkipped;              // simple shapes dropped for being larger than MeshShapes.MaxShapeCm in some direction (see there)
    public string DefaultProfile = "";   // the body setup's DefaultInstance collision profile: what a component that says "Default" uses
    public string DefaultEnabled = "";

    public int Tris => I.Count / 3;

    public int AddV(double x, double y, double z)
    {
        V.Add((float)x); V.Add((float)y); V.Add((float)z);
        if (x < MinX) MinX = x; if (y < MinY) MinY = y; if (z < MinZ) MinZ = z;
        if (x > MaxX) MaxX = x; if (y > MaxY) MaxY = y; if (z > MaxZ) MaxZ = z;
        return V.Count / 3 - 1;
    }

    public void AddTri(int a, int b, int c) { I.Add(a); I.Add(b); I.Add(c); }

    public void AddQuad(int a, int b, int c, int d) { AddTri(a, b, c); AddTri(a, c, d); }

    /// <summary>The signed volume under the winding convention (positive: the faces point out of the solid).</summary>
    public double SignedVolume()
    {
        double v = 0;
        for (int i = 0; i + 2 < I.Count; i += 3)
        {
            int a = I[i] * 3, b = I[i + 1] * 3, c = I[i + 2] * 3;
            v += V[a] * (V[b + 1] * V[c + 2] - V[b + 2] * V[c + 1]) - V[a + 1] * (V[b] * V[c + 2] - V[b + 2] * V[c]) + V[a + 2] * (V[b] * V[c + 1] - V[b + 1] * V[c]);
        }
        return v / 6.0;
    }
}

public static class MeshShapes
{
    /// <summary>True when the winding convention of UE's render index buffers is the opposite of ours (the cross product of a front face points in): set by a self-check, see Orientation.</summary>
    public static bool RenderWindingFlipped = true;
// C-O-A with Cyb

    /// <summary>
    /// No collision shape of a placed prop is larger than this in any direction (a kilometre).  The first whole-Riverlands read found one that is: the box in the BodySetup of
    /// `ConstructionPiece_HuAela_Mod_Bridge_Segment_1` is 2,097,150 x 2,097,153 x 6,809 cm (2^21: a default or an editor-only element; the mesh itself is 20 m across and the Chaos data
    /// cooked into the game is a set of 173 convex hulls).  Placed five times in the ruins east of Aela it made every navmesh tile within ten kilometres see a wall of 130 km^2.
    /// Such an element is left out and counted (`meshes.hugeShapesSkipped`); E1's area holds none.
    /// </summary>
    public const double MaxShapeCm = 100000.0;

    public static MeshGeo Build(UStaticMesh sm, string name, UBodySetup? bs)
    {
        var g = new MeshGeo { Name = name };
        if (bs?.GetOrDefault<FStructFallback?>("DefaultInstance", null) is { } di)
        {
            g.DefaultProfile = di.GetOrDefault<FName>("CollisionProfileName").Text;
            if (g.DefaultProfile == "None") g.DefaultProfile = "";
            g.DefaultEnabled = di.Properties.FirstOrDefault(p => p.Name.Text == "CollisionEnabled")?.Tag?.GenericValue?.ToString() ?? "";
        }
        string flag = bs?.Properties.FirstOrDefault(p => p.Name.Text == "CollisionTraceFlag")?.Tag?.GenericValue?.ToString() ?? "";
        bool complexAsSimple = flag.Contains("ComplexAsSimple", StringComparison.Ordinal);
        bool addedComplex = false;
        if (complexAsSimple && sm.RenderData?.LODs is { Length: > 0 } lods)
        {
            int li = Math.Clamp(sm.LODForCollision, 0, lods.Length - 1);
            if (lods[li].SkipLod && li != 0) li = 0;
            addedComplex = AddRenderLod(g, lods[li]);
            if (addedComplex) g.Kind = "complex";
        }
        if (!addedComplex && bs?.AggGeom != null)
        {
            var a = bs.AggGeom;
            foreach (var e in a.BoxElems) if (e.CollisionEnabled != ECollisionEnabled.NoCollision) { if (Math.Max(e.X, Math.Max(e.Y, e.Z)) > MaxShapeCm) g.HugeSkipped++; else { AddBox(g, e); g.Shapes++; } }
            foreach (var e in a.SphereElems) if (e.CollisionEnabled != ECollisionEnabled.NoCollision) { if (e.Radius * 2 > MaxShapeCm) g.HugeSkipped++; else { AddSphere(g, e.Center, e.Radius); g.Shapes++; } }
            foreach (var e in a.SphylElems) if (e.CollisionEnabled != ECollisionEnabled.NoCollision) { if (e.Length + e.Radius * 2 > MaxShapeCm) g.HugeSkipped++; else { AddCapsule(g, e); g.Shapes++; } }
            foreach (var e in a.TaperedCapsuleElems) if (e.CollisionEnabled != ECollisionEnabled.NoCollision) { if (e.Length + Math.Max(e.Radius0, e.Radius1) * 2 > MaxShapeCm) g.HugeSkipped++; else { AddTaperedCapsule(g, e); g.Shapes++; } }
            foreach (var e in a.ConvexElems) if (e.CollisionEnabled != ECollisionEnabled.NoCollision) { if (AddConvex(g, e)) g.Shapes++; }
            if (g.Shapes > 0) g.Kind = "simple";
        }
        if (g.Kind == "") g.Kind = "none";
        return g;
    }

    static bool AddRenderLod(MeshGeo g, FStaticMeshLODResources lod)
    {
        var pos = lod.PositionVertexBuffer?.Verts;
        var ib = lod.IndexBuffer;
        if (pos == null || ib == null || lod.Sections.Length == 0) return false;
        int baseV = g.V.Count / 3;
        for (int i = 0; i < pos.Length; i++) g.AddV(pos[i].X, pos[i].Y, pos[i].Z);
        int added = 0;
        foreach (var s in lod.Sections)
        {
            if (!s.bEnableCollision) continue;
            for (int t = 0; t < s.NumTriangles; t++)
            {
                int o = s.FirstIndex + t * 3;
                if (o + 2 >= ib.Length) break;
                int a = (int)ib[o], b = (int)ib[o + 1], c = (int)ib[o + 2];
                if (a == b || b == c || a == c) continue;
                if (RenderWindingFlipped) g.AddTri(baseV + a, baseV + c, baseV + b); else g.AddTri(baseV + a, baseV + b, baseV + c);
                added++;
            }
        }
        return added > 0;
    }

    static void AddBox(MeshGeo g, FKBoxElem e)
    {
        var m = Xf.FromRotator(e.Rotation.Pitch, e.Rotation.Yaw, e.Rotation.Roll, e.Center.X, e.Center.Y, e.Center.Z, 1, 1, 1);
        BoxVerts(g, m, e.X * 0.5, e.Y * 0.5, e.Z * 0.5);
    }

    static void BoxVerts(MeshGeo g, in Xf m, double hx, double hy, double hz)
    {
        int[] v = new int[8];
        for (int i = 0; i < 8; i++)
        {
            var p = m.Apply((i & 1) == 0 ? -hx : hx, (i & 2) == 0 ? -hy : hy, (i & 4) == 0 ? -hz : hz);
            v[i] = g.AddV(p.x, p.y, p.z);
        }
        // corner i = (x bit0, y bit1, z bit2); faces wound so that (b-a)x(c-a) points out
        g.AddQuad(v[0], v[2], v[3], v[1]);   // -z
        g.AddQuad(v[4], v[5], v[7], v[6]);   // +z
        g.AddQuad(v[0], v[1], v[5], v[4]);   // -y
        g.AddQuad(v[2], v[6], v[7], v[3]);   // +y
        g.AddQuad(v[0], v[4], v[6], v[2]);   // -x
        g.AddQuad(v[1], v[3], v[7], v[5]);   // +x
    }

    static void AddSphere(MeshGeo g, FVector c, double r)
    {
        AddCapsuleAxis(g, Xf.FromTRS(c.X, c.Y, c.Z, (0, 0, 0, 1), 1, 1, 1), r, 0);
    }

    static void AddCapsule(MeshGeo g, FKSphylElem e)
    {
        var m = Xf.FromRotator(e.Rotation.Pitch, e.Rotation.Yaw, e.Rotation.Roll, e.Center.X, e.Center.Y, e.Center.Z, 1, 1, 1);
        AddCapsuleAxis(g, m, e.Radius, e.Length);
    }

    static void AddTaperedCapsule(MeshGeo g, FKTaperedCapsuleElem e)
    {
        var m = Xf.FromRotator(e.Rotation.Pitch, e.Rotation.Yaw, e.Rotation.Roll, e.Center.X, e.Center.Y, e.Center.Z, 1, 1, 1);
        AddCapsuleAxis(g, m, Math.Max(e.Radius0, e.Radius1), e.Length);
    }

    /// <summary>A capsule along the local z axis: a cylinder of `length` between two hemispheres of `r` (a sphere when length is 0); 12 segments, 3 rings a cap.</summary>
    static void AddCapsuleAxis(MeshGeo g, in Xf m, double r, double length)
    {
        const int seg = 12, rings = 3;
        double half = length * 0.5;
        var rowsZ = new List<(double z, double rad)>();
        for (int k = rings; k >= 1; k--) { double a = Math.PI * 0.5 * k / rings; rowsZ.Add((half + r * Math.Sin(a), r * Math.Cos(a))); }   // top cap, from the pole
        rowsZ.Add((half, r));
        if (length > 0) rowsZ.Add((-half, r));
        for (int k = 1; k <= rings; k++) { double a = Math.PI * 0.5 * k / rings; rowsZ.Add((-half - r * Math.Sin(a), r * Math.Cos(a))); }
        int topPole = g.AddV(m.Apply(0, 0, half + r).x, m.Apply(0, 0, half + r).y, m.Apply(0, 0, half + r).z);
        int botPole = g.AddV(m.Apply(0, 0, -half - r).x, m.Apply(0, 0, -half - r).y, m.Apply(0, 0, -half - r).z);
        var ring = new List<int[]>();
        foreach (var (z, rad) in rowsZ)
        {
            if (rad < 1e-6) continue;
            var ids = new int[seg];
            for (int s = 0; s < seg; s++)
            {
                double an = 2 * Math.PI * s / seg;
                var p = m.Apply(rad * Math.Cos(an), rad * Math.Sin(an), z);
                ids[s] = g.AddV(p.x, p.y, p.z);
            }
            ring.Add(ids);
        }
        for (int s = 0; s < seg; s++)
        {
            int s1 = (s + 1) % seg;
            g.AddTri(topPole, ring[0][s], ring[0][s1]);
            for (int k = 0; k + 1 < ring.Count; k++)
                g.AddQuad(ring[k][s], ring[k + 1][s], ring[k + 1][s1], ring[k][s1]);
            g.AddTri(botPole, ring[^1][s1], ring[^1][s]);
        }
    }
// ~ C:Y:B :: C | O | A ~

    static bool AddConvex(MeshGeo g, FKConvexElem e)
    {
        var vd = e.VertexData;
        if (vd == null || vd.Length < 4) return false;
        var el = e.Transform;
        bool hasXf = !(el.Scale3D.X == 0 && el.Scale3D.Y == 0 && el.Scale3D.Z == 0);
        Xf m = Xf.Identity;
        if (hasXf)
        {
            var q = el.Rotation;
            m = Xf.FromTRS(el.Translation.X, el.Translation.Y, el.Translation.Z, (q.X, q.Y, q.Z, q.W), el.Scale3D.X, el.Scale3D.Y, el.Scale3D.Z);
        }
        var pts = new (double x, double y, double z)[vd.Length];
        for (int i = 0; i < vd.Length; i++) pts[i] = m.Apply(vd[i].X, vd[i].Y, vd[i].Z);
        if (pts.Max(p => p.x) - pts.Min(p => p.x) > MaxShapeCm || pts.Max(p => p.y) - pts.Min(p => p.y) > MaxShapeCm || pts.Max(p => p.z) - pts.Min(p => p.z) > MaxShapeCm) { g.HugeSkipped++; return false; }
        int baseV = g.V.Count / 3;
        foreach (var p in pts) g.AddV(p.x, p.y, p.z);
        var idx = e.IndexData;
        var tris = new List<int>();
        if (idx != null && idx.Length >= 3 && idx.Length % 3 == 0 && idx.All(i => i >= 0 && i < vd.Length))
        {
            for (int i = 0; i < idx.Length; i += 3) tris.AddRange(new[] { idx[i], idx[i + 1], idx[i + 2] });
        }
        else tris = Hull.Faces(pts);
        // orient every face away from the centroid (the hull is convex): independent of the stored winding
        double cx = pts.Average(p => p.x), cy = pts.Average(p => p.y), cz = pts.Average(p => p.z);
        for (int i = 0; i + 2 < tris.Count; i += 3)
        {
            var a = pts[tris[i]]; var b = pts[tris[i + 1]]; var c = pts[tris[i + 2]];
            double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            double d = nx * (a.x - cx) + ny * (a.y - cy) + nz * (a.z - cz);
            if (Math.Abs(d) < 1e-9 && nx == 0 && ny == 0 && nz == 0) continue;
            if (d >= 0) g.AddTri(baseV + tris[i], baseV + tris[i + 1], baseV + tris[i + 2]);
            else g.AddTri(baseV + tris[i], baseV + tris[i + 2], baseV + tris[i + 1]);
        }
        return true;
    }
}

/// <summary>A convex hull's faces by brute force (a collision hull has a few dozen vertices).</summary>
public static class Hull
{
    public static List<int> Faces(IReadOnlyList<(double x, double y, double z)> p)
    {
        var res = new List<int>();
        int n = p.Count;
        if (n > 160) return res;
        double scale = 0;
        foreach (var q in p) scale = Math.Max(scale, Math.Max(Math.Abs(q.x), Math.Max(Math.Abs(q.y), Math.Abs(q.z))));
        double eps = 1e-6 * Math.Max(scale, 1.0);
        var seen = new HashSet<(double, double, double, double)>();
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                for (int k = j + 1; k < n; k++)
                {
                    double ux = p[j].x - p[i].x, uy = p[j].y - p[i].y, uz = p[j].z - p[i].z, vx = p[k].x - p[i].x, vy = p[k].y - p[i].y, vz = p[k].z - p[i].z;
                    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                    double len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    if (len < 1e-9) continue;
                    nx /= len; ny /= len; nz /= len;
                    double d0 = nx * p[i].x + ny * p[i].y + nz * p[i].z;
                    int pos = 0, neg = 0;
                    for (int m = 0; m < n && (pos == 0 || neg == 0); m++)
                    {
                        double d = nx * p[m].x + ny * p[m].y + nz * p[m].z - d0;
                        if (d > eps) pos++; else if (d < -eps) neg++;
                    }
                    if (pos == 0 || neg == 0) { res.Add(i); res.Add(j); res.Add(k); }
                }
        return res;
    }
}
