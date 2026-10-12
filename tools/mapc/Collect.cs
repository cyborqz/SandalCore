// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component.StaticMesh;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.PhysicsEngine;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>A placed mesh.  <paramref name="Src"/> is where it came from in the one order E1's single-process read had: 0 the persistent level, then 1 + the position of its cell in the
/// cells sorted by name.  Of two placements that are one (same mesh, place and rotation: a component two packages hold) the lower Src wins, which is the one E1's read kept (the first it met),
/// whichever batch and in whichever order the cells are read now.</summary>
public readonly record struct Placement(int Mesh, Xf M, int Src = 0);

/// <summary>One gatherable foliage instance (stage 3's trees and plants): an `IntrepidGatherableFISMC_*` component's instance of a Foliage cell, in world space.</summary>
public sealed record FoliageRow(string Cell, string Owner, string Component, int Index, long DefId, string DefName, string Mesh, double X, double Y, double Z, double Yaw, double Pitch, double Roll, double Sx, double Sy, double Sz, string Tags);

/// <summary>What mapc reads out of the map for the area.</summary>
public sealed class AreaGeometry
{
    public double CenterX, CenterY, Radius, Margin;
    public Region? Region;              // the Riverlands' rectangles when the map was read for them (the geometry file carries them: the navmesh covers their tiles)
    public List<TerrainTile> Tiles = new();
    public List<MeshGeo> Meshes = new();
    public List<Placement> Placements = new();
    public SortedDictionary<string, long> Stats = new();
    public List<string> Warnings = new();
    public List<FoliageRow> Foliage = new();

    public void Stat(string k, long n = 1) { Stats[k] = Stats.GetValueOrDefault(k) + n; }

    /// <summary>The deterministic order, whatever the load order was.</summary>
    public void Finish()
    {
        Placements = Placements.OrderBy(p => Meshes[p.Mesh].Name, StringComparer.Ordinal)
            .ThenBy(p => Math.Round(p.M.M03 * 4)).ThenBy(p => Math.Round(p.M.M13 * 4)).ThenBy(p => Math.Round(p.M.M23 * 4))
            .ThenBy(p => Math.Round(p.M.M00 * 1000)).ThenBy(p => Math.Round(p.M.M10 * 1000)).ThenBy(p => Math.Round(p.M.M22 * 1000)).ToList();
        Tiles = Tiles.OrderBy(t => t.X0).ThenBy(t => t.Y0).ToList();
        // the instance's key is (cell, owner, component, index): a component's NAME repeats across the actors of one cell (review B I2), the actor's does not
        Foliage = Foliage.OrderBy(f => f.Cell, StringComparer.Ordinal).ThenBy(f => f.Owner, StringComparer.Ordinal).ThenBy(f => f.Component, StringComparer.Ordinal).ThenBy(f => f.Index).ToList();
    }
// [C.O.A :: Cyb]

    /// <summary>Two placements that are one and come from the same package or the same source rank: the one with the smaller matrix wins (a tie-break that depends on nothing else).</summary>
    public static bool Smaller(in Xf a, in Xf b)
    {
        float[] fa = { (float)a.M00, (float)a.M01, (float)a.M02, (float)a.M10, (float)a.M11, (float)a.M12, (float)a.M20, (float)a.M21, (float)a.M22 };
        float[] fb = { (float)b.M00, (float)b.M01, (float)b.M02, (float)b.M10, (float)b.M11, (float)b.M12, (float)b.M20, (float)b.M21, (float)b.M22 };
        for (int i = 0; i < 9; i++) if (fa[i] != fb[i]) return fa[i] < fb[i];
        if (a.M03 != b.M03) return a.M03 < b.M03;
        if (a.M13 != b.M13) return a.M13 < b.M13;
        return a.M23 < b.M23;
    }
}

public static class Collector
{
    // collision profiles by what the pawn (the player's and the NPCs' capsules) does against them; the game's DefaultEngine.ini lists them
    static readonly HashSet<string> NonBlockingProfiles = new(StringComparer.Ordinal)
    {
        "NoCollision", "OverlapAll", "OverlapAllDynamic", "IgnoreOnlyPawn", "OverlapOnlyPawn", "Trigger", "UI", "Spectator", "CharacterMesh", "Ragdoll",
        "Audio", "WaterBodyCollision", "WorldOnlyProjectile", "BlockNPC", "DeployableNoOverlap", "DeployableMustOverlap", "DeployableFootprint",
        "Vehicle", "Projectile", "DynamicOnlyProjectile",
    };

