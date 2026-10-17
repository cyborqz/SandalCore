// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using System.Globalization;
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component.Landscape;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;
using Newtonsoft.Json;

namespace Mapc;

/// <summary>
/// mapc town-floors: the floor of the five city nodes' bowls.  The floor of a town is not the baked landscape lifted by 50 m: it is the layout's own
/// "Landscape Platform" sublevel, one per LAYOUT (two files serve all five bowls), which the client's ANodeLayoutReplicator loads at the node.  This reads ONLY those two umaps (48 MB,
/// seconds; never a cell of Verra_World_Master, never the 14 GB map read): the landscape components' 16-bit heights through Terrain.Collect and the landscape's visibility layer
/// (the holes, which Terrain.Collect does not read), and writes dist/content/town_platforms.bin (format: server/worldd/world/town_platform.h).
/// </summary>
public static class TownFloors
{
    public const string FileName = "town_platforms.bin";

    /// <summary>One layout's platform: the layout it serves and the package that holds it.</summary>
    public sealed record Source(string Layout, string Package);

    const string Dir = "AOC/Content/ENV/Nodes/Nodes_Master/Node_Sublevels/Landscape_Platforms/";

    /// <summary>Layout_Flat_D (layout prop 14 NS_Flat_D_Master_01_Landscape: Winstead, Joeva, New Aela) and Layout_River_C (prop 53 NS_River_C_Landscape: Halcyon, Miraleth): the
    /// Riverlands' variant of each (the other variants in the folders are for other biomes).</summary>
    public static readonly Source[] Sources =
    {
        new("Layout_Flat_D", Dir + "Layout_Flat_D/Landscape_Flat_D_Master_01.umap"),
        new("Layout_River_C", Dir + "Layout_River_C/Landscape_River_C_Master_01.umap"),
    };

    /// <summary>The visibility layer's weight from which a vertex is a hole (UE's shader clips the landscape where the layer is 0.5 or more).</summary>
    public const int HoleWeight = 128;

    /// <summary>A landscape component of a platform: the heights (Terrain.Collect's tile) and its holes.</summary>
    public sealed class Component
    {
        public TerrainTile Tile = new();
        public byte[]? Visibility;      // the visibility layer's weight per vertex (0..255); null when the component has no such layer
    }

    public sealed class Platform
    {
        public string Layout = "", Package = "";
        public List<Component> Components = new();
        public List<string> Notes = new();
        public long[] WeightHistogram = new long[4];   // visibility weights over the vertices of components that have the layer: 0, 1..127, 128..254, 255
        public int ComponentsWithLayer;
        public double MaxEdgeMismatchCm;               // the largest height difference between two components' shared border vertices
        public string Summary = "";
    }

    // ---- reading ------------------------------------------------------------------------------------------------------------------------------

    /// <summary>Reads one platform's package: the heights, and the visibility layer where a component has one.  Components are sorted by their place, so the file does not depend on
    /// the package's export order.</summary>
    public static Platform Read(Game game, Source src, Action<string> log)
    {
        var res = new Platform { Layout = src.Layout, Package = src.Package };
        var pkg = game.Load(src.Package) ?? throw new InvalidOperationException($"the package {src.Package} is not in the game's containers");
        var exports = pkg.GetExports().ToList();
        var tiles = Terrain.Collect(pkg, src.Layout, w => res.Notes.Add(w)).ToList();
        // the components by name, to find each tile's visibility weights (Terrain.Collect names a tile "<source>:<component>")
        var byName = new Dictionary<string, UObject>(StringComparer.Ordinal);
        foreach (var e in exports) if (e.ExportType == "LandscapeComponent") byName[src.Layout + ":" + e.Name] = e;
        foreach (var t in tiles)
        {
            var c = new Component { Tile = t };
            if (byName.TryGetValue(t.Source, out var e) && e is ULandscapeComponent lc) c.Visibility = ReadVisibility(lc, t, res);
            res.Components.Add(c);
        }
        res.Components.Sort((a, b) => a.Tile.Y0 != b.Tile.Y0 ? a.Tile.Y0.CompareTo(b.Tile.Y0) : a.Tile.X0.CompareTo(b.Tile.X0));
        foreach (var c in res.Components) if (c.Visibility != null) res.ComponentsWithLayer++;
        res.MaxEdgeMismatchCm = EdgeMismatch(res.Components);
        game.Forget(src.Package);
        return res;
    }
// -- CoA & C | Y | B --

