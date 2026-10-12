// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;

namespace Mapc;

/// <summary>
/// The area geometry file (dist/content/area_geom.bin), little-endian, written the same bytes every run.  Version 2 (G1 stage 1; version 1, E1's, had no region):
///   "AOCGEOM1", u32 version (2), f64 centre x, centre y, radius, margin (cm: E1's circle around the login spot; the navmesh's grid is anchored on it)
///   u64 region hash (FNV-1a of the rectangles' text; 0: none), u32 region rectangles, then f64 x0 y0 x1 y1 for each (the footprint the map was read for: the navmesh covers the tiles that touch it)
///   u32 terrain tiles, u32 meshes, u32 placements
///   tile:      f64 x0 y0 stepX stepY zBase zScale, u32 nx ny, u16 raw[nx*ny]       (world z = zBase + (raw - 32768) * zScale; row-major, x fastest)
///   mesh:      u16 name length, name (utf-8), u32 vertices, u32 indices, f32 xyz[vertices*3], u32 index[indices]   (the winding: (b-a)x(c-a) points out of the solid)
///   placement: u32 mesh, f32 m00 m01 m02, m10 m11 m12, m20 m21 m22, f64 tx ty tz                                    (world = R * local + t, the scale inside R)
/// Terrain tiles, meshes and placements are in the deterministic order AreaGeometry.Finish gives.  Why one file and not chunks: the whole Riverlands' geometry is a few hundred megabytes
/// (the 1,636-cell read of this repository: see tools/mapc/README.md for the measured size), which aoc_navbuild reads whole once, offline; the bounded-memory half of the ground is the
/// NAVMESH file (server/worldd/nav/nav_file.h, version 2: chunks of 8 x 8 tiles), which worldd streams.  The batches of `mapc extract --batches M` write this same format as parts; the
/// parent merges them (<see cref="Merge"/>).  Version 1 (E1, no region) is still read by aoc_navbuild.
/// </summary>
public static class GeomFile
{
    public const string Magic = "AOCGEOM1";
    public const uint Version = 2;

    public static byte[] Write(AreaGeometry g)
    {
        // only the meshes something uses, renumbered in the order of their first use
        var used = new Dictionary<int, int>();
        var order = new List<int>();
        foreach (var p in g.Placements)
            if (!used.ContainsKey(p.Mesh)) { used[p.Mesh] = order.Count; order.Add(p.Mesh); }
        var ms = new MemoryStream(1 << 24);
        using (var w = new BinaryWriter(ms, Encoding.UTF8, true))
        {
            w.Write(Encoding.ASCII.GetBytes(Magic));
            w.Write(Version);
            w.Write(g.CenterX); w.Write(g.CenterY); w.Write(g.Radius); w.Write(g.Margin);
            w.Write(g.Region?.Hash ?? 0ul);
            w.Write((uint)(g.Region?.Rects.Count ?? 0));
            if (g.Region != null)
                foreach (var (x0, y0, x1, y1) in g.Region.Rects) { w.Write(x0); w.Write(y0); w.Write(x1); w.Write(y1); }
            w.Write((uint)g.Tiles.Count); w.Write((uint)order.Count); w.Write((uint)g.Placements.Count);
            foreach (var t in g.Tiles)
            {
                w.Write(t.X0); w.Write(t.Y0); w.Write(t.StepX); w.Write(t.StepY); w.Write(t.ZBase); w.Write(t.ZScale);
                w.Write((uint)t.Nx); w.Write((uint)t.Ny);
                foreach (var v in t.Raw) w.Write(v);
            }
            foreach (int mi in order)
            {
                var m = g.Meshes[mi];
                var name = Encoding.UTF8.GetBytes(m.Name);
                w.Write((ushort)name.Length); w.Write(name);
                w.Write((uint)(m.V.Count / 3)); w.Write((uint)m.I.Count);
                foreach (var f in m.V) w.Write(f);
                foreach (var i in m.I) w.Write((uint)i);
            }
            foreach (var p in g.Placements)
            {
                w.Write((uint)used[p.Mesh]);
                var M = p.M;
                w.Write((float)M.M00); w.Write((float)M.M01); w.Write((float)M.M02);
                w.Write((float)M.M10); w.Write((float)M.M11); w.Write((float)M.M12);
                w.Write((float)M.M20); w.Write((float)M.M21); w.Write((float)M.M22);
                w.Write(M.M03); w.Write(M.M13); w.Write(M.M23);
            }
        }
        return ms.ToArray();
    }
// CoA x C:Y:B