    /// <summary>Whether a placed mesh component stops a walking pawn: its BodyInstance (or its archetype's) says the pawn channel blocks.</summary>
    public static bool BlocksPawn(UObject comp, AreaGeometry stats, string meshDefaultProfile = "", string meshDefaultEnabled = "")
    {
        string profile = Lv.TryStructField<FName>(comp, "BodyInstance", "CollisionProfileName", out var pn) ? pn.Text : "";
        if (profile == "None") profile = "";
        bool useMesh = profile == "" || profile == "Default";
        string enabled = "";
        for (UObject? o = comp; o != null && enabled.Length == 0; o = o.Template?.Object?.Value)
            if (o.TryGetValue(out FStructFallback st, "BodyInstance") && st != null)
                enabled = st.Properties.FirstOrDefault(p => p.Name.Text == "CollisionEnabled")?.Tag?.GenericValue?.ToString() ?? "";
        if (useMesh && enabled.Length == 0) { profile = meshDefaultProfile; enabled = meshDefaultEnabled; }
        else if (useMesh) profile = meshDefaultProfile;
        stats.Stat("profile." + (profile.Length == 0 ? "(blank)" : profile) + (enabled.Length > 0 ? "/" + enabled.Replace("ECollisionEnabled::", "") : ""));
        if (profile.Length > 0 && NonBlockingProfiles.Contains(profile)) return false;
        if (enabled.Contains("NoCollision") || enabled.Contains("PhysicsOnly")) return false;
        if (profile == "Custom" || profile == "")
        {
            // an explicit pawn response in the body's overrides
            for (UObject? o = comp; o != null; o = o.Template?.Object?.Value)
            {
                if (!(o.TryGetValue(out FStructFallback st, "BodyInstance") && st != null)) continue;
                var resp = st.GetOrDefault<FStructFallback?>("CollisionResponses", null);
                var arr = resp?.GetOrDefault<UScriptArray?>("ResponseArray", null);
                if (arr == null) continue;
                foreach (var item in arr.Properties)
                {
                    if (item.GenericValue is not FScriptStruct ss || ss.StructType is not FStructFallback f) continue;
                    var ch = f.GetOrDefault<FName>("Channel").Text;
                    var rs = f.Properties.FirstOrDefault(x => x.Name.Text == "Response")?.Tag?.GenericValue?.ToString() ?? "";
                    if (ch == "Pawn" && !rs.Contains("Block")) return false;
                }
                break;
            }
        }
        return true;
    }