    /// <summary>The visibility layer's weight at every vertex of a component (the layer info named "LandscapeVisibilityLayerInfo" in the component's weightmap allocations): the weightmap
    /// texture is one per component here (64 x 64 for 64 x 64 vertices, BGRA8), channel c is the byte R=2, G=1, B=0, A=3 of the texel (UE's FColor offsets).</summary>
    static byte[]? ReadVisibility(ULandscapeComponent lc, TerrainTile t, Platform res)
    {
        FWeightmapLayerAllocationInfo? alloc = null;
        foreach (var a in lc.WeightmapLayerAllocations)
            if (a.GetLayerName().Contains("Visibility", StringComparison.OrdinalIgnoreCase)) { alloc = a; break; }
        if (alloc == null) return null;
        var texs = lc.GetWeightmapTextures();
        if (alloc.WeightmapTextureIndex >= texs.Length || texs[alloc.WeightmapTextureIndex] == null) { res.Notes.Add($"{t.Source}: the visibility layer names a missing weightmap texture"); return null; }
        var tex = texs[alloc.WeightmapTextureIndex];
        var mip = tex.GetFirstMip();
        var data = mip?.BulkData?.Data;
        if (mip == null || data == null) { res.Notes.Add($"{t.Source}: the visibility weightmap has no data"); return null; }
        if (tex.PlatformData.PixelFormat != "PF_B8G8R8A8" || data.Length < mip.SizeX * mip.SizeY * 4) { res.Notes.Add($"{t.Source}: the visibility weightmap is {tex.PlatformData.PixelFormat}, {data.Length} bytes: not read"); return null; }
        int quads = Lv.Prop(lc, "ComponentSizeQuads", 63), subQuads = Lv.Prop(lc, "SubsectionSizeQuads", 63), numSub = Lv.Prop(lc, "NumSubsections", 1);
        var sb = Lv.Prop(lc, "WeightmapScaleBias", new FVector4(0, 0, 0, 0));
        int w = mip.SizeX;
        int texX0 = (int)Math.Floor(sb.Z * w), texY0 = (int)Math.Floor(sb.W * mip.SizeY);   // (0.5 texel: the component's own texture starts at 0)
        int channel = alloc.WeightmapTextureChannel switch { 0 => 2, 1 => 1, 2 => 0, _ => 3 };
        int nv = quads + 1;
        var vis = new byte[nv * nv];
        for (int vy = 0; vy < nv; vy++)
            for (int vx = 0; vx < nv; vx++)
            {
                int sx = Math.Min(vx / subQuads, numSub - 1), sy = Math.Min(vy / subQuads, numSub - 1);
                int lx = vx - sx * subQuads, ly = vy - sy * subQuads;
                int tx = texX0 + sx * (subQuads + 1) + lx, ty = texY0 + sy * (subQuads + 1) + ly;
                byte v = data[(ty * w + tx) * 4 + channel];
                vis[vy * nv + vx] = v;
                res.WeightHistogram[v == 0 ? 0 : v < HoleWeight ? 1 : v < 255 ? 2 : 3]++;
            }
        return vis;
    }

    /// <summary>Two neighbouring components share a border row of vertices (UE's components overlap by one vertex): the largest difference of the heights there, a check of the decode and
    /// of the components' places.</summary>
    static double EdgeMismatch(List<Component> comps)
    {
        double worst = 0;
        var at = new Dictionary<(long, long), Component>();
        foreach (var c in comps) at[((long)Math.Round(c.Tile.X0 / c.Tile.StepX), (long)Math.Round(c.Tile.Y0 / c.Tile.StepY))] = c;
        foreach (var c in comps)
        {
            var t = c.Tile;
            long kx = (long)Math.Round(t.X0 / t.StepX), ky = (long)Math.Round(t.Y0 / t.StepY);
            if (at.TryGetValue((kx + t.Nx - 1, ky), out var right))
                for (int iy = 0; iy < t.Ny; iy++) worst = Math.Max(worst, Math.Abs(t.Z(t.Nx - 1, iy) - right.Tile.Z(0, iy)));
            if (at.TryGetValue((kx, ky + t.Ny - 1), out var up))
                for (int ix = 0; ix < t.Nx; ix++) worst = Math.Max(worst, Math.Abs(t.Z(ix, t.Ny - 1) - up.Tile.Z(ix, 0)));
        }
        return worst;
    }

