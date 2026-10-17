// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using Mapc;
using System.Buffers.Binary;
using System.Text;

int passed = 0;
void Check(bool condition, string label)
{
    if (!condition) throw new Exception("FAIL: " + label);
    passed++;
    Console.WriteLine("PASS: " + label);
}
void Reject(byte[] input, string label)
{
    try { GeomFile.Read(input); }
    catch (Exception e) when (e is EndOfStreamException or InvalidDataException or OverflowException)
    { passed++; Console.WriteLine("PASS: " + label + " (" + e.GetType().Name + ")"); return; }
    throw new Exception("FAIL: accepted " + label);
}
byte[] U32(byte[] input, int offset, uint value)
{
    var copy = (byte[])input.Clone();
    BinaryPrimitives.WriteUInt32LittleEndian(copy.AsSpan(offset, 4), value);
    return copy;
}

var geometry = new AreaGeometry { CenterX = 1, CenterY = 2, Radius = 3, Margin = 4 };
geometry.Tiles.Add(new TerrainTile { Nx = 2, Ny = 2, StepX = 10, StepY = 10, ZScale = 1, Raw = new ushort[] { 1, 2, 3, 4 } });
var mesh = new MeshGeo { Name = "triangle", V = new List<float> { 0, 0, 0, 10, 0, 0, 0, 10, 0 }, I = new List<int> { 0, 1, 2 } };
geometry.Meshes.Add(mesh);
geometry.Placements.Add(new Placement(0, new Xf(1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30)));
var data = GeomFile.Write(geometry);
var decoded = GeomFile.Read(data);
Check(decoded.Tiles.Count == 1 && decoded.Tiles[0].Raw.SequenceEqual(geometry.Tiles[0].Raw) &&
      decoded.Meshes.Count == 1 && decoded.Meshes[0].I.SequenceEqual(mesh.I) &&
      decoded.Placements.Count == 1 && decoded.Placements[0].M.M03 == 10 && data.SequenceEqual(GeomFile.Write(decoded)), "terrain, mesh and placement roundtrip preserves exact bytes");
Check(GeomFile.Read(GeomFile.Write(new AreaGeometry())).Placements.Count == 0, "empty geometry roundtrip");
// -- C.Y.B . C-O-A --

const int terrainHeader = 68, terrainRaw = terrainHeader + 56;
int meshStart = terrainRaw + 8;
int vertexCount = meshStart + 2 + Encoding.UTF8.GetByteCount(mesh.Name);
int vertices = vertexCount + 8;
int indices = vertices + mesh.V.Count * sizeof(float);
int placement = indices + mesh.I.Count * sizeof(int);
Reject(data[..7], "truncated magic");
Reject(data[..(terrainRaw + 7)], "truncated terrain samples");
Reject(data[..(vertexCount - 1)], "truncated mesh name");
Reject(data[..(indices - 1)], "truncated vertex data");
Reject(data[..(placement - 1)], "truncated index data");
Reject(data[..^1], "truncated placement");
Reject(U32(data, terrainHeader + 48, uint.MaxValue), "oversized terrain count");
Reject(U32(data, terrainHeader + 48, 4097), "terrain dimension beyond 4096");
Reject(U32(data, vertexCount, uint.MaxValue), "oversized vertex count");
Reject(U32(data, vertexCount + 4, uint.MaxValue), "oversized index count");
Reject(U32(data, vertexCount + 4, 2), "index count does not form triangles");
Reject(U32(data, indices, 3), "triangle index outside vertex array");
Reject(U32(data, indices, uint.MaxValue), "negative encoded triangle index");
Reject(U32(data, placement, 1), "placement names unknown mesh");
Reject(data.Concat(new byte[] { 0 }).ToArray(), "trailing bytes");
mesh.Name = new string('a', 65536);
try { GeomFile.Write(geometry); throw new Exception("FAIL: accepted oversized UTF-8 name"); }
catch (OverflowException) { passed++; Console.WriteLine("PASS: oversized UTF-8 name rejected"); }
Console.WriteLine($"{passed} passed, 0 failed");
