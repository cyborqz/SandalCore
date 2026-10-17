// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text.RegularExpressions;

namespace Mapc;

/// <summary>
/// Two small exports of files the game ships as they are, for the generators that are not C# (scripts/extract_game_data.ps1 runs them):
///   mapc export-config   the client's Config directory (AOC/Config: DefaultCombat.ini, DefaultAoCRecordConstants.ini, ...) into DIR/client_config/
///   mapc export-zoi      the zone-of-influence chunk packages of layer 0 (C_cx_cy_0.uasset) into DIR/zoi_chunks/
/// A file is written byte for byte as the container holds it. A file that already has those bytes is left alone, so its time stays and the next run changes nothing.
/// </summary>
public static class Export
{
    public const string ConfigPrefix = "AOC/Config/";
    public const string ConfigDir = "client_config";
    public const string ZoiPrefix = "AOC/Content/DesignAssets/World/ZOI/Verra_World_Master_ZOIs/_Generated_/Chunks/";
    public const string ZoiDir = "zoi_chunks";

    // layer 0 of the chunk grid: the surface table and the Riverlands' footprint are read from it alone
    static readonly Regex Layer0 = new(@"^C_-?\d+_-?\d+_0\.uasset$", RegexOptions.Compiled | RegexOptions.IgnoreCase);

    /// <summary>AOC/Config/** (every .ini the game ships, with its sub folders) into outDir/client_config.</summary>
    public static int Config(Game g, string outDir) =>
        Copy(g, ConfigPrefix, rel => rel.EndsWith(".ini", StringComparison.OrdinalIgnoreCase), Path.Combine(outDir, ConfigDir), "config files");

    /// <summary>The layer 0 chunk packages of the ZOI grid into outDir/zoi_chunks (the hash folders of the container are kept).</summary>
    public static int Zoi(Game g, string outDir) =>
        Copy(g, ZoiPrefix, rel => Layer0.IsMatch(Path.GetFileName(rel)), Path.Combine(outDir, ZoiDir), "ZOI chunk packages");

    static int Copy(Game g, string prefix, Func<string, bool> keep, string dest, string what)
    {
        var keys = g.Provider.Files.Keys
            .Where(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && keep(k.Substring(prefix.Length)))
            .OrderBy(k => k, StringComparer.Ordinal)
            .ToList();
        if (keys.Count == 0)
        {
            Console.Error.WriteLine($"mapc: no {what} under {prefix} in the game's containers (is this the game's Paks folder? --paks)");
            return 1;
        }
        string root = Path.GetFullPath(dest) + Path.DirectorySeparatorChar;
        int written = 0, same = 0, failed = 0;
        long bytes = 0;
        foreach (var key in keys)
        {
            string rel = key.Substring(prefix.Length);
            string target = Path.GetFullPath(Path.Combine(dest, rel.Replace('/', Path.DirectorySeparatorChar)));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) { Console.Error.WriteLine("mapc: refusing the path " + key); failed++; continue; }
            if (!g.Provider.TrySaveAsset(key, out var data)) { Console.Error.WriteLine("mapc: cannot read " + key); failed++; continue; }
            bytes += data.Length;
            if (File.Exists(target) && new FileInfo(target).Length == data.Length && File.ReadAllBytes(target).AsSpan().SequenceEqual(data)) { same++; continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.WriteAllBytes(target, data);
            written++;
        }
        Console.WriteLine($"{what}: {keys.Count} found, {written} written, {same} already there, {failed} failed, {bytes} bytes, into {dest}");
        return failed == 0 ? 0 : 1;
    }
}
