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

// ---- the towns' layouts: a tiny synthetic layout, one wall and one raised floor ------------------------------------------------------------------
MeshGeo Box(string name, double x0, double y0, double z0, double x1, double y1, double z1, string profile = "BlockAll")
{
    var g = new MeshGeo { Name = name, Kind = "simple", DefaultProfile = profile, Shapes = 1 };
    var v = new int[8];
    for (int i = 0; i < 8; i++) v[i] = g.AddV((i & 1) == 0 ? x0 : x1, (i & 2) == 0 ? y0 : y1, (i & 4) == 0 ? z0 : z1);
    g.AddQuad(v[0], v[2], v[3], v[1]); g.AddQuad(v[4], v[5], v[7], v[6]); g.AddQuad(v[0], v[1], v[5], v[4]);
    g.AddQuad(v[2], v[6], v[7], v[3]); g.AddQuad(v[0], v[4], v[6], v[2]); g.AddQuad(v[1], v[3], v[7], v[5]);
    return g;
}
(double x0, double y0, double z0, double x1, double y1, double z1) Bounds(AreaGeometry g, int placement)
{
    var p = g.Placements[placement]; var m = g.Meshes[p.Mesh];
    double a = 1e30, b = 1e30, c = 1e30, d = -1e30, e = -1e30, f = -1e30;
    for (int i = 0; i < m.V.Count; i += 3)
    {
        var (x, y, z) = p.M.Apply(m.V[i], m.V[i + 1], m.V[i + 2]);
        a = Math.Min(a, x); b = Math.Min(b, y); c = Math.Min(c, z); d = Math.Max(d, x); e = Math.Max(e, y); f = Math.Max(f, z);
    }
    return (a, b, c, d, e, f);
}
bool Near((double x0, double y0, double z0, double x1, double y1, double z1) a, (double, double, double, double, double, double) b)
    => Math.Abs(a.x0 - b.Item1) < 1e-2 && Math.Abs(a.y0 - b.Item2) < 1e-2 && Math.Abs(a.z0 - b.Item3) < 1e-2 && Math.Abs(a.x1 - b.Item4) < 1e-2 && Math.Abs(a.y1 - b.Item5) < 1e-2 && Math.Abs(a.z1 - b.Item6) < 1e-2;

const string wallPath = "/Game/Test/SM_Wall.SM_Wall", floorPath = "/Game/Test/SM_Floor.SM_Floor", glassPath = "/Game/Test/SM_Glass.SM_Glass", housePath = "/Game/Test/Level_Instances/LI_House.LI_House";
string[] itemLines =
{
    "# a comment",
    "@node\t1\tTestA\tLayout_Test\tDefault\t1000\t2000\t300\t90",
    "@node\t2\tTestB\tLayout_Test\tDefault\t-5000\t0\t0\t0",
    "@asset\t0\tM\t" + wallPath,
    "@asset\t1\tM\t" + floorPath,
    "@asset\t2\tM\t" + glassPath,
    "@asset\t3\tL\t" + housePath,
    // the wall: 4 m long (x), 30 cm thick (y), 3 m high, turned 90 degrees at the node's place, Town only
    "1\t0\t10\t1\t0\t-1\t0\t1\t0\t0\t0\t0\t1\t1000\t2000\t300",
    // the raised floor: a 5 m slab whose top face is 3 m over the street, Village and Town
    "1\t1\t18\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t1100\t2100\t600",
    // glass: no collision
    "1\t2\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\t0",
    // a house (a level instance): turned half a turn at (5000, 0, 100)
    "1\t3\t10\t0\t-1\t0\t0\t0\t-1\t0\t0\t0\t1\t5000\t0\t100",
    // the same wall in another bowl, and a wall that stands at Wilderness only
    "2\t0\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t-5000\t0\t0",
    "1\t0\t1\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\t0",
};
var layoutItems = TownLayouts.Parse(itemLines, "the synthetic list");
Check(layoutItems.Bowls.Count == 2 && layoutItems.Assets.Count == 4 && layoutItems.Rows.Count == 6 && layoutItems.Rows[0].Levels == 0x10 && !layoutItems.Rows[3].StepUp && layoutItems.Rows[0].M.M03 == 1000 && layoutItems.Rows[0].M.M01 == -1 && layoutItems.Rows[0].M.M10 == 1, "town layout list: nodes, assets, rows and the matrix columns");
foreach (var (bad, label) in new[]
{
    ("1\t0\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t1000\t2000", "a row with 15 columns"),
    ("9\t0\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\t0", "a row of an unlisted node"),
    ("1\t7\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\t0", "a row of an unlisted asset"),
    ("1\t0\t0\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\t0", "a row that stands at no level"),
    ("1\t0\t10\t1\t1\t0\t0\t0\t1\t0\t0\t0\t1\t0\t0\tNaN", "a row with a number that is not finite"),
    ("@bowl\t1", "an unknown line"),
})
{
    try { TownLayouts.Parse(itemLines.Concat(new[] { bad }), "the synthetic list"); throw new Exception("FAIL: accepted " + label); }
    catch (InvalidDataException e) when (e.Message.Contains("line " + (itemLines.Length + 1), StringComparison.Ordinal)) { passed++; Console.WriteLine("PASS: town layout list rejects " + label); }
}