    // ---- writing ------------------------------------------------------------------------------------------------------------------------------

    /// <summary>The file (server/worldd/world/town_platform.h): little-endian; "AOCTPF01", u32 version 1, u32 layouts; per layout u16 name length + name, u16 package length + package,
    /// u32 components, and per component  f64 x0 y0 stepX stepY zBase zScale, u32 nx ny, u32 flags (bit 0: it has holes), u16 raw[nx*ny] (world z = zBase + (raw - 32768) * zScale, x fastest),
    /// then, with the holes flag, a bit per vertex (LSB first, 1 = the visibility weight is 128 or more: a hole).  Coordinates are the platform umap's own (the node's frame decides where it stands).</summary>
    public static byte[] Write(IReadOnlyList<Platform> platforms)
    {
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms, Encoding.ASCII, true);
        w.Write(Encoding.ASCII.GetBytes("AOCTPF01"));
        w.Write(1u);
        w.Write((uint)platforms.Count);
        foreach (var p in platforms)
        {
            var name = Encoding.ASCII.GetBytes(p.Layout); w.Write((ushort)name.Length); w.Write(name);
            var pkg = Encoding.ASCII.GetBytes(p.Package); w.Write((ushort)pkg.Length); w.Write(pkg);
            w.Write((uint)p.Components.Count);
            foreach (var c in p.Components)
            {
                var t = c.Tile;
                w.Write(t.X0); w.Write(t.Y0); w.Write(t.StepX); w.Write(t.StepY); w.Write(t.ZBase); w.Write(t.ZScale);
                w.Write((uint)t.Nx); w.Write((uint)t.Ny);
                bool any = c.Visibility != null && c.Visibility.Any(v => v >= HoleWeight);
                w.Write(any ? 1u : 0u);
                foreach (var r in t.Raw) w.Write(r);
                if (any)
                {
                    var bits = new byte[(t.Raw.Length + 7) / 8];
                    for (int i = 0; i < t.Raw.Length; i++) if (c.Visibility![i] >= HoleWeight) bits[i >> 3] |= (byte)(1 << (i & 7));
                    w.Write(bits);
                }
            }
        }
        w.Flush();
        return ms.ToArray();
    }
