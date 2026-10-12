// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.UE4.Objects.Core.Math;

namespace Mapc;

/// <summary>A 3x4 affine transform in double precision: p' = R p + T (R holds the scale), a column-vector convention.</summary>
public readonly struct Xf
{
    public readonly double M00, M01, M02, M03, M10, M11, M12, M13, M20, M21, M22, M23;

    public Xf(double m00, double m01, double m02, double m03, double m10, double m11, double m12, double m13, double m20, double m21, double m22, double m23)
    {
        M00 = m00; M01 = m01; M02 = m02; M03 = m03; M10 = m10; M11 = m11; M12 = m12; M13 = m13; M20 = m20; M21 = m21; M22 = m22; M23 = m23;
    }

    public static readonly Xf Identity = new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);

    /// <summary>UE's FRotator (degrees: pitch about Y, yaw about Z, roll about X) as its quaternion, double precision.</summary>
    public static (double x, double y, double z, double w) QuatOf(double pitch, double yaw, double roll)
    {
        const double d2r = Math.PI / 180.0 * 0.5;
        double sp = Math.Sin(pitch * d2r), cp = Math.Cos(pitch * d2r);
        double sy = Math.Sin(yaw * d2r), cy = Math.Cos(yaw * d2r);
        double sr = Math.Sin(roll * d2r), cr = Math.Cos(roll * d2r);
        return (cr * sp * sy - sr * cp * cy,
                -cr * sp * cy - sr * cp * sy,
                cr * cp * sy - sr * sp * cy,
                cr * cp * cy + sr * sp * sy);
    }

    public static Xf FromTRS(double tx, double ty, double tz, (double x, double y, double z, double w) q, double sx, double sy, double sz)
    {
        double x = q.x, y = q.y, z = q.z, w = q.w;
        double n = x * x + y * y + z * z + w * w;
        if (n > 0) { double inv = 1.0 / Math.Sqrt(n); x *= inv; y *= inv; z *= inv; w *= inv; }
        double r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
        double r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
        double r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
        return new Xf(r00 * sx, r01 * sy, r02 * sz, tx, r10 * sx, r11 * sy, r12 * sz, ty, r20 * sx, r21 * sy, r22 * sz, tz);
    }
// C.Y.B x CoA

    public static Xf FromRotator(double pitch, double yaw, double roll, double tx, double ty, double tz, double sx, double sy, double sz)
        => FromTRS(tx, ty, tz, QuatOf(pitch, yaw, roll), sx, sy, sz);

    /// <summary>this applied after b: (this * b)(p) = this(b(p)).</summary>
    public Xf Mul(in Xf b) => new(
        M00 * b.M00 + M01 * b.M10 + M02 * b.M20, M00 * b.M01 + M01 * b.M11 + M02 * b.M21, M00 * b.M02 + M01 * b.M12 + M02 * b.M22, M00 * b.M03 + M01 * b.M13 + M02 * b.M23 + M03,
        M10 * b.M00 + M11 * b.M10 + M12 * b.M20, M10 * b.M01 + M11 * b.M11 + M12 * b.M21, M10 * b.M02 + M11 * b.M12 + M12 * b.M22, M10 * b.M03 + M11 * b.M13 + M12 * b.M23 + M13,
        M20 * b.M00 + M21 * b.M10 + M22 * b.M20, M20 * b.M01 + M21 * b.M11 + M22 * b.M21, M20 * b.M02 + M21 * b.M12 + M22 * b.M22, M20 * b.M03 + M21 * b.M13 + M22 * b.M23 + M23);

    public (double x, double y, double z) Apply(double x, double y, double z)
        => (M00 * x + M01 * y + M02 * z + M03, M10 * x + M11 * y + M12 * z + M13, M20 * x + M21 * y + M22 * z + M23);

    public double Det3 => M00 * (M11 * M22 - M12 * M21) - M01 * (M10 * M22 - M12 * M20) + M02 * (M10 * M21 - M11 * M20);

    public double ScaleX => Math.Sqrt(M00 * M00 + M10 * M10 + M20 * M20);
    public double ScaleY => Math.Sqrt(M01 * M01 + M11 * M11 + M21 * M21);
    public double ScaleZ => Math.Sqrt(M02 * M02 + M12 * M12 + M22 * M22);

    public override string ToString() => $"[{M03:F1},{M13:F1},{M23:F1}] sx{ScaleX:F3} sy{ScaleY:F3} sz{ScaleZ:F3}";
}
