using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace LazySF2.Manager;

/// <summary>
/// Puts the mod into the Strike Fighters 2 folder and takes it out again, with the same rules as
/// scripts/install.ps1: every file the mod may create is listed in lazysf2.manifest.txt, and a
/// winmm.dll we did not install is never overwritten.
/// </summary>
public static class ModInstaller
{
    public const string ManifestName = "lazysf2.manifest.txt";
    public const string DllName = "winmm.dll";
    // lazysf2.ini held the mouse setting before it moved into the game's Options; it stays listed so
    // uninstall also removes one left by an older version.
    static readonly string[] ManagedFiles = [DllName, "lazysf2.ini", "lazysf2.log", ManifestName];

    public enum State { GameNotFound, NotInstalled, Installed, Outdated, Foreign }
    public enum Result { Ok, AccessDenied, Foreign, GameNotFound, Failed }

    /// <summary>The winmm.dll shipped with this version of the manager.</summary>
    public static string PayloadDll => Path.Combine(AppContext.BaseDirectory, "payload", DllName);

    /// <summary>The game folder from the installer's registry entry, or null.</summary>
    public static string? FindGameDir()
    {
        using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
        using var key = hklm.OpenSubKey(@"SOFTWARE\ThirdWire\Strike Fighters 2");
        if (key?.GetValue("Path") is not string exe) return null;
        string? dir = Path.GetDirectoryName(exe);
        return dir != null && File.Exists(Path.Combine(dir, "StrikeFighters2.exe")) ? dir : null;
    }

    public static State GetState(string? gameDir)
    {
        if (gameDir == null) return State.GameNotFound;
        string dll = Path.Combine(gameDir, DllName);
        bool ours = File.Exists(Path.Combine(gameDir, ManifestName));
        if (!File.Exists(dll)) return State.NotInstalled;
        if (!ours) return State.Foreign;
        return SameContent(dll, PayloadDll) ? State.Installed : State.Outdated;
    }

    public static Result Install(string? gameDir) => Install(gameDir, PayloadDll);

    // The payload path is a parameter so tests can run Install without the shipped payload folder.
    internal static Result Install(string? gameDir, string payloadDll)
    {
        if (gameDir == null) return Result.GameNotFound;
        if (HasReparsePoint(gameDir)) return Result.Failed;
        try
        {
            string dll = Path.Combine(gameDir, DllName);
            if (File.Exists(dll) && !File.Exists(Path.Combine(gameDir, ManifestName))) return Result.Foreign;
            File.Copy(payloadDll, dll, overwrite: true);
            File.WriteAllLines(Path.Combine(gameDir, ManifestName), ManagedFiles, new UTF8Encoding(false));
            return Result.Ok;
        }
        catch (UnauthorizedAccessException) { return Result.AccessDenied; }
        catch (IOException) { return Result.Failed; }  // e.g. the game is running and has winmm.dll loaded
    }

    /// <summary>
    /// Files to remove: manifest lines that, trimmed, equal one of the managed names (allow-list,
    /// case-insensitive). Anything else, such as "C:x" or "sub\winmm.dll", is ignored, because this
    /// runs elevated and the folder is often writable by non-admins.
    /// </summary>
    public static IReadOnlyList<string> InstalledFiles(string gameDir)
    {
        string manifest = Path.Combine(gameDir, ManifestName);
        if (!File.Exists(manifest)) return [];
        string root = TrimSeparators(Path.GetFullPath(gameDir));
        var result = new List<string>();
        foreach (string line in File.ReadAllLines(manifest))
        {
            string name = line.Trim();
            string? managed = ManagedFiles.FirstOrDefault(m => m.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (managed == null) continue;
            string path = Path.GetFullPath(Path.Combine(gameDir, managed));
            string? parent = Path.GetDirectoryName(path);
            if (parent == null || !TrimSeparators(parent).Equals(root, StringComparison.OrdinalIgnoreCase)) continue;
            if (File.Exists(path) && !result.Contains(path, StringComparer.OrdinalIgnoreCase)) result.Add(path);
        }
        return result;
    }

    static string TrimSeparators(string p) => p.Length > 3 ? p.TrimEnd('\\', '/') : p;

    /// <summary>
    /// True if the game folder or any parent directory is a reparse point (junction, symlink), or an
    /// existing managed file in it is one. A drive root is never counted.
    /// </summary>
    public static bool HasReparsePoint(string gameDir)
    {
        try
        {
            string full = TrimSeparators(Path.GetFullPath(gameDir));
            string root = TrimSeparators(Path.GetPathRoot(full) ?? "");
            for (string? d = full; d != null; d = Path.GetDirectoryName(d))
            {
                if (TrimSeparators(d).Equals(root, StringComparison.OrdinalIgnoreCase)) break;
                if (Directory.Exists(d) && new DirectoryInfo(d).Attributes.HasFlag(FileAttributes.ReparsePoint)) return true;
            }
            foreach (string name in ManagedFiles)
            {
                var fi = new FileInfo(Path.Combine(full, name));
                if (fi.Exists && fi.Attributes.HasFlag(FileAttributes.ReparsePoint)) return true;
            }
            return false;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            return true;  // can't tell: refuse
        }
    }

    public static Result Uninstall(string? gameDir)
    {
        if (gameDir == null) return Result.GameNotFound;
        if (HasReparsePoint(gameDir)) return Result.Failed;
        try
        {
            // The manifest goes last, so a partial removal can be retried.
            foreach (string f in InstalledFiles(gameDir).OrderBy(f => f.EndsWith(ManifestName)))
                File.Delete(f);
            return Result.Ok;
        }
        catch (UnauthorizedAccessException) { return Result.AccessDenied; }
        catch (IOException) { return Result.Failed; }
    }

    static bool SameContent(string a, string b)
    {
        if (!File.Exists(a) || !File.Exists(b)) return false;
        using var sha = SHA256.Create();
        using var fa = File.OpenRead(a);
        using var fb = File.OpenRead(b);
        return sha.ComputeHash(fa).AsSpan().SequenceEqual(SHA256.HashData(fb));
    }
}