var wall = Box("StaticMesh'SM_Wall'@00000001", 0, 0, 0, 400, 30, 300);
var slab = Box("StaticMesh'SM_Floor'@00000002", 0, 0, -20, 500, 500, 0);
var glass = Box("StaticMesh'SM_Glass'@00000003", 0, 0, 0, 100, 10, 100, "NoCollision");
var houseWall = Box("StaticMesh'SM_Wall'@00000001", 0, 0, 0, 400, 30, 300);   // (the same mesh as the first, by name)
var houseFloor = Box("StaticMesh'SM_HouseFloor'@00000004", 0, 0, -20, 200, 200, 0);
Check(TownLayouts.Blocks(wall) && !TownLayouts.Blocks(glass) && !TownLayouts.Blocks(new MeshGeo { Name = "x", Kind = "none" }) && !TownLayouts.Blocks(Box("pawn", 0, 0, 0, 1, 1, 1, "OverlapAll")), "town layout meshes: a profile that does not block, and a mesh with no shape, are left out");
var fake = new FakeSource();
fake.Meshes[wallPath] = wall; fake.Meshes[floorPath] = slab; fake.Meshes[glassPath] = TownLayouts.Blocks(glass) ? glass : null;
fake.Levels[housePath] = new[] { new TownLayouts.LiPart(houseWall, Xf.FromTRS(10, 20, 0, (0, 0, 0, 1), 1, 1, 1)), new TownLayouts.LiPart(houseFloor, Xf.FromTRS(0, 0, 250, (0, 0, 0, 1), 1, 1, 1)) };
var layer = TownLayouts.Compose(layoutItems, fake);
// the wall turned a quarter turn at (1000, 2000, 300): x runs along +y, y along -x
bool wallAt = false, floorAt = false, houseWallAt = false, houseFloorAt = false;
for (int i = 0; i < layer.Placements.Count; i++)
{
    var name = layer.Meshes[layer.Placements[i].Mesh].Name; var b = Bounds(layer, i);
    if (name.Contains("SM_Wall") && Near(b, (970, 2000, 300, 1000, 2400, 600))) wallAt = true;
    if (name.Contains("SM_Floor") && Near(b, (1100, 2100, 580, 1600, 2600, 600))) floorAt = true;
    // the house: half a turn at (5000, 0, 100), its wall at (10, 20, 0) of the house: x from 5000 - 10 - 400 to 5000 - 10
    if (name.Contains("SM_Wall") && Near(b, (4590, -50, 100, 4990, -20, 400))) houseWallAt = true;
    if (name.Contains("SM_HouseFloor") && Near(b, (4800, -200, 330, 5000, 0, 350))) houseFloorAt = true;
}
Check(wallAt, "town layouts: a wall is placed at its row's matrix (turned, raised to the node's height)");
Check(floorAt, "town layouts: a raised floor's top face is at the row's height");
Check(houseWallAt && houseFloorAt, "town layouts: a level instance's meshes are put at the row's matrix after their own");
Check(layer.Placements.Count == 6 && layer.Meshes.Count == 4 && layer.Stats["layouts.rows.noCollision"] == 1 && layer.Stats["layouts.levelInstances"] == 1 && layer.Stats["layouts.rows.noStepUp"] == 1, "town layouts: 6 placements of 4 meshes (the glass has no collision, the house gives two)");
var only4 = TownLayouts.Compose(layoutItems, fake, 0x10);
Check(only4.Placements.Count == 5 && only4.Stats["layouts.rows.otherLevels"] == 1, "town layouts: a level mask leaves out what stands at no wanted level");
var onlyB = TownLayouts.Compose(layoutItems, fake, TownLayouts.AllLevels, new HashSet<long> { 2 });
Check(onlyB.Placements.Count == 1 && Near(Bounds(onlyB, 0), (-5000, 0, 0, -4600, 30, 300)), "town layouts: a node filter keeps one bowl");
var reversed = new TownLayouts.Items { Bowls = layoutItems.Bowls, Assets = layoutItems.Assets, Rows = layoutItems.Rows.AsEnumerable().Reverse().ToList() };
var layerBytes = GeomFile.Write(layer);
Check(layerBytes.SequenceEqual(GeomFile.Write(TownLayouts.Compose(reversed, fake))), "town layouts: the file does not depend on the order of the rows");
var layerBack = GeomFile.Read(layerBytes);
Check(layerBack.Tiles.Count == 0 && layerBack.Region == null && layerBack.Placements.Count == 6 && layerBytes.SequenceEqual(GeomFile.Write(layerBack)), "town layouts: the file is area_geom.bin's format, whole after a round trip");
var roundTrip = Path.Combine(Path.GetTempPath(), "mapc_layer_test_" + Environment.ProcessId + ".bin");
try { File.WriteAllBytes(roundTrip, layerBytes); Check(TownLayouts.ReadLayer(roundTrip).Placements.All(p => p.Src == int.MaxValue), "town layouts: a layer read for a merge ranks after every cell"); }
finally { File.Delete(roundTrip); }
// merged into a map's geometry: the terrain and the map's placement stay, a mesh of the same name is one mesh
var map = new AreaGeometry { CenterX = 1, CenterY = 2, Radius = 3, Margin = 4 };
map.Tiles.Add(new TerrainTile { Nx = 2, Ny = 2, StepX = 10, StepY = 10, ZScale = 1, Raw = new ushort[] { 1, 2, 3, 4 } });
map.Meshes.Add(Box("StaticMesh'SM_Wall'@00000001", 0, 0, 0, 400, 30, 300));
map.Placements.Add(new Placement(0, new Xf(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0), 3));
var merged = GeomFile.Merge(new[] { map, GeomFile.Read(layerBytes) });
Check(merged.Tiles.Count == 1 && merged.Meshes.Count == 3 && merged.Placements.Count == 6 && merged.CenterX == 1, "town layouts: merged into the map's geometry, a mesh of the same name is one mesh, a placement both have is one, and the terrain stays");
Check(GeomFile.Write(merged).SequenceEqual(GeomFile.Write(GeomFile.Read(GeomFile.Write(merged)))), "town layouts: the merged geometry round-trips");
Check(TownLayouts.PackagePath(wallPath, ".uasset") == "AOC/Content/Test/SM_Wall.uasset" && TownLayouts.PackagePath("/Engine/BasicShapes/Cube.Cube", ".uasset") == "Engine/Content/BasicShapes/Cube.uasset" && TownLayouts.ObjectName(wallPath) == "SM_Wall", "town layouts: object paths name their package and object");

Console.WriteLine($"{passed} passed, 0 failed");

sealed class FakeSource : TownLayouts.ISource
{
    public readonly Dictionary<string, MeshGeo?> Meshes = new();
    public readonly Dictionary<string, TownLayouts.LiPart[]> Levels = new();
    public MeshGeo? Mesh(string objectPath) => Meshes.GetValueOrDefault(objectPath);
    public IReadOnlyList<TownLayouts.LiPart> LevelInstance(string objectPath) => Levels.GetValueOrDefault(objectPath) ?? Array.Empty<TownLayouts.LiPart>();
}