    /// <summary>Reads a version 2 file back (a batch's part).  The placements keep the float matrices the file holds (widened to double).</summary>
    public static AreaGeometry Read(byte[] data)
    {
        using var r = new BinaryReader(new MemoryStream(data, false), Encoding.UTF8);
        if (Encoding.ASCII.GetString(r.ReadBytes(8)) != Magic) throw new InvalidDataException("not an area_geom.bin");
        if (r.ReadUInt32() != Version) throw new InvalidDataException("unsupported area_geom.bin version");
        var g = new AreaGeometry { CenterX = r.ReadDouble(), CenterY = r.ReadDouble(), Radius = r.ReadDouble(), Margin = r.ReadDouble() };
        ulong hash = r.ReadUInt64();
        uint nr = r.ReadUInt32();
        if (nr > 0)
        {
            var reg = new Region { Hash = hash };
            for (uint i = 0; i < nr; i++)
            {
                double x0 = r.ReadDouble(), y0 = r.ReadDouble(), x1 = r.ReadDouble(), y1 = r.ReadDouble();
                reg.Rects.Add((x0, y0, x1, y1));
                reg.MinX = Math.Min(reg.MinX, x0); reg.MinY = Math.Min(reg.MinY, y0); reg.MaxX = Math.Max(reg.MaxX, x1); reg.MaxY = Math.Max(reg.MaxY, y1);
            }
            g.Region = reg;
        }
        uint nt = r.ReadUInt32(), nm = r.ReadUInt32(), np = r.ReadUInt32();
        for (uint i = 0; i < nt; i++)
        {
            var t = new TerrainTile { X0 = r.ReadDouble(), Y0 = r.ReadDouble(), StepX = r.ReadDouble(), StepY = r.ReadDouble(), ZBase = r.ReadDouble(), ZScale = r.ReadDouble() };
            t.Nx = (int)r.ReadUInt32(); t.Ny = (int)r.ReadUInt32();
            t.Raw = new ushort[t.Nx * t.Ny];
            var bytes = r.ReadBytes(t.Raw.Length * 2);
            Buffer.BlockCopy(bytes, 0, t.Raw, 0, bytes.Length);
            g.Tiles.Add(t);
        }
        for (uint i = 0; i < nm; i++)
        {
            int nl = r.ReadUInt16();
            var m = new MeshGeo { Name = Encoding.UTF8.GetString(r.ReadBytes(nl)), Kind = "part" };
            int nv = (int)r.ReadUInt32(), ni = (int)r.ReadUInt32();
            var vb = r.ReadBytes(nv * 12);
            var fl = new float[nv * 3];
            Buffer.BlockCopy(vb, 0, fl, 0, vb.Length);
            m.V = new List<float>(fl);
            for (int k = 0; k < fl.Length; k += 3)
            {
                m.MinX = Math.Min(m.MinX, fl[k]); m.MaxX = Math.Max(m.MaxX, fl[k]);
                m.MinY = Math.Min(m.MinY, fl[k + 1]); m.MaxY = Math.Max(m.MaxY, fl[k + 1]);
                m.MinZ = Math.Min(m.MinZ, fl[k + 2]); m.MaxZ = Math.Max(m.MaxZ, fl[k + 2]);
            }
            var ib = r.ReadBytes(ni * 4);
            var il = new int[ni];
            Buffer.BlockCopy(ib, 0, il, 0, ib.Length);
            m.I = new List<int>(il);
            g.Meshes.Add(m);
        }
        for (uint i = 0; i < np; i++)
        {
            int mesh = (int)r.ReadUInt32();
            double m00 = r.ReadSingle(), m01 = r.ReadSingle(), m02 = r.ReadSingle(), m10 = r.ReadSingle(), m11 = r.ReadSingle(), m12 = r.ReadSingle(), m20 = r.ReadSingle(), m21 = r.ReadSingle(), m22 = r.ReadSingle();
            double tx = r.ReadDouble(), ty = r.ReadDouble(), tz = r.ReadDouble();
            g.Placements.Add(new Placement(mesh, new Xf(m00, m01, m02, tx, m10, m11, m12, ty, m20, m21, m22, tz)));
        }
        return g;
    }