    /// <summary>
    /// The map read for a scope.  <paramref name="batch"/> (N, M) reads batch N of M of the read set (Cells.Split: a child process of `mapc extract --batches M`), batch 0 with the
    /// persistent level; null reads every cell in this process.  The cells one at a time (E1 review I3): a cell's package, its exports and the static-mesh packages its imports resolved to
    /// (CUE4Parse's provider keeps no package cache: LoadPackage parses anew) are let go before the next cell is read, so the heap holds one cell and the MeshGeo of every mesh seen.
    /// The result does not depend on the order the cells are read in or on how they are batched: the placements are sorted at the end, the meshes are numbered by first use of the sorted
    /// placements, and of two placements that are one the smaller matrix wins (AreaGeometry.Smaller).
    /// </summary>
    public static AreaGeometry Collect(Game g, Scope scope, (int n, int m)? batch = null, Action<string>? log = null)
    {
        var res = new AreaGeometry { CenterX = scope.Cx, CenterY = scope.Cy, Radius = scope.Radius, Margin = scope.Margin, Region = scope.Region };
        List<CellInfo> mine;
        bool withMain = true;
        var all = Cells.Describe(g, scope);
        if (batch is { } bt)
        {
            mine = Cells.Split(all, bt.m)[bt.n];
            withMain = bt.n == 0;
            log?.Invoke($"batch {bt.n}/{bt.m}: {mine.Count} of the {all.Count(c => c.IsReadGrid)} cells to read");
        }
        else mine = all.Where(c => c.IsReadGrid).ToList();
        foreach (var c in all.Where(c => !c.IsReadGrid)) if (batch is null || batch.Value.n == 0) res.Stat("cells.hlod.skipped");
        var main = g.Load(Game.MasterPath + ".umap")!;

        var rank = new Dictionary<string, int>(StringComparer.Ordinal);
        for (int i = 0; i < all.Count; i++) rank[all[i].Id] = i + 1;

        IEnumerable<(string name, IPackage pkg, string grid, string? path)> Packages()
        {
            if (withMain) yield return ("MAIN", main, "persistent", null);
            foreach (var c in mine)
            {
                var p = g.Load(Cells.PathOf(c.Id));
                if (p == null) { res.Stat("cells.notloadable"); res.Warnings.Add($"cell {c.Id} ({c.Grid}) is not in the game's containers"); continue; }
                res.Stat("cells." + c.Grid);
                yield return (c.Id, p, c.Grid, Cells.PathOf(c.Id));
            }
        }

        var meshIdByPath = new Dictionary<string, int>();
        var seen = new Dictionary<(int, long, long, long, long, long, long), int>();
        int done = 0;
        var swAll = System.Diagnostics.Stopwatch.StartNew();
        foreach (var (name, pkg, grid, path) in Packages())
        {
            try
            {
                CollectPackage(res, name, pkg, grid, meshIdByPath, seen, scope, rank.GetValueOrDefault(name, 0));
            }
            finally
            {
                if (path != null) g.Forget(path);
            }
            if (log != null && ++done % 100 == 0) log($"  {done} packages, {swAll.Elapsed.TotalSeconds:F0} s, {res.Placements.Count} placements, {res.Meshes.Count} meshes, {res.Foliage.Count} foliage instances");
        }
        res.Finish();
        return res;
    }

    /// <summary>The E1 call: one process, the circle.</summary>
    public static AreaGeometry Collect(Game g, double cx, double cy, double radius, double margin) => Collect(g, new Scope(cx, cy, radius, margin));
// [CoA - C_Y_B]

