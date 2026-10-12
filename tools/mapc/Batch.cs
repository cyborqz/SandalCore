// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace Mapc;

/// <summary>The machine: free memory, and the processes a heavy job must not run beside (the PC hard-resets under heavy load).</summary>
public static class Sys
{
    [StructLayout(LayoutKind.Sequential)]
    struct MemoryStatusEx
    {
        public uint Length, MemoryLoad;
        public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile, TotalVirtual, AvailVirtual, AvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx s);

    public static double FreePhysicalGb()
    {
        var s = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        return GlobalMemoryStatusEx(ref s) ? s.AvailPhys / 1073741824.0 : double.MaxValue;
    }

    public static double TotalPhysicalGb()
    {
        var s = new MemoryStatusEx { Length = (uint)Marshal.SizeOf<MemoryStatusEx>() };
        return GlobalMemoryStatusEx(ref s) ? s.TotalPhys / 1073741824.0 : 0;
    }

    static readonly string[] Heavy = { "MSBuild", "cl", "link", "cmake", "aoc_navbuild", "AOCClient", "AOCClient-Win64-Shipping", "AshesOfCreation" };

    /// <summary>The names of the running processes a map read must not share the machine with: the game client, a build.  (Other mapc children are the caller's own.)</summary>
    public static List<string> HeavyNeighbours()
    {
        var found = new List<string>();
        foreach (var p in Process.GetProcesses())
        {
            try
            {
                string n = p.ProcessName;
                if (n.StartsWith("AOCClient", StringComparison.OrdinalIgnoreCase) || Heavy.Contains(n, StringComparer.OrdinalIgnoreCase)) found.Add(n + "#" + p.Id);
            }
            catch { }
            finally { p.Dispose(); }
        }
        return found;
    }

    /// <summary>Waits until at least <paramref name="minFreeGb"/> of memory is free and no game or build runs; false after <paramref name="maxMinutes"/>.</summary>
    public static bool WaitForRoom(double minFreeGb, double maxMinutes, Action<string> log)
    {
        var sw = Stopwatch.StartNew();
        string last = "";
        while (true)
        {
            double free = FreePhysicalGb();
            var heavy = HeavyNeighbours();
            if (free >= minFreeGb && heavy.Count == 0) return true;
            string why = free < minFreeGb ? $"only {free:F1} GB free (the guard wants {minFreeGb:F1})" : "running: " + string.Join(", ", heavy);
            if (why != last) { log("waiting: " + why); last = why; }
            if (sw.Elapsed.TotalMinutes >= maxMinutes) { log($"gave up after {maxMinutes:F0} minutes: {why}"); return false; }
            Thread.Sleep(15000);
        }
    }
}
// [C_Y_B ~ C O A]

public sealed class BatchStat
{
    public int Index, Cells;
    public double Seconds, PeakWorkingSetGb, PeakPrivateGb;
    public long PartBytes;
    public bool Reused;
    public override string ToString() => Reused
        ? $"batch {Index}: reused from an earlier run ({Cells} cells, {PartBytes / 1048576.0:F0} MB)"
        : $"batch {Index}: {Cells} cells, {Seconds:F0} s, peak working set {PeakWorkingSetGb:F2} GB, peak commit {PeakPrivateGb:F2} GB, part {PartBytes / 1048576.0:F0} MB";
}

/// <summary>
/// `mapc extract --batches M`: the map read as M child processes, one at a time, each under DOTNET_GCHeapHardLimit and behind the free-memory guard.  A child reads its slice of the cells
/// (<see cref="Cells.Split"/>: a function of the cell list alone) and writes a part (the geometry file's own format) and its foliage rows; the parent merges the parts
/// (<see cref="GeomFile.Merge"/>).  A part that a finished child left in the work folder is reused when its signature matches (a crash or a hard reset loses one batch, not all).
/// </summary>
/// <summary>`--stop-after N`: the measurement run of the first batches; nothing is merged.</summary>
public sealed class StoppedAfterBatch : Exception
{
    public readonly List<BatchStat> Stats;
    public StoppedAfterBatch(List<BatchStat> stats) : base("stopped after batch " + (stats.Count - 1) + " (--stop-after)") { Stats = stats; }
}

public static class BatchRunner
{
    public static string Signature(string scopeText, int m, string dll, IReadOnlyList<string>? baseArgs = null)
    {
        long stamp = File.Exists(dll) ? File.GetLastWriteTimeUtc(dll).Ticks : 0;
        // (review B I5) a game patch between a crash and the re-run must not mix old parts with new: the newest write time of the paks directory and of the usmap are part of it
        long paks = 0, usmap = 0;
        if (baseArgs != null)
        {
            for (int i = 0; i + 1 < baseArgs.Count; i++)
            {
                if (baseArgs[i] == "--paks" && Directory.Exists(baseArgs[i + 1]))
                    foreach (var f in new DirectoryInfo(baseArgs[i + 1]).EnumerateFiles()) paks = Math.Max(paks, f.LastWriteTimeUtc.Ticks);
                if (baseArgs[i] == "--usmap" && File.Exists(baseArgs[i + 1])) usmap = File.GetLastWriteTimeUtc(baseArgs[i + 1]).Ticks;
            }
        }
        return $"m={m};scope={scopeText};dll={stamp};paks={paks};usmap={usmap}";
    }

