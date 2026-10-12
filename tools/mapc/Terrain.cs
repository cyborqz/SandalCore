// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component.Landscape;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>One landscape component's heights: a regular grid of 16-bit samples (UE's heightmap encoding), in world cm.</summary>
public sealed class TerrainTile
{
    public double X0, Y0;          // world position of vertex (0, 0)
    public double StepX, StepY;    // cm between vertices
    public int Nx, Ny;             // vertices per side
    public double ZBase, ZScale;   // world z = ZBase + (raw - 32768) * ZScale
    public ushort[] Raw = Array.Empty<ushort>();
    public string Source = "";
    public bool[]? Hole;           // a vertex under the visibility layer (a cave mouth, a pit), when the component has one

    public double Z(int ix, int iy) => ZBase + (Raw[iy * Nx + ix] - 32768) * ZScale;
    public double X1 => X0 + (Nx - 1) * StepX;
    public double Y1 => Y0 + (Ny - 1) * StepY;

    public bool TryHeight(double x, double y, out double z)
    {
        z = 0;
        double fx = (x - X0) / StepX, fy = (y - Y0) / StepY;
        if (fx < 0 || fy < 0 || fx > Nx - 1 || fy > Ny - 1) return false;
        int ix = Math.Min((int)fx, Nx - 2), iy = Math.Min((int)fy, Ny - 2);
        double tx = fx - ix, ty = fy - iy;
        double z00 = Z(ix, iy), z10 = Z(ix + 1, iy), z01 = Z(ix, iy + 1), z11 = Z(ix + 1, iy + 1);
        // UE splits each quad along its (0,0)-(1,1) diagonal
        z = tx >= ty ? z00 + (z10 - z00) * tx + (z11 - z10) * ty : z00 + (z11 - z01) * tx + (z01 - z00) * ty;
        return true;
    }
}

public static class Terrain
{
    public static IEnumerable<TerrainTile> Collect(IPackage pkg, string source, Action<string>? warn = null)
    {
        var exports = pkg.GetExports().ToList();
        // a heightmap texture's data lives in the LandscapeTextureStorageProviderFactory whose outer it is
        var factories = new Dictionary<string, ULandscapeTextureStorageProviderFactory>();
        foreach (var e in exports)
            if (e is ULandscapeTextureStorageProviderFactory f && e.Outer != null) factories[e.Outer.Name] = f;

        foreach (var e in exports)
        {
            if (e.ExportType != "LandscapeComponent") continue;
            var tex = Lv.Resolve(Lv.Prop<FPackageIndex?>(e, "HeightmapTexture", null));
            if (tex == null) { warn?.Invoke($"{source}: {e.Name} has no heightmap"); continue; }
            if (!factories.TryGetValue(tex.Name, out var fac) || fac.Mips.Length == 0) { warn?.Invoke($"{source}: {e.Name}: no storage provider for {tex.Name}"); continue; }
// CoA with C:Y:B

            int quads = Lv.Prop(e, "ComponentSizeQuads", 63);
            int subQuads = Lv.Prop(e, "SubsectionSizeQuads", 63);
            int numSub = Lv.Prop(e, "NumSubsections", 1);
            var sb = Lv.Prop(e, "HeightmapScaleBias", new FVector4(0, 0, 0, 0));
            var mip = fac.Mips[0];
            var src = mip.BulkData.Data;
            if (src == null || src.Length == 0) { warn?.Invoke($"{source}: {e.Name}: no mip data"); continue; }
            int w = mip.SizeX, h = mip.SizeY;
            byte[] px;
            if (mip.bCompressed)
            {
                px = new byte[w * h * 4];
                fac.DecompressMip(src, src.Length, px, px.Length, 0);
            }
            else px = src;

            var xf = Lv.World(e);
            if (Math.Abs(xf.M01) > 1e-6 || Math.Abs(xf.M10) > 1e-6) warn?.Invoke($"{source}: {e.Name}: a rotated landscape component");
            int nv = quads + 1;
            int texX0 = (int)Math.Round(sb.Z * w), texY0 = (int)Math.Round(sb.W * h);
            var t = new TerrainTile
            {
                X0 = xf.M03, Y0 = xf.M13, StepX = xf.M00, StepY = xf.M11, Nx = nv, Ny = nv,
                ZBase = xf.M23, ZScale = xf.M22 / 128.0, Raw = new ushort[nv * nv], Source = source + ":" + e.Name,
            };
            for (int vy = 0; vy < nv; vy++)
                for (int vx = 0; vx < nv; vx++)
                {
                    int sx = Math.Min(vx / subQuads, numSub - 1), sy = Math.Min(vy / subQuads, numSub - 1);
                    int lx = vx - sx * subQuads, ly = vy - sy * subQuads;
                    int tx = texX0 + sx * (subQuads + 1) + lx, ty = texY0 + sy * (subQuads + 1) + ly;
                    int o = (ty * w + tx) * 4;
                    t.Raw[vy * nv + vx] = (ushort)(px[o + 2] * 256 + px[o + 1]);
                }
            yield return t;
        }
    }
}