    /// <summary>One package's terrain tiles (a Landscape cell) or placed meshes into <paramref name="res"/>.</summary>
    static void CollectPackage(AreaGeometry res, string name, IPackage pkg, string grid, Dictionary<string, int> meshIdByPath,
                               Dictionary<(int, long, long, long, long, long, long), int> seen, Scope scope, int src)
    {
        {
            if (grid == "Landscape")
            {
                foreach (var t in Terrain.Collect(pkg, name, w => res.Warnings.Add(w))) res.Tiles.Add(t);
                return;
            }
            var xfCache = new Dictionary<UObject, Xf>(ReferenceEqualityComparer.Instance);
            Xf WorldOf(UObject o)
            {
                if (xfCache.TryGetValue(o, out var x)) return x;
                x = Lv.World(o);
                xfCache[o] = x;
                return x;
            }
            foreach (var e in pkg.GetExports())
            {
                if (e is not UStaticMeshComponent smc) continue;
                res.Stat("components.seen");
                if (grid == "Water") { res.Stat("components.water.skipped"); continue; }
                if (e.ExportType == "SplineMeshComponent" || e.ExportType == "PCGManagedSplineMeshComponent") { res.Stat("components.spline.skipped"); continue; }
                var wx = WorldOf(e);
                if (grid == "Foliage" && e.ExportType.StartsWith("IntrepidGatherableFISMC", StringComparison.Ordinal) && e is UInstancedStaticMeshComponent gism) CollectFoliage(res, name, gism, wx, scope);
                // a cheap cull before anything is loaded: far outside the circle even with the mesh's size left to the later test
                if (scope.CullFar(wx.M03, wx.M13, 60000)) { res.Stat("components.far"); continue; }
                bool instanced = e is UInstancedStaticMeshComponent;
                FInstancedStaticMeshInstanceData[]? inst = null;
                if (instanced)
                {
                    inst = ((UInstancedStaticMeshComponent)e).GetInstances();
                    if (inst.Length == 0) { res.Stat("components.instanced.empty"); continue; }
                }
                var meshIdx = smc.GetStaticMesh();
                var meshObj = Lv.Resolve(meshIdx) as UStaticMesh;
                if (meshObj == null) { res.Stat("components.nomesh"); continue; }
                string meshPath = Lv.PathOf(meshIdx);
                // A mesh is known by its package path; its name in the geometry file is the short name (E1's, which sorts the placements) and a hash of that path, so that two assets with the
                // same short name in different folders stay two meshes (the batches' parts are merged by name).  The hash comes after the closing quote: the order of different short names is unchanged.
                string fullPath = "";
                try { fullPath = meshIdx.ResolvedObject?.GetPathName() ?? ""; } catch { }
                if (fullPath.Length == 0) fullPath = meshPath;
                if (!meshIdByPath.TryGetValue(fullPath, out int mid))
                {
                    var bs = Lv.Resolve(meshObj.BodySetup) as UBodySetup;
                    var geo = MeshShapes.Build(meshObj, meshPath + "@" + Fnv32(fullPath), bs);
                    mid = res.Meshes.Count;
                    res.Meshes.Add(geo);
                    meshIdByPath[fullPath] = mid;
                    res.Stat("meshes." + geo.Kind);
                    if (geo.HugeSkipped > 0) { res.Stat("meshes.hugeShapesSkipped", geo.HugeSkipped); res.Stat("meshes.withHugeShape"); }
                }
                var mg = res.Meshes[mid];
                if (mg.Kind == "none") { res.Stat("components.nocollisionshape"); continue; }
                if (!BlocksPawn(e, res, mg.DefaultProfile, mg.DefaultEnabled)) { res.Stat("components.nonblocking" + (instanced ? ".instanced" : "")); continue; }
                double rad = Math.Max(Math.Max(Math.Abs(mg.MinX), Math.Abs(mg.MaxX)), Math.Max(Math.Max(Math.Abs(mg.MinY), Math.Abs(mg.MaxY)), Math.Max(Math.Abs(mg.MinZ), Math.Abs(mg.MaxZ)))) * 1.75;
                if (instanced)
                {
                    foreach (var inst1 in inst!)
                    {
                        var t = inst1.TransformData;
                        var local = Xf.FromTRS(t.Translation.X, t.Translation.Y, t.Translation.Z, (t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W), t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z);
                        AddPlacement(res, seen, mid, rad * Math.Max(wx.ScaleX, Math.Max(wx.ScaleY, wx.ScaleZ)), wx.Mul(local), scope, src);
                    }
                }
                else AddPlacement(res, seen, mid, rad, wx, scope, src);
            }
        }
    }
// CoA / C.Y.B