    /// <summary>(review B I5) A part's file as the .done marker knows it: name, length and FNV-1a 64 of its bytes; the file is flushed to the disk while it is open here, so a marker never outlives
    /// data that a power cut would take (NTFS keeps a file's length with zeroed data after one).</summary>
    public static string Seal(string file)
    {
        using var fs = new FileStream(file, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        ulong h = 14695981039346656037UL;
        long n = 0;
        var buf = new byte[1 << 20];
        int r;
        while ((r = fs.Read(buf, 0, buf.Length)) > 0)
        {
            for (int i = 0; i < r; i++) { h ^= buf[i]; h *= 1099511628211UL; }
            n += r;
        }
        fs.Flush(true);
        return Path.GetFileName(file) + ":" + n.ToString(CultureInfo.InvariantCulture) + ":" + h.ToString("X16", CultureInfo.InvariantCulture);
    }

    /// <summary>Writes a small file so that it is whole on the disk or not there: a temporary beside it, flushed, then renamed over it.</summary>
    public static void WriteDurable(string path, string text)
    {
        string tmp = path + ".tmp";
        using (var fs = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            var b = new UTF8Encoding(false).GetBytes(text);
            fs.Write(b, 0, b.Length);
            fs.Flush(true);
        }
        File.Move(tmp, path, true);
    }

    /// <summary>The part is whole: its marker has the signature and the three files still have the length and hash the marker recorded.</summary>
    static bool PartIsWhole(string part, string done, string sig)
    {
        if (!File.Exists(done) || !File.Exists(part) || !File.Exists(part + ".src") || !File.Exists(part + ".foliage.tsv")) return false;
        var lines = File.ReadAllLines(done);
        if (lines.Length < 5 || lines[0].Trim() != sig) return false;
        try { return Seal(part) == lines[2].Trim() && Seal(part + ".src") == lines[3].Trim() && Seal(part + ".foliage.tsv") == lines[4].Trim(); }
        catch (IOException) { return false; }
    }

    public static List<BatchStat> Run(string dll, IReadOnlyList<string> baseArgs, string scopeText, int m, string workDir, double heapGb, double minFreeGb, double waitMinutes, int stopAfter, Action<string> log, out List<string> partFiles)
    {
        Directory.CreateDirectory(workDir);
        var stats = new List<BatchStat>();
        partFiles = new List<string>();
        string sig = Signature(scopeText, m, dll, baseArgs);
        for (int i = 0; i < m; i++)
        {
            string part = Path.Combine(workDir, $"part_{i:D2}_of_{m}.bin"), done = part + ".done";
            partFiles.Add(part);
            if (PartIsWhole(part, done, sig))
            {
                var lines = File.ReadAllLines(done);
                var st = new BatchStat { Index = i, Reused = true, PartBytes = new FileInfo(part).Length };
                if (lines.Length > 1) int.TryParse(lines[1], out st.Cells);
                stats.Add(st);
                log(st.ToString());
                if (i == stopAfter) throw new StoppedAfterBatch(stats);
                continue;
            }
            if (!Sys.WaitForRoom(minFreeGb, waitMinutes, log)) throw new InvalidOperationException($"batch {i}/{m} not started: no room on this machine");
            try { File.Delete(done); } catch { }
            var args = new List<string> { dll, "extract", "--cells", $"{i}/{m}", "--part", part };
            args.AddRange(baseArgs);
            var psi = new ProcessStartInfo(Environment.ProcessPath ?? "dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var a in args) psi.ArgumentList.Add(a);
            psi.Environment["DOTNET_GCHeapHardLimit"] = ((ulong)(heapGb * 1073741824.0)).ToString("X", CultureInfo.InvariantCulture);
            psi.Environment["DOTNET_gcConcurrent"] = "0";
            psi.Environment["DOTNET_GCgen0size"] = "0x4000000";
            log($"batch {i}/{m}: starting (heap limit {heapGb:F1} GB, {Sys.FreePhysicalGb():F1} GB free)");
            var sw = Stopwatch.StartNew();
            using var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + psi.FileName);
            double peakWs = 0, peakPv = 0;
            var tail = new Queue<string>();
            int cellsRead = 0;
            void Out(string? line)
            {
                if (line == null) return;
                lock (tail) { tail.Enqueue(line); if (tail.Count > 30) tail.Dequeue(); }
                if (line.StartsWith("batch ", StringComparison.Ordinal) && line.Contains(" cells to read")) int.TryParse(line.Split(' ')[2], out cellsRead);
                log("  [" + i + "] " + line);
            }
            p.OutputDataReceived += (_, e) => Out(e.Data);
            p.ErrorDataReceived += (_, e) => Out(e.Data == null ? null : "stderr: " + e.Data);
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            while (!p.WaitForExit(500))
            {
                try { p.Refresh(); peakWs = Math.Max(peakWs, p.PeakWorkingSet64 / 1073741824.0); peakPv = Math.Max(peakPv, p.PeakPagedMemorySize64 / 1073741824.0); } catch { }
                // the machine must stay usable: a batch that eats the free memory is stopped (the guard's floor is half of its start value)
                if (Sys.FreePhysicalGb() < 1.5) { try { p.Kill(true); } catch { } throw new InvalidOperationException($"batch {i}/{m} stopped: less than 1.5 GB of memory was left free"); }
            }
            p.WaitForExit();
            if (p.ExitCode != 0)
            {
                string t; lock (tail) t = string.Join("\n", tail);
                throw new InvalidOperationException($"batch {i}/{m} failed with exit code {p.ExitCode}:\n{t}");
            }
            var s = new BatchStat { Index = i, Seconds = sw.Elapsed.TotalSeconds, PeakWorkingSetGb = peakWs, PeakPrivateGb = peakPv, PartBytes = new FileInfo(part).Length };
            s.Cells = cellsRead;
            // the marker goes last, whole, and names what it vouches for (review B I5)
            WriteDurable(done, sig + "\n" + s.Cells + "\n" + Seal(part) + "\n" + Seal(part + ".src") + "\n" + Seal(part + ".foliage.tsv") + "\n");
            stats.Add(s);
            log(s.ToString());
            if (i == stopAfter) throw new StoppedAfterBatch(stats);
        }
        return stats;
    }
// C-Y-B captures, C.O.A code

    // ---- the foliage rows (stage 3's input) ---------------------------------------------------------------------------------------------------

    public const string FoliageFileName = "region_foliage.tsv";

    public static string FoliageTsv(IEnumerable<FoliageRow> rows, string scopeText)   // scopeText: what the instances stand in
    {
        var ic = CultureInfo.InvariantCulture;
        var sb = new StringBuilder();
        sb.Append("# region_foliage v2: the IntrepidGatherableFISMC instances of the Foliage cells inside ").Append(scopeText).Append(" (tools/mapc: mapc extract). World cm and degrees; index = the instance's place in its component's PerInstanceSMData; the instance's key is (cell, owner, component, index): a component's name repeats across the actors of a cell, its owner actor's does not.\n");
        sb.Append("# def_id\tdef_name\tcell\towner\tcomponent\tindex\tx\ty\tz\tyaw\tpitch\troll\tsx\tsy\tsz\tmesh\ttags\n");
        foreach (var f in rows)
            sb.Append(f.DefId.ToString(ic)).Append('\t').Append(f.DefName).Append('\t').Append(f.Cell).Append('\t').Append(f.Owner).Append('\t').Append(f.Component).Append('\t').Append(f.Index.ToString(ic)).Append('\t')
              .Append(f.X.ToString("F1", ic)).Append('\t').Append(f.Y.ToString("F1", ic)).Append('\t').Append(f.Z.ToString("F1", ic)).Append('\t')
              .Append(f.Yaw.ToString("F3", ic)).Append('\t').Append(f.Pitch.ToString("F3", ic)).Append('\t').Append(f.Roll.ToString("F3", ic)).Append('\t')
              .Append(f.Sx.ToString("F4", ic)).Append('\t').Append(f.Sy.ToString("F4", ic)).Append('\t').Append(f.Sz.ToString("F4", ic)).Append('\t').Append(f.Mesh).Append('\t').Append(f.Tags).Append('\n');
        return sb.ToString();
    }

    /// <summary>A part's foliage sidecar: the rows in the final file's own format, so the parent reads them back.</summary>
    public static List<FoliageRow> ReadFoliageTsv(string path)
    {
        var ic = CultureInfo.InvariantCulture;
        var rows = new List<FoliageRow>();
        foreach (var line in File.ReadLines(path))
        {
            if (line.Length == 0 || line[0] == '#') continue;
            var c = line.Split('\t');
            rows.Add(new FoliageRow(c[2], c[3], c[4], int.Parse(c[5], ic), long.Parse(c[0], ic), c[1], c[15], double.Parse(c[6], ic), double.Parse(c[7], ic), double.Parse(c[8], ic),
                double.Parse(c[9], ic), double.Parse(c[10], ic), double.Parse(c[11], ic), double.Parse(c[12], ic), double.Parse(c[13], ic), double.Parse(c[14], ic), c.Length > 16 ? c[16] : ""));
        }
        return rows;
    }
}
