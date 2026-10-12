// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.StaticMesh;
using CUE4Parse.UE4.Objects.PhysicsEngine;
using Newtonsoft.Json;

namespace Mapc;

public static class Inspect
{
    static string Js(object o, int max = 3000)
    {
        var js = JsonConvert.SerializeObject(o, Formatting.Indented);
        return js.Length > max ? js[..max] + "..." : js;
    }

    public static string? FindAsset(Game g, string name)
    {
        var suffix = "/" + name + ".uasset";
        foreach (var k in g.Provider.Files.Keys)
            if (k.EndsWith(suffix, StringComparison.OrdinalIgnoreCase)) return k;
        return null;
    }

    public static bool Verbose;
    public static void Run(Game g, string[] args)
    {
        Verbose = args.Contains("-v"); args = args.Where(a => a != "-v").ToArray();
        foreach (var name in args.Skip(1))
        {
            var path = FindAsset(g, name);
            Console.WriteLine("==== " + name + " -> " + path);
            if (path == null) continue;
            var pkg = g.Load(path);
            if (pkg == null) { Console.WriteLine("  not loadable"); continue; }
            foreach (var e in pkg.GetExports())
            {
                if (e is UStaticMesh sm)
                {
                    Console.WriteLine($"  StaticMesh: cooked={sm.bCooked} LODForCollision={sm.LODForCollision} bodySetup={sm.BodySetup?.Name} nav={sm.NavCollision?.Name}");
                    var rd = sm.RenderData;
                    if (rd != null)
                    {
                        Console.WriteLine($"  render: LODs={rd.LODs?.Length} nanite={(rd.NaniteResources != null)} bounds={rd.Bounds?.BoxExtent}");
                        if (rd.LODs != null)
                            for (int i = 0; i < rd.LODs.Length; i++)
                            {
                                var l = rd.LODs[i];
                                Console.WriteLine($"   LOD{i}: skip={l.SkipLod} sections={l.Sections.Length} verts={l.PositionVertexBuffer?.Verts.Length} idx={l.IndexBuffer?.Length}");
                            }
                    }
                    Console.WriteLine("  props: " + Js(sm.Properties.Select(p => p.Name.Text).ToArray(), 800));
                }
                else if (e is UBodySetup bs)
                {
                    Console.WriteLine("  BodySetup " + bs.Name + " traceFlag=" + (bs.Properties.FirstOrDefault(p => p.Name.Text == "CollisionTraceFlag")?.Tag?.GenericValue?.ToString() ?? "(default)") + (Verbose ? " props: " + Js(bs, 2500) : ""));
                    if (bs.AggGeom != null)
                        Console.WriteLine($"   agg: sph={bs.AggGeom.SphereElems.Length} box={bs.AggGeom.BoxElems.Length} sphyl={bs.AggGeom.SphylElems.Length} convex={bs.AggGeom.ConvexElems.Length} (verts {string.Join(",", bs.AggGeom.ConvexElems.Select(c => c.VertexData.Length))})");
                    Console.WriteLine("   cooked formats: " + (bs.CookedFormatData == null ? "null" : string.Join(", ", bs.CookedFormatData.Formats.Select(f => f.Key + ":" + (f.Value.Data?.Length ?? -1)))));
                }
                else if (e.ExportType != "StaticMesh") Console.WriteLine("  other export " + e.ExportType + " " + e.Name + (Verbose ? " " + Js(e, 30000) : ""));
            }
        }
    }
// -- C.O.A & C.Y.B --


    /// <summary>Everything one Blueprint's class default object and its components set, untruncated (diagnostic): `mapc bp NAME [TYPE]`.</summary>
    public static void Bp(Game g, string name, string? onlyType)
    {
        var path = FindAsset(g, name);
        Console.WriteLine("==== " + name + " -> " + path);
        if (path == null) return;
        var pkg = g.Load(path);
        if (pkg == null) { Console.WriteLine("  not loadable"); return; }
        foreach (var e in pkg.GetExports())
        {
            if (onlyType != null && e.ExportType != onlyType && e.Name != onlyType) continue;
            Console.WriteLine("-- " + e.ExportType + " " + e.Name + " (outer " + e.Outer?.Name + ", template " + e.Template?.GetFullName(false) + ")");
            foreach (var pp in e.Properties) if (pp.Name.Text == "StartingAlignments") { Console.WriteLine("  StartingAlignments tag " + pp.Tag?.GetType().Name + " value " + pp.Tag?.GenericValue?.GetType().FullName); if (pp.Tag?.GenericValue is CUE4Parse.UE4.Assets.Objects.UScriptArray ua) foreach (var it in ua.Properties) Console.WriteLine("   item " + it.GetType().Name + " " + it.GenericValue?.GetType().FullName + " " + ((it.GenericValue as CUE4Parse.UE4.Assets.Objects.FScriptStruct)?.StructType?.GetType().FullName)); }
            try { Console.WriteLine(JsonConvert.SerializeObject(e.Properties.ToDictionary(p => p.Name.Text, p => p.Tag?.GenericValue), Formatting.None, new JsonSerializerSettings { ReferenceLoopHandling = ReferenceLoopHandling.Ignore, Error = (s, a) => a.ErrorContext.Handled = true })); }
            catch (Exception ex) { Console.WriteLine("  (props: " + ex.Message + ")"); }
        }
    }

    /// <summary>Every component near the login spot that uses a mesh whose path contains `meshName`: its type, its actor and its properties.</summary>
    public static void Uses(Game g, string meshName, double nearCm, int max)
    {
        var ids = Cells.Hit(g, Cli.CenterX, Cli.CenterY, Cli.RadiusCm, 15000, out var grids).ToList();
        var pkgs = new List<(string, CUE4Parse.UE4.Assets.IPackage)> { ("MAIN", g.Load(Game.MasterPath + ".umap")!) };
        foreach (var id in ids) { if (grids[id] is "MainGrid") { var p = g.Load(Cells.PathOf(id)); if (p != null) pkgs.Add((id, p)); } }
        int shown = 0;
        foreach (var (name, pkg) in pkgs)
            foreach (var e in pkg.GetExports())
            {
                if (e is not CUE4Parse.UE4.Assets.Exports.Component.StaticMesh.UStaticMeshComponent smc) continue;
                var mp = Lv.PathOf(smc.GetStaticMesh());
                if (!mp.Contains(meshName)) continue;
                var w = Lv.World(e);
                if (Math.Sqrt((w.M03 - Cli.CenterX) * (w.M03 - Cli.CenterX) + (w.M13 - Cli.CenterY) * (w.M13 - Cli.CenterY)) > nearCm) continue;
                if (shown++ >= max) return;
                Console.WriteLine($"==== {name} {e.ExportType} {e.Name} outer={e.Outer?.Name} ({e.Outer?.ExportType}) mesh={mp} at {w}");
                Console.WriteLine(Js(e, 2500));
                var tpl = e.Template?.Object?.Value;
                Console.WriteLine("  template: " + (e.Template?.GetFullName(false) ?? "null") + " loaded=" + (tpl != null) + (tpl != null ? " props " + Js(tpl, 1800) : ""));
                var actor = e.Outer;
                if (actor != null) Console.WriteLine("  actor props: " + Js(actor.Properties.Select(p => p.Name.Text).ToArray(), 600));
            }
    }
}