    /// <summary>
    /// The gatherable foliage of a Foliage cell (stage 3's trees and plants): one row per instance of an IntrepidGatherableFISMC component that stands in the scope.  The def is the component's
    /// DDERecordId / DDERecordName: a GatheringFoliageData record of the design data (not a GatherableActorDef: 68 of the 73 ids are GatheringFoliageData guids in content.db and none is a
    /// GatherableActorDef's); a component that does not serialize its id (4.2 % of the rows, id 0) still names the def, and the name resolves it.  The owner is the actor that holds the
    /// component (its name is what tells two components of one name apart: a component's name repeats across the actors of a cell, review B I2), the index is the instance's place in the
    /// component's PerInstanceSMData (what a cell's AIntrepidStateIFA indexes by), the tags are the component's GatherableGameplayTagMap entry for that index.
    /// </summary>
    static void CollectFoliage(AreaGeometry res, string cell, UInstancedStaticMeshComponent comp, Xf world, Scope scope)
    {
        FInstancedStaticMeshInstanceData[] inst;
        try { inst = comp.GetInstances(); } catch (Exception ex) { res.Warnings.Add($"{cell}: {comp.Name}: no instances ({ex.Message})"); return; }
        res.Stat("foliage.gatherable.components");
        long defId = comp.GetOrDefault("DDERecordId", 0L);
        string defName = comp.GetOrDefault<FName>("DDERecordName").Text ?? "";
        // 4.2 % of the components name their def but do not serialize its id (13 names, 3,535 instances; the archetype chain does not hold it either: tried, review B I2): counted, and
        // the reader resolves the name (it is unique in content.db's GatheringFoliageData for each of the 13)
        if (defId == 0) res.Stat("foliage.gatherable.def_id.zero");
        string owner = comp.Outer?.Name ?? "";
        string mesh = Lv.PathOf(comp.GetStaticMesh());
        int q = mesh.IndexOf('\''); if (q >= 0) { int q2 = mesh.LastIndexOf('\''); if (q2 > q) mesh = mesh[(q + 1)..q2]; }
        var tags = new Dictionary<int, string>();
        try
        {
            var prop = comp.Properties.FirstOrDefault(p => p.Name.Text == "GatherableGameplayTagMap");
            if (prop?.Tag?.GenericValue is UScriptMap map)
            {
                foreach (var kv in map.Properties)
                {
                    string vs = Newtonsoft.Json.JsonConvert.SerializeObject(kv.Value?.GenericValue, new Newtonsoft.Json.JsonSerializerSettings { ReferenceLoopHandling = Newtonsoft.Json.ReferenceLoopHandling.Ignore, Error = (s2, a2) => a2.ErrorContext.Handled = true });
                    var names = new List<string>();
                    foreach (var tok in Newtonsoft.Json.Linq.JToken.Parse(vs)) names.Add(tok.ToString());
                    if (int.TryParse(kv.Key?.GenericValue?.ToString(), out int idx)) tags[idx] = string.Join(";", names.OrderBy(n => n, StringComparer.Ordinal));
                }
            }
        }
        catch (Exception ex) { res.Warnings.Add($"{cell}: {comp.Name}: the tag map is unreadable ({ex.Message})"); }
        for (int i = 0; i < inst.Length; i++)
        {
            var t = inst[i].TransformData;
            var local = Xf.FromTRS(t.Translation.X, t.Translation.Y, t.Translation.Z, (t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W), t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z);
            var w = world.Mul(local);
            if (!scope.Contains(w.M03, w.M13)) { res.Stat("foliage.gatherable.outside"); continue; }
            double sx = w.ScaleX, sy = w.ScaleY, sz = w.ScaleZ;
            double xx = w.M00 / sx, xy = w.M10 / sx, xz = w.M20 / sx, yx = w.M01 / sy, yy = w.M11 / sy, yz = w.M21 / sy, zx = w.M02 / sz, zy = w.M12 / sz, zz = w.M22 / sz;
            double yaw = Math.Atan2(xy, xx), pitch = Math.Atan2(xz, Math.Sqrt(xx * xx + xy * xy));
            double syx = -Math.Sin(yaw), syy = Math.Cos(yaw);
            double roll = Math.Atan2(zx * syx + zy * syy, yx * syx + yy * syy);
            const double r2d = 180.0 / Math.PI;
            res.Foliage.Add(new FoliageRow(cell, owner, comp.Name, i, defId, defName, mesh, w.M03, w.M13, w.M23, yaw * r2d, pitch * r2d, roll * r2d, sx, sy, sz, tags.GetValueOrDefault(i, "")));
            res.Stat("foliage.gatherable.instances");
        }
    }

    static string Fnv32(string s)
    {
        uint h = 2166136261u;
        foreach (char ch in s) { h ^= ch; h *= 16777619u; }
        return h.ToString("x8");
    }

    static void AddPlacement(AreaGeometry res, Dictionary<(int, long, long, long, long, long, long), int> seen, int mesh, double radius, Xf m, Scope scope, int src)
    {
        if (!scope.PlacementIn(m.M03, m.M13, radius)) { res.Stat("placements.outside"); return; }
        var key = (mesh, (long)Math.Round(m.M03 * 4), (long)Math.Round(m.M13 * 4), (long)Math.Round(m.M23 * 4), (long)Math.Round(m.M00 * 1000), (long)Math.Round(m.M10 * 1000), (long)Math.Round(m.M22 * 1000));
        if (seen.TryGetValue(key, out int at))
        {
            res.Stat("placements.duplicate");
            if (src < res.Placements[at].Src) res.Placements[at] = new Placement(mesh, m, src);
            return;
        }
        seen[key] = res.Placements.Count;
        res.Placements.Add(new Placement(mesh, m, src));
        res.Stat("placements.kept");
    }
}