    /// <summary>A part's sidecar: the Src of each placement, in the order the part file lists them.</summary>
    public static void WriteSrc(string path, AreaGeometry g)
    {
        var bytes = new byte[g.Placements.Count * 4];
        for (int i = 0; i < g.Placements.Count; i++) BitConverter.TryWriteBytes(bytes.AsSpan(i * 4, 4), g.Placements[i].Src);
        File.WriteAllBytes(path, bytes);
    }

    public static void ReadSrc(string path, AreaGeometry g)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length != g.Placements.Count * 4) throw new InvalidDataException(path + " does not match its part");
        for (int i = 0; i < g.Placements.Count; i++) g.Placements[i] = g.Placements[i] with { Src = BitConverter.ToInt32(bytes, i * 4) };
    }

    /// <summary>
    /// The batches' parts as one geometry: the terrain tiles side by side, the meshes joined by name (a mesh is the same triangles whichever batch read it), the placements joined by the
    /// collector's own key with the smaller matrix winning; the result is sorted by AreaGeometry.Finish, so it does not depend on how the cells were cut into batches or in which order the
    /// parts come.
    /// </summary>
    public static AreaGeometry Merge(IEnumerable<AreaGeometry> parts)
    {
        AreaGeometry? acc = null;
        var meshByName = new Dictionary<string, int>(StringComparer.Ordinal);
        var seen = new Dictionary<(int, long, long, long, long, long, long), int>();
        foreach (var part in parts)
        {
            acc ??= new AreaGeometry { CenterX = part.CenterX, CenterY = part.CenterY, Radius = part.Radius, Margin = part.Margin, Region = part.Region };
            var map = new int[part.Meshes.Count];
            for (int i = 0; i < part.Meshes.Count; i++)
            {
                if (!meshByName.TryGetValue(part.Meshes[i].Name, out int id)) { id = acc.Meshes.Count; acc.Meshes.Add(part.Meshes[i]); meshByName[part.Meshes[i].Name] = id; }
                map[i] = id;
            }
            foreach (var t in part.Tiles) acc.Tiles.Add(t);
            foreach (var p in part.Placements)
            {
                var m = p.M;
                var key = (map[p.Mesh], (long)Math.Round(m.M03 * 4), (long)Math.Round(m.M13 * 4), (long)Math.Round(m.M23 * 4), (long)Math.Round(m.M00 * 1000), (long)Math.Round(m.M10 * 1000), (long)Math.Round(m.M22 * 1000));
                if (seen.TryGetValue(key, out int at))
                {
                    var have = acc.Placements[at];
                    if (p.Src < have.Src || (p.Src == have.Src && AreaGeometry.Smaller(m, have.M))) acc.Placements[at] = new Placement(map[p.Mesh], m, p.Src);
                    continue;
                }
                seen[key] = acc.Placements.Count;
                acc.Placements.Add(new Placement(map[p.Mesh], m, p.Src));
            }
            foreach (var kv in part.Stats) acc.Stat(kv.Key, kv.Value);
            acc.Warnings.AddRange(part.Warnings);
            acc.Foliage.AddRange(part.Foliage);
        }
        acc ??= new AreaGeometry();
        acc.Finish();
        return acc;
    }
}
