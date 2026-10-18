// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component.StaticMesh;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Objects.PhysicsEngine;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>
/// mapc town-layouts: what the five towns' layouts place in a bowl, as collision geometry.  A bowl's buildings, walls, terraces, stairs and towers are static meshes that the client
/// places from the node's layout (props and pre-built plots, filtered by the node's level) over the platform that mapc town-floors reads; the walking map needs them as obstacles
/// and as upper floors.  The list of what stands where is made from content.db by tools/capture/gen_town_layouts.py (dist/content/town_layout_items.tsv: world cm, one row per placed
/// instance, a bit per node level it stands at); this reads that list and the game's packages, and writes dist/content/town_layouts.bin in the area geometry's own format (GeomFile:
/// no terrain, the meshes and their placements), which `mapc build` merges into area_geom.bin before aoc_navbuild builds the navigation mesh from it.
///
/// A static mesh is read once, by its package path: its simple collision (the boxes, spheres, capsules and convex hulls of the body setup; the render LOD only for a mesh whose trace
/// flag is UseComplexAsSimple), exactly as the map's meshes are (MeshShapes).  A layout instance uses the collision profile "Default": the mesh's own, so a mesh whose default profile or
/// collision is none does not block and is left out.  A level instance (a world, or a Blueprint that places one) is read like a cell: its static mesh components with their transforms
/// (and, for a nested level instance, the world it names), put at the row's transform.  Doors are not drawn (an open doorway is walkable).
/// </summary>
public static class TownLayouts
{
    public const string ItemsFileName = "town_layout_items.tsv";
    public const string FileName = "town_layouts.bin";
    public const int AllLevels = 0x7f;
    const int MaxLevelInstanceDepth = 4;

    public sealed class Bowl
    {
        public long Node;
        public string Name = "", Layout = "", Type = "";
        public double X, Y, Z, Yaw;
    }

    public sealed class Asset
    {
        public int Id;
        public char Kind;          // 'M' a static mesh, 'L' a level instance
        public string Path = "";   // the object path ("/Game/.../SM_Name.SM_Name")
    }

    /// <summary>One placed instance: its bowl and asset (indices), the node levels it stands at (a bit each), whether a character can step up onto it, and its place in the world.</summary>
    public readonly record struct Row(int Bowl, int Asset, int Levels, bool StepUp, Xf M);

    public sealed class Items
    {
        public List<Bowl> Bowls = new();
        public List<Asset> Assets = new();
        public List<Row> Rows = new();
    }

    /// <summary>One static mesh of a level instance, in the level instance's own frame.</summary>
    public readonly record struct LiPart(MeshGeo Mesh, Xf Local);

    /// <summary>Where the meshes come from: the game's packages (<see cref="GameSource"/>), or a test's.</summary>
    public interface ISource
    {
        /// <summary>The mesh's pawn-blocking geometry, or null when it has none (no shape, a profile that does not block, a package the game does not have).</summary>
        MeshGeo? Mesh(string objectPath);
        /// <summary>The blocking static meshes of a level instance, in its own frame.</summary>
        IReadOnlyList<LiPart> LevelInstance(string objectPath);
    }

    // ---- the list ----------------------------------------------------------------------------------------------------------------------------

