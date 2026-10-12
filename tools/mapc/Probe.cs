// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
namespace Mapc;

/// <summary>The answer key against the extracted geometry (a diagnostic of the extraction; the acceptance test proper is worldd's, on the navmesh).
/// The key: tab-separated kind, capture, category, x, y, z (the actor's origin, a capsule's centre), mode; a walking pawn's origin is a capsule's
/// half height above its ground (players 98 cm).</summary>
public static class Probe
{
    public static void Evaluate(AreaGeometry geo, string keyPath)
    {
        var rows = File.ReadAllLines(keyPath).Select(l => l.Split('\t')).ToList();
        var boxes = new List<(double x0, double y0, double x1, double y1, int idx)>();
        for (int i = 0; i < geo.Placements.Count; i++)
        {
            var p = geo.Placements[i]; var m = geo.Meshes[p.Mesh];
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            for (int c = 0; c < 8; c++)
            {
                var q = p.M.Apply((c & 1) == 0 ? m.MinX : m.MaxX, (c & 2) == 0 ? m.MinY : m.MaxY, (c & 4) == 0 ? m.MinZ : m.MaxZ);
                x0 = Math.Min(x0, q.x); x1 = Math.Max(x1, q.x); y0 = Math.Min(y0, q.y); y1 = Math.Max(y1, q.y);
            }
            boxes.Add((x0, y0, x1, y1, i));
        }
        int hitTerrain = 0, hitMesh = 0, miss = 0, airborne = 0;
        var report = new List<string>();
        foreach (var r in rows)
        {
            string kind = r[0], cat = r[2];
            double x = double.Parse(r[3]), y = double.Parse(r[4]), z = double.Parse(r[5]);
            string mode = r.Length > 6 ? r[6] : "";
            if (mode == "3") { airborne++; continue; }          // falling: a jump, not ground
            var surf = new List<(double h, string src)>();
            foreach (var t in geo.Tiles) if (t.TryHeight(x, y, out var th)) { surf.Add((th, "terrain")); break; }
            foreach (var b in boxes)
            {
                if (x < b.x0 || x > b.x1 || y < b.y0 || y > b.y1) continue;
                var p = geo.Placements[b.idx]; var m = geo.Meshes[p.Mesh];
                var M = p.M;
                for (int i = 0; i + 2 < m.I.Count; i += 3)
                {
                    var a = M.Apply(m.V[m.I[i] * 3], m.V[m.I[i] * 3 + 1], m.V[m.I[i] * 3 + 2]);
                    var bb = M.Apply(m.V[m.I[i + 1] * 3], m.V[m.I[i + 1] * 3 + 1], m.V[m.I[i + 1] * 3 + 2]);
                    var c = M.Apply(m.V[m.I[i + 2] * 3], m.V[m.I[i + 2] * 3 + 1], m.V[m.I[i + 2] * 3 + 2]);
                    double ux = bb.x - a.x, uy = bb.y - a.y, uz = bb.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                    double nz = ux * vy - uy * vx, nx = uy * vz - uz * vy, ny = uz * vx - ux * vz;
                    if (M.Det3 < 0) { nx = -nx; ny = -ny; nz = -nz; }
                    double len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    if (len < 1e-9 || nz / len < 0.2) continue;
                    double d = (bb.y - c.y) * (a.x - c.x) + (c.x - bb.x) * (a.y - c.y);
                    if (Math.Abs(d) < 1e-9) continue;
                    double l1 = ((bb.y - c.y) * (x - c.x) + (c.x - bb.x) * (y - c.y)) / d;
                    double l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
                    double l3 = 1 - l1 - l2;
                    if (l1 < -1e-4 || l2 < -1e-4 || l3 < -1e-4) continue;
                    surf.Add((l1 * a.z + l2 * bb.z + l3 * c.z, m.Name));
                }
            }
            bool person = kind == "player" || cat == "player_other";
            double lo = person ? 96 : 55, hi = person ? 106 : 150;
            double best = double.NaN; string bs = "";
            foreach (var (h, src) in surf)
            {
                double off = z - h;
                if (off >= lo && off <= hi && (double.IsNaN(best) || off < best)) { best = off; bs = src; }
            }
            if (double.IsNaN(best)) { miss++; report.Add($"MISS {kind}/{cat} cid {r[1]} mode {mode} ({x:F0},{y:F0},{z:F1}) offsets above: " + string.Join(" ", surf.Select(s => $"{z - s.h:F0}:{(s.src == "terrain" ? "T" : s.src.Split('/').Last())}").Take(8))); }
            else if (bs == "terrain") hitTerrain++; else hitMesh++;
        }
        Console.WriteLine($"key {rows.Count}: airborne (not ground) {airborne}; of the {rows.Count - airborne} on the ground: terrain {hitTerrain}, a mesh surface {hitMesh}, miss {miss}");
        foreach (var l in report.Take(60)) Console.WriteLine(l);
    }
}
