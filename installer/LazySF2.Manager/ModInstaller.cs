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
    public const string IniName = "lazysf2.ini";
    static readonly string[] ManagedFiles = [DllName, IniName, "lazysf2.log", ManifestName];

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

    public static Result Install(string? gameDir)
    {
        if (gameDir == null) return Result.GameNotFound;
        try
        {
            string dll = Path.Combine(gameDir, DllName);
            if (File.Exists(dll) && !File.Exists(Path.Combine(gameDir, ManifestName))) return Result.Foreign;
            File.Copy(PayloadDll, dll, overwrite: true);
            File.WriteAllLines(Path.Combine(gameDir, ManifestName), ManagedFiles, new UTF8Encoding(false));
            // New installs match the Windows cursor (the stock mouse feels off); a Repair keeps the choice.
            if (!File.Exists(Path.Combine(gameDir, IniName))) return SetMouseMode(gameDir, "Cursor");
            return Result.Ok;
        }
        catch (UnauthorizedAccessException) { return Result.AccessDenied; }
        catch (IOException) { return Result.Failed; }  // e.g. the game is running and has winmm.dll loaded
    }

    /// <summary>Files to remove: those listed in the manifest (plain names in the game folder only).</summary>
    public static IReadOnlyList<string> InstalledFiles(string gameDir)
    {
        string manifest = Path.Combine(gameDir, ManifestName);
        if (!File.Exists(manifest)) return [];
        return File.ReadAllLines(manifest)
            .Select(l => l.Trim())
            .Where(n => n.Length > 0 && n.IndexOfAny(['\\', '/']) < 0 && !n.Contains(".."))
            .Select(n => Path.Combine(gameDir, n))
            .Where(File.Exists)
            .ToList();
    }

    public static Result Uninstall(string? gameDir)
    {
        if (gameDir == null) return Result.GameNotFound;
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

    /// <summary>lazysf2.ini [Mouse] Mode: "Stock" (default) or "Cursor".</summary>
    public static string GetMouseMode(string gameDir)
    {
        string ini = Path.Combine(gameDir, IniName);
        if (!File.Exists(ini)) return "Stock";
        string? line = File.ReadAllLines(ini).FirstOrDefault(l => l.TrimStart().StartsWith("Mode=", StringComparison.OrdinalIgnoreCase));
        return line != null && line.Trim().EndsWith("Cursor", StringComparison.OrdinalIgnoreCase) ? "Cursor" : "Stock";
    }

    public static Result SetMouseMode(string gameDir, string mode)
    {
        try
        {
            File.WriteAllLines(Path.Combine(gameDir, IniName), ["[Mouse]", $"Mode={mode}"], Encoding.ASCII);
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