    static double D(string s) => double.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);

    /// <summary>Reads the list (tools/capture/gen_town_layouts.py's file).  A malformed line is an error that names it.</summary>
    public static Items Parse(IEnumerable<string> lines, string source = "town layout items")
    {
        var it = new Items();
        var bowlOf = new Dictionary<long, int>();
        int n = 0;
        foreach (var raw in lines)
        {
            n++;
            var line = raw.TrimEnd('\r');
            if (line.Length == 0 || line[0] == '#') continue;
            var c = line.Split('\t');
            try
            {
                if (c[0] == "@node")
                {
                    if (c.Length != 9) throw new FormatException("a node line has 9 columns");
                    var b = new Bowl { Node = long.Parse(c[1], CultureInfo.InvariantCulture), Name = c[2], Layout = c[3], Type = c[4], X = D(c[5]), Y = D(c[6]), Z = D(c[7]), Yaw = D(c[8]) };
                    if (bowlOf.ContainsKey(b.Node)) throw new FormatException("a node is listed twice");
                    bowlOf[b.Node] = it.Bowls.Count;
                    it.Bowls.Add(b);
                }
                else if (c[0] == "@asset")
                {
                    if (c.Length != 4) throw new FormatException("an asset line has 4 columns");
                    var a = new Asset { Id = int.Parse(c[1], CultureInfo.InvariantCulture), Kind = c[2].Length == 1 ? c[2][0] : '?', Path = c[3] };
                    if (a.Id != it.Assets.Count) throw new FormatException("assets are numbered from 0 in order");
                    if (a.Kind != 'M' && a.Kind != 'L') throw new FormatException("an asset is M (a static mesh) or L (a level instance)");
                    if (a.Path.Length == 0 || a.Path[0] != '/') throw new FormatException("an asset path starts with /");
                    it.Assets.Add(a);
                }
                else if (c[0].Length > 0 && c[0][0] == '@') throw new FormatException("an unknown line");
                else
                {
                    if (c.Length != 16) throw new FormatException("a row has 16 columns");
                    long node = long.Parse(c[0], CultureInfo.InvariantCulture);
                    if (!bowlOf.TryGetValue(node, out int bowl)) throw new FormatException("a row names a node that has no @node line");
                    int asset = int.Parse(c[1], CultureInfo.InvariantCulture);
                    if (asset < 0 || asset >= it.Assets.Count) throw new FormatException("a row names an asset that has no @asset line");
                    int levels = int.Parse(c[2], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    if (levels <= 0 || levels > AllLevels) throw new FormatException("the levels of a row are bits 0 to 6");
                    var m = new double[12];
                    for (int i = 0; i < 12; i++)
                    {
                        m[i] = D(c[4 + i]);
                        if (!double.IsFinite(m[i])) throw new FormatException("a number is not finite");
                    }
                    var xf = new Xf(m[0], m[1], m[2], m[9], m[3], m[4], m[5], m[10], m[6], m[7], m[8], m[11]);
                    it.Rows.Add(new Row(bowl, asset, levels, c[3] != "0", xf));
                }
            }
            catch (Exception e) when (e is FormatException or OverflowException or IndexOutOfRangeException)
            {
                throw new InvalidDataException($"{source}, line {n}: {e.Message}");
            }
        }
        return it;
    }

    // ---- the geometry ------------------------------------------------------------------------------------------------------------------------

    /// <summary>Whether a static mesh used with the collision profile "Default" (its own) stops a walking pawn.</summary>
    public static bool Blocks(MeshGeo g)
    {
        if (g.Kind == "none" || g.I.Count == 0) return false;
        if (g.DefaultProfile.Length > 0 && Collector.NonBlockingProfiles.Contains(g.DefaultProfile)) return false;
        if (g.DefaultEnabled.Contains("NoCollision", StringComparison.Ordinal) || g.DefaultEnabled.Contains("PhysicsOnly", StringComparison.Ordinal)) return false;
        return true;
    }

    /// <summary>
    /// The geometry of the rows whose levels meet <paramref name="levelMask"/> (and, with <paramref name="nodes"/>, of those bowls): every blocking mesh placed at its row's transform,
    /// a level instance's meshes put at the row's transform after their own.  No terrain and no region: the file is a layer for area_geom.bin.  The order is the area geometry's
    /// (AreaGeometry.Finish), so the bytes do not depend on the order of the rows.
    /// </summary>
    public static AreaGeometry Compose(Items items, ISource src, int levelMask = AllLevels, ISet<long>? nodes = null, AreaGeometry? into = null)
    {
        var res = into ?? new AreaGeometry();
        var index = new Dictionary<MeshGeo, int>(ReferenceEqualityComparer.Instance);
        int Mesh(MeshGeo g)
        {
            if (!index.TryGetValue(g, out int i)) { i = res.Meshes.Count; res.Meshes.Add(g); index[g] = i; res.Stat("layouts.meshes"); }
            return i;
        }
        var seenLi = new HashSet<int>();
        foreach (var row in items.Rows)
        {
            if ((row.Levels & levelMask) == 0) { res.Stat("layouts.rows.otherLevels"); continue; }
            if (nodes != null && !nodes.Contains(items.Bowls[row.Bowl].Node)) { res.Stat("layouts.rows.otherNodes"); continue; }
            res.Stat("layouts.rows");
            var asset = items.Assets[row.Asset];
            if (!row.StepUp) res.Stat("layouts.rows.noStepUp");
            if (asset.Kind == 'M')
            {
                var mesh = src.Mesh(asset.Path);
                if (mesh == null) { res.Stat("layouts.rows.noCollision"); continue; }
                res.Placements.Add(new Placement(Mesh(mesh), row.M, int.MaxValue));
                res.Stat("layouts.placements");
            }
            else
            {
                if (seenLi.Add(row.Asset)) res.Stat("layouts.levelInstances");
                var parts = src.LevelInstance(asset.Path);
                if (parts.Count == 0) { res.Stat("layouts.rows.emptyLevelInstance"); continue; }
                foreach (var p in parts)
                {
                    res.Placements.Add(new Placement(Mesh(p.Mesh), row.M.Mul(p.Local), int.MaxValue));
                    res.Stat("layouts.placements");
                    res.Stat("layouts.placements.levelInstance");
                }
            }
        }
        res.Finish();
        return res;
    }

    /// <summary>A layer file (<see cref="FileName"/>) as a geometry to merge: its placements rank after every cell's, so that of two that are one the map's wins.</summary>
    public static AreaGeometry ReadLayer(string path)
    {
        var g = GeomFile.Read(File.ReadAllBytes(path));
        for (int i = 0; i < g.Placements.Count; i++) g.Placements[i] = g.Placements[i] with { Src = int.MaxValue };
        return g;
    }

    /// <summary>The package a "/Game/ENV/x/y.y" object path lives in, as the containers name it ("AOC/Content/ENV/x/y" and an extension; /Engine/ paths are under Engine/Content).</summary>
    public static string PackagePath(string objectPath, string extension)
    {
        int dot = objectPath.LastIndexOf('.');
        string pkg = dot > objectPath.LastIndexOf('/') ? objectPath[..dot] : objectPath;
        if (pkg.StartsWith("/Game/", StringComparison.Ordinal)) return "AOC/Content/" + pkg["/Game/".Length..] + extension;
        if (pkg.StartsWith("/Engine/", StringComparison.Ordinal)) return "Engine/Content/" + pkg["/Engine/".Length..] + extension;
        return pkg.TrimStart('/') + extension;
    }

    /// <summary>The object's own name ("SM_Name" of "/Game/.../SM_Name.SM_Name").</summary>
    public static string ObjectName(string objectPath)
    {
        int dot = objectPath.LastIndexOf('.');
        return dot > objectPath.LastIndexOf('/') ? objectPath[(dot + 1)..] : objectPath[(objectPath.LastIndexOf('/') + 1)..];
    }

    // ---- the game's packages -----------------------------------------------------------------------------------------------------------------

    /// <summary>The meshes and level instances from the game's containers, each read once (a package is let go as soon as its geometry is made).</summary>
    public sealed class GameSource : ISource
    {
        readonly Game game;
        readonly AreaGeometry stats;
        readonly Action<string> log;
        readonly Dictionary<string, MeshGeo?> shapes = new(StringComparer.Ordinal);   // every mesh read, blocking by its default or not (null: it has no shape)
        readonly Dictionary<string, MeshGeo?> meshes = new(StringComparer.Ordinal);   // what a layout instance of the mesh does: the shape when the mesh's own profile blocks, else null
        readonly Dictionary<string, List<LiPart>> levelInstances = new(StringComparer.Ordinal);
        int warnings;
        public int Packages;

        public GameSource(Game game, AreaGeometry stats, Action<string> log) { this.game = game; this.stats = stats; this.log = log; }

        void Warn(string text)
        {
            stats.Warnings.Add(text);
            if (++warnings <= 30) log("  warning: " + text);
        }

        public MeshGeo? Mesh(string objectPath)
        {
            if (meshes.TryGetValue(objectPath, out var have)) return have;
            MeshGeo? geo = null;
            string pkgPath = PackagePath(objectPath, ".uasset");
            try
            {
                var pkg = game.Load(pkgPath);
                if (pkg == null) { stats.Stat("layouts.mesh.notInContainers"); Warn($"{objectPath}: the package is not in the game's containers"); }
                else
                {
                    Packages++;
                    string name = ObjectName(objectPath);
                    UStaticMesh? sm = null;
                    foreach (var e in pkg.GetExports())
                        if (e is UStaticMesh m && (sm == null || e.Name == name)) { sm = m; if (e.Name == name) break; }
                    if (sm == null) { stats.Stat("layouts.mesh.noStaticMesh"); Warn($"{objectPath}: the package holds no static mesh"); }
                    else
                    {
                        var shape = Shape(sm, objectPath);
                        if (shape != null && Blocks(shape)) { stats.Stat("layouts.mesh.blocking"); stats.Stat("layouts.mesh.triangles", shape.Tris); geo = shape; }
                        else stats.Stat("layouts.mesh.nonBlocking");
                    }
                }
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                stats.Stat("layouts.mesh.unreadable");
                Warn($"{objectPath}: {ex.GetType().Name}: {ex.Message}");
            }
            finally { game.Forget(pkgPath); }
            meshes[objectPath] = geo;
            return geo;
        }

        /// <summary>A mesh's collision shape by its object path (a hash of the full path in the name: the map's own naming, so a mesh both read is one mesh); null when it has none.  Whether a use of
        /// the mesh blocks is the caller's: a layout instance uses the mesh's own profile, a component of a level may name its own.</summary>
        MeshGeo? Shape(UStaticMesh sm, string objectPath)
        {
            if (shapes.TryGetValue(objectPath, out var have)) return have;
            var bs = Lv.Resolve(sm.BodySetup) as UBodySetup;
            var geo = MeshShapes.Build(sm, $"StaticMesh'{sm.Name}'@{Collector.Fnv32(objectPath)}", bs);
            stats.Stat("layouts.mesh." + geo.Kind);
            if (geo.HugeSkipped > 0) { stats.Stat("layouts.mesh.hugeShapesSkipped", geo.HugeSkipped); stats.Stat("layouts.mesh.withHugeShape"); }
            MeshGeo? shape = geo.Kind == "none" || geo.I.Count == 0 ? null : geo;
            shapes[objectPath] = shape;
            return shape;
        }

        public IReadOnlyList<LiPart> LevelInstance(string objectPath) => LevelInstanceAt(objectPath, 0);

        IReadOnlyList<LiPart> LevelInstanceAt(string objectPath, int depth)
        {
            if (levelInstances.TryGetValue(objectPath, out var have)) return have;
            var parts = new List<LiPart>();
            levelInstances[objectPath] = parts;   // (a level that names itself finds its own, still empty, list)
            if (depth > MaxLevelInstanceDepth) { Warn($"{objectPath}: level instances nested deeper than {MaxLevelInstanceDepth}"); return parts; }
            IPackage? pkg = null;
            string pkgPath = "";
            foreach (var ext in new[] { ".umap", ".uasset" })
            {
                pkgPath = PackagePath(objectPath, ext);
                pkg = game.Load(pkgPath);
                if (pkg != null) break;
                game.Forget(pkgPath);
            }
            if (pkg == null) { stats.Stat("layouts.levelInstance.notInContainers"); Warn($"{objectPath}: the package is not in the game's containers"); return parts; }
            try
            {
                Packages++;
                if (pkgPath.EndsWith(".uasset", StringComparison.Ordinal))
                {
                    // A Blueprint that places a level instance names its world in its script, which is not read here.  By the project's naming the world is the Blueprint's
                    // sibling without the BP_ prefix (BP_Fountain_N4 and Fountain_N4 in the same folder).
                    string name = ObjectName(objectPath);
                    int slash = objectPath.LastIndexOf('/');
                    if (name.StartsWith("BP_", StringComparison.Ordinal) && slash > 0)
                    {
                        string world = name["BP_".Length..];
                        stats.Stat("layouts.levelInstance.blueprint");
                        parts.AddRange(LevelInstanceAt(objectPath[..(slash + 1)] + world + "." + world, depth + 1));
                    }
                    else { stats.Stat("layouts.levelInstance.blueprint.unknown"); Warn($"{objectPath}: a Blueprint whose world is not known"); }
                }
                else ReadLevel(pkg, objectPath, depth, parts);
                stats.Stat("layouts.levelInstance.read");
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                stats.Stat("layouts.levelInstance.unreadable");
                Warn($"{objectPath}: {ex.GetType().Name}: {ex.Message}");
            }
            finally { game.Forget(pkgPath); }
            return parts;
        }

        /// <summary>The static mesh components of a level (an actor's components are its package's exports), with their transforms inside it; a level instance it places is read in turn.</summary>
        void ReadLevel(IPackage pkg, string source, int depth, List<LiPart> parts)
        {
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
                stats.Stat("layouts.levelInstance.components");
                if (e.ExportType == "SplineMeshComponent" || e.ExportType == "PCGManagedSplineMeshComponent") { stats.Stat("layouts.levelInstance.components.spline"); continue; }
                var wx = WorldOf(e);
                bool instanced = e is UInstancedStaticMeshComponent;
                FInstancedStaticMeshInstanceData[]? inst = null;
                if (instanced)
                {
                    inst = ((UInstancedStaticMeshComponent)e).GetInstances();
                    if (inst.Length == 0) { stats.Stat("layouts.levelInstance.components.empty"); continue; }
                }
                var meshIdx = smc.GetStaticMesh();
                var meshObj = Lv.Resolve(meshIdx) as UStaticMesh;
                if (meshObj == null) { stats.Stat("layouts.levelInstance.components.noMesh"); continue; }
                string fullPath = "";
                try { fullPath = meshIdx.ResolvedObject?.GetPathName() ?? ""; } catch { }
                if (fullPath.Length == 0) fullPath = Lv.PathOf(meshIdx);
                var geo = Shape(meshObj, fullPath);
                if (geo == null) { stats.Stat("layouts.levelInstance.components.noCollision"); continue; }
                // the component's own collision profile (a profile of its own overrides the mesh's default)
                if (!Collector.BlocksPawn(e, stats, geo.DefaultProfile, geo.DefaultEnabled)) { stats.Stat("layouts.levelInstance.components.nonBlocking"); continue; }
                if (instanced)
                {
                    foreach (var one in inst!)
                    {
                        var t = one.TransformData;
                        var local = Xf.FromTRS(t.Translation.X, t.Translation.Y, t.Translation.Z, (t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W), t.Scale3D.X, t.Scale3D.Y, t.Scale3D.Z);
                        parts.Add(new LiPart(geo, wx.Mul(local)));
                    }
                }
                else parts.Add(new LiPart(geo, wx));
            }
        }
    }

    // ---- the diagnostic ----------------------------------------------------------------------------------------------------------------------

    /// <summary>mapc town-layouts --probe PATH...: what a package holds (diagnostic; no file written).</summary>
    public static int Probe(Game game, IEnumerable<string> objectPaths, Action<string> log)
    {
        foreach (var objectPath in objectPaths)
        {
            foreach (var ext in new[] { ".umap", ".uasset" })
            {
                string pkgPath = PackagePath(objectPath, ext);
                var pkg = game.Load(pkgPath);
                if (pkg == null) { game.Forget(pkgPath); continue; }
                log($"== {objectPath}: {pkgPath}");
                var exports = pkg.GetExports().ToList();
                var byType = new SortedDictionary<string, int>(StringComparer.Ordinal);
                foreach (var e in exports) byType[e.ExportType] = byType.GetValueOrDefault(e.ExportType) + 1;
                log($"   {exports.Count} exports: " + string.Join(", ", byType.OrderByDescending(k => k.Value).Select(k => k.Key + " x" + k.Value)));
                var shown = new Dictionary<string, int>(StringComparer.Ordinal);
                foreach (var e in exports)
                {
                    bool smc = e is UStaticMeshComponent;
                    if (smc)
                    {
                        var mesh = Lv.PathOf(((UStaticMeshComponent)e).GetStaticMesh());
                        int n = e is UInstancedStaticMeshComponent ism ? ism.GetInstances().Length : 1;
                        if (shown.GetValueOrDefault("smc") < 12) { shown["smc"] = shown.GetValueOrDefault("smc") + 1; log($"   StaticMeshComponent {e.Name} ({e.ExportType}, outer {e.Outer?.Name}) x{n} {mesh} at {Lv.World(e)}"); }
                        continue;
                    }
                    if (e.ExportType is "SceneComponent" or "StaticMesh" or "BodySetup" or "NavCollision") continue;
                    if (e.ExportType is "Function" or "LevelInstanceComponent" or "BlueprintGeneratedClass")
                    {
                        // the package paths a Blueprint's script or properties name (a Blueprint that places a level instance names its world here)
                        var named = GamePaths(e);
                        if (named.Count > 0) log($"   {e.ExportType} {e.Name} names: " + string.Join(", ", named.Take(12)));
                    }
                    if (shown.GetValueOrDefault(e.ExportType) >= 4) continue;
                    shown[e.ExportType] = shown.GetValueOrDefault(e.ExportType) + 1;
                    var props = string.Join(", ", e.Properties.Take(14).Select(p => p.Name.Text + "=" + Brief(p.Tag?.GenericValue)));
                    log($"   {e.ExportType} {e.Name} (outer {e.Outer?.Name}): {props}");
                }
                game.Forget(pkgPath);
                break;
            }
        }
        return 0;
    }

    static readonly System.Text.RegularExpressions.Regex GamePathRx = new(@"/Game/[A-Za-z0-9_/\.\-]+", System.Text.RegularExpressions.RegexOptions.Compiled);

    static List<string> GamePaths(UObject e)
    {
        try
        {
            string s = Newtonsoft.Json.JsonConvert.SerializeObject(e, Newtonsoft.Json.Formatting.None, new Newtonsoft.Json.JsonSerializerSettings { ReferenceLoopHandling = Newtonsoft.Json.ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true });
            return GamePathRx.Matches(s).Select(m => m.Value).Distinct(StringComparer.Ordinal).Where(p => !p.Contains("/Script/", StringComparison.Ordinal)).ToList();
        }
        catch (Exception) { return new List<string>(); }
    }

    static string Brief(object? v)
    {
        try
        {
            string s = Newtonsoft.Json.JsonConvert.SerializeObject(v, Newtonsoft.Json.Formatting.None, new Newtonsoft.Json.JsonSerializerSettings { ReferenceLoopHandling = Newtonsoft.Json.ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true });
            return s.Length > 160 ? s[..160] + "..." : s;
        }
        catch (Exception ex) { return "(" + ex.Message + ")"; }
    }

    /// <summary>The numbers of a composed geometry, for the log.</summary>
    public static string Describe(AreaGeometry g)
    {
        long tris = g.Placements.Sum(p => (long)g.Meshes[p.Mesh].Tris);
        return $"{g.Meshes.Count} meshes ({g.Meshes.Sum(m => (long)m.Tris)} triangles), {g.Placements.Count} placements ({tris} triangles placed)";
    }
}