// built by C_Y_B with CoA

    /// <summary>The numbers of one platform, for the log (and the README).</summary>
    public static string Describe(Platform p)
    {
        var tiles = p.Components.Select(c => c.Tile).ToList();
        double zmin = double.MaxValue, zmax = double.MinValue;
        foreach (var t in tiles) for (int i = 0; i < t.Raw.Length; i++) { double z = t.ZBase + (t.Raw[i] - 32768) * t.ZScale; zmin = Math.Min(zmin, z); zmax = Math.Max(zmax, z); }
        // the holes over the platform's lattice (neighbouring components share a border row: a vertex counts once), and how near to the platform's origin the nearest one is
        var holes = new HashSet<long>();
        long holeComps = 0;
        double nearest = double.MaxValue;
        foreach (var c in p.Components)
        {
            if (c.Visibility == null) continue;
            var t = c.Tile;
            long before = holes.Count;
            for (int iy = 0; iy < t.Ny; iy++)
                for (int ix = 0; ix < t.Nx; ix++)
                {
                    if (c.Visibility[iy * t.Nx + ix] < HoleWeight) continue;
                    double x = t.X0 + ix * t.StepX, y = t.Y0 + iy * t.StepY;
                    holes.Add((long)Math.Round(x / t.StepX) * 1_000_000L + (long)Math.Round(y / t.StepY));
                    nearest = Math.Min(nearest, Math.Sqrt(x * x + y * y));
                }
            if (holes.Count > before) holeComps++;
        }
        long holeVerts = holes.Count;
        var sb = new StringBuilder();
        sb.Append(FormattableString.Invariant($"{p.Layout}: {p.Components.Count} components of {string.Join(",", tiles.Select(t => $"{t.Nx}x{t.Ny}").Distinct())} vertices at {string.Join(",", tiles.Select(t => $"{t.StepX:F0}x{t.StepY:F0}").Distinct())} cm; "));
        sb.Append(FormattableString.Invariant($"x {tiles.Min(t => t.X0):F0}..{tiles.Max(t => t.X1):F0}, y {tiles.Min(t => t.Y0):F0}..{tiles.Max(t => t.Y1):F0} cm (platform-local); z {zmin:F0}..{zmax:F0} cm; "));
        sb.Append(FormattableString.Invariant($"largest height difference across two components' shared border vertices {p.MaxEdgeMismatchCm:F2} cm; "));
        sb.Append(FormattableString.Invariant($"visibility layer in {p.ComponentsWithLayer} components (weights counted per component, a shared border vertex in each: 0: {p.WeightHistogram[0]}, 1..127: {p.WeightHistogram[1]}, 128..254: {p.WeightHistogram[2]}, 255: {p.WeightHistogram[3]}); holes: {holeVerts} vertices of the lattice in {holeComps} components, the nearest {nearest / 100.0:F1} m from the platform's origin"));
        return sb.ToString();
    }

    /// <summary>The whole job: both platforms read, the file's bytes made.  Prints the numbers.</summary>
    public static byte[] Build(Game game, Action<string> log, out int notes)
    {
        var sw = Stopwatch.StartNew();
        var platforms = new List<Platform>();
        notes = 0;
        foreach (var src in Sources)
        {
            var p = Read(game, src, log);
            log("  " + Describe(p));
            foreach (var n in p.Notes) { log("  note: " + n); notes++; }
            platforms.Add(p);
            GC.Collect();
        }
        var bytes = Write(platforms);
        log($"  town-floors: {bytes.Length} bytes in {sw.Elapsed.TotalSeconds:F1} s");
        return bytes;
    }

    // ---- the probe ----------------------------------------------------------------------------------------------------------------------------

    /// <summary>mapc town-floors --probe: what the two umaps hold (diagnostic; no file written).</summary>
    public static int Probe(Game game, Action<string> log)
    {
        foreach (var src in Sources)
        {
            log($"== {src.Layout}: {src.Package}");
            var pkg = game.Load(src.Package);
            if (pkg == null) { log("   not loadable"); continue; }
            var exports = pkg.GetExports().ToList();
            var byType = new SortedDictionary<string, int>(StringComparer.Ordinal);
            foreach (var e in exports) byType[e.ExportType] = byType.GetValueOrDefault(e.ExportType) + 1;
            log($"   {exports.Count} exports: " + string.Join(", ", byType.OrderByDescending(k => k.Value).Take(30).Select(k => k.Key + " x" + k.Value)));
            foreach (var e in exports)
            {
                if (e.ExportType == "SceneComponent" && e.Name == "RootComponent0")
                    log($"   -- root component {e.Name} (outer {e.Outer?.Name}): world {Lv.World(e)}; " + string.Join(", ", e.Properties.Select(p => p.Name.Text + "=" + Brief(p.Tag?.GenericValue))));
                if (e.ExportType == "Landscape")
                    foreach (var p in e.Properties)
                        if (p.Name.Text is "LandscapeGuid" or "LandscapeMaterial" or "ComponentSizeQuads" or "SubsectionSizeQuads" or "NumSubsections" or "TargetLayers")
                            log($"      {e.Name}.{p.Name.Text} = {Brief(p.Tag?.GenericValue)}");
            }
            var comps = exports.Where(e => e.ExportType == "LandscapeComponent").OfType<ULandscapeComponent>().ToList();
            var layers = new SortedDictionary<string, int>(StringComparer.Ordinal);
            foreach (var lc in comps) foreach (var a in lc.WeightmapLayerAllocations) layers[a.GetLayerName()] = layers.GetValueOrDefault(a.GetLayerName()) + 1;
            log($"   {comps.Count} LandscapeComponents; layers (components using each): " + string.Join(", ", layers.Select(k => k.Key + " x" + k.Value)));
            game.Forget(src.Package);
        }
        return 0;
    }

    static string Brief(object? v)
    {
        try
        {
            string s = JsonConvert.SerializeObject(v, Formatting.None, new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s2, a) => a.ErrorContext.Handled = true });
            return s.Length > 300 ? s[..300] + "..." : s;
        }
        catch (Exception ex) { return "(" + ex.Message + ")"; }
    }
}
