// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Exports.Component;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>Property and transform helpers over CUE4Parse's objects.</summary>
public static class Lv
{
    /// <summary>A property of the object, else of its archetype chain (a blueprint component keeps its defaults there).</summary>
    public static bool TryProp<T>(UObject? o, string name, out T value)
    {
        for (int depth = 0; o != null && depth < 8; depth++)
        {
            if (o.TryGetValue(out T v, name)) { value = v; return true; }
            o = o.Template?.Object?.Value;
        }
        value = default!;
        return false;
    }

    /// <summary>A field of a struct property (BodyInstance), searched along the archetype chain field by field: a blueprint child that sets only
    /// one field of the struct leaves the others to its parents.</summary>
    public static bool TryStructField<T>(UObject? o, string structName, string field, out T value)
    {
        for (int depth = 0; o != null && depth < 8; depth++)
        {
            if (o.TryGetValue(out FStructFallback st, structName) && st != null && st.TryGetValue(out T v, field)) { value = v; return true; }
            o = o.Template?.Object?.Value;
        }
        value = default!;
        return false;
    }

    public static T Prop<T>(UObject? o, string name, T def)
        => TryProp<T>(o, name, out var v) ? v : def;

    public static UObject? Resolve(FPackageIndex? idx)
    {
        if (idx == null || idx.IsNull) return null;
        try { return idx.ResolvedObject?.Object?.Value; } catch { return null; }
    }

    /// <summary>A component's relative transform: loc and rot from the object or its archetype, scale likewise.</summary>
    public static Xf Relative(UObject comp)
    {
        double tx = 0, ty = 0, tz = 0, p = 0, yw = 0, r = 0, sx = 1, sy = 1, sz = 1;
        if (TryProp<FVector>(comp, "RelativeLocation", out var l)) { tx = l.X; ty = l.Y; tz = l.Z; }
        if (TryProp<FRotator>(comp, "RelativeRotation", out var rot)) { p = rot.Pitch; yw = rot.Yaw; r = rot.Roll; }
        if (TryProp<FVector>(comp, "RelativeScale3D", out var s)) { sx = s.X; sy = s.Y; sz = s.Z; }
        return Xf.FromRotator(p, yw, r, tx, ty, tz, sx, sy, sz);
    }
// -- C | Y | B x C | O | A --

    /// <summary>The component's world transform: its relative transform after every AttachParent's (to the root); an absolute flag stops the chain's rotation/scale/location per UE.</summary>
    public static Xf World(UObject comp, int depth = 0)
    {
        var rel = Relative(comp);
        if (depth > 24) return rel;
        var parent = Resolve(Prop<FPackageIndex?>(comp, "AttachParent", null));
        if (parent == null || parent == comp) return rel;
        if (Prop(comp, "bAbsoluteLocation", false) || Prop(comp, "bAbsoluteRotation", false) || Prop(comp, "bAbsoluteScale", false))
            return AbsoluteMix(comp, rel, World(parent, depth + 1));
        return World(parent, depth + 1).Mul(rel);
    }

    static Xf AbsoluteMix(UObject comp, Xf rel, Xf parent)
    {
        // rarely used on placed meshes: keep the relative scale/rotation, but a flagged part ignores the parent
        bool al = Prop(comp, "bAbsoluteLocation", false), ar = Prop(comp, "bAbsoluteRotation", false), asc = Prop(comp, "bAbsoluteScale", false);
        var composed = parent.Mul(rel);
        if (!al && !ar && !asc) return composed;
        return new Xf(
            ar || asc ? rel.M00 : composed.M00, ar || asc ? rel.M01 : composed.M01, ar || asc ? rel.M02 : composed.M02, al ? rel.M03 : composed.M03,
            ar || asc ? rel.M10 : composed.M10, ar || asc ? rel.M11 : composed.M11, ar || asc ? rel.M12 : composed.M12, al ? rel.M13 : composed.M13,
            ar || asc ? rel.M20 : composed.M20, ar || asc ? rel.M21 : composed.M21, ar || asc ? rel.M22 : composed.M22, al ? rel.M23 : composed.M23);
    }

    public static string PathOf(FPackageIndex? idx)
    {
        if (idx == null || idx.IsNull) return "";
        try { return idx.ResolvedObject?.GetFullName(false) ?? idx.Name ?? ""; } catch { return ""; }
    }
}
