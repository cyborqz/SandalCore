// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using Serilog;

namespace Mapc;

/// <summary>The game's containers opened read-only through CUE4Parse, with the usmap's mappings.</summary>
public sealed class Game
{
    public static readonly string DefaultPaks = Path.Combine(Environment.GetEnvironmentVariable("GAME_ROOT") ?? @"C:\Ashes of Creation\Game", "AOC", "Content", "Paks");   // $GAME_ROOT\AOC\Content\Paks
    public static readonly string DefaultUsmap = Path.Combine("dist", "content", "aoc.usmap");   // relative to the repository root, where mapc is run from
    public const string MasterPath = "AOC/Content/Levels/Verra_World_Master/Verra_World_Master";

    public readonly DefaultFileProvider Provider;
    readonly Dictionary<string, IPackage?> _cache = new();

    public Game(string paks, string usmap)
    {
        Log.Logger = new LoggerConfiguration().MinimumLevel.Fatal().WriteTo.Console().CreateLogger();
        // CUE4Parse 1.2.2 reads usmap versions up to 3; the game's is 4 (see UsmapCompat)
        var tmp = Path.Combine(Path.GetTempPath(), "mapc_aoc_v3.usmap");
        File.WriteAllBytes(tmp, UsmapCompat.DowngradeToV3(File.ReadAllBytes(usmap)));
        Provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_6), StringComparer.OrdinalIgnoreCase);
        Provider.MappingsContainer = new FileUsmapTypeMappingsProvider(tmp);
        Provider.Initialize();
        Provider.SubmitKey(new FGuid(), new FAesKey(new byte[32]));   // the containers are not encrypted: the all-zero key mounts them
        File.Delete(tmp);
    }

    public IPackage? Load(string path)
    {
        if (_cache.TryGetValue(path, out var p)) return p;
        try { p = Provider.LoadPackage(path); } catch (Exception) { p = null; }
        _cache[path] = p;
        return p;
    }

    public void Forget(string path) => _cache.Remove(path);
}
