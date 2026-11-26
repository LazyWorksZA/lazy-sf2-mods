using Microsoft.Win32;

namespace LazyWorks.SF2Patch.Manager;

/// <summary>The registry and file system reads (and the saved-folder writes) that finding the game needs.</summary>
public interface IGameEnvironment
{
    /// <summary>A string value, or null when the key or the value is missing or not a string.</summary>
    string? GetString(RegistryHive hive, RegistryView view, string subKey, string name);
    IReadOnlyList<string> GetSubKeyNames(RegistryHive hive, RegistryView view, string subKey);
    /// <summary>Writes a string value, creating the key. False when access is denied.</summary>
    bool SetString(RegistryHive hive, RegistryView view, string subKey, string name, string value);
    /// <summary>Deletes the key and everything under it. False when access is denied.</summary>
    bool DeleteKey(RegistryHive hive, RegistryView view, string subKey);
    bool FileExists(string path);
    /// <summary>File names of other Strike Fighters executables (StrikeFighters*.exe) in a folder.</summary>
    IReadOnlyList<string> FindOtherSf2Exes(string dir);
    /// <summary>TimeDateStamp and SizeOfImage from the PE header, or null when the file can't be read as a PE.</summary>
    (uint TimeDateStamp, uint SizeOfImage)? ReadPeStamp(string exePath);
}

/// <summary>
/// Finds the Strike Fighters 2 folder the mod targets (the one with StrikeFighters2.exe, July 2013 build)
/// and remembers a folder the player chose. Every place tried is logged.
/// </summary>
public static class GameLocator
{
    public const string ExeName = "StrikeFighters2.exe";
    public const string SavedKey = @"SOFTWARE\LazyWorks\LazySF2Patch";
    public const string SavedValue = "GameDir";
    // Same values as kExeTimestamp / kExeImageSize in src/hooks (the July 2013 final patch).
    public const uint ExeTimestamp = 0x51d22110, ExeImageSize = 0x62000;

    const string ThirdWire = @"SOFTWARE\ThirdWire";
    const string Uninstall = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
    const string VirtualStore = @"Software\Classes\VirtualStore\MACHINE\SOFTWARE";

    public enum Check { Ok, NoExe, WrongBuild }

    /// <summary>Whether <paramref name="dir"/> holds the StrikeFighters2.exe of the July 2013 build.</summary>
    public static Check CheckFolder(IGameEnvironment env, string dir)
    {
        string exe = Path.Combine(dir, ExeName);
        if (!env.FileExists(exe)) return Check.NoExe;
        // An exe we can't parse is accepted: the mod itself skips its hooks on a build it doesn't know.
        if (env.ReadPeStamp(exe) is { } s && (s.TimeDateStamp != ExeTimestamp || s.SizeOfImage != ExeImageSize)) return Check.WrongBuild;
        return Check.Ok;
    }

    /// <summary>The first valid game folder, in the order of the steps below, or null.</summary>
    public static string? Find(IGameEnvironment env, Action<string> log)
    {
        foreach (var (label, source) in Steps(env))
        {
            var found = source().Where(c => !string.IsNullOrWhiteSpace(c)).Select(c => c!).ToList();
            if (found.Count == 0) { log($"game folder: {label}: nothing"); continue; }
            foreach (string c in found)
            {
                string? dir = TryCandidate(env, label, c, log);
                if (dir != null) return dir;
            }
        }
        log("game folder: not found");
        return null;
    }

    static IEnumerable<(string Label, Func<IEnumerable<string?>> Source)> Steps(IGameEnvironment env)
    {
        const string sf2 = ThirdWire + @"\Strike Fighters 2";
        yield return ("HKLM 32-bit " + sf2, () => [env.GetString(RegistryHive.LocalMachine, RegistryView.Registry32, sf2, "Path")]);
        yield return ("HKLM 64-bit " + sf2, () => [env.GetString(RegistryHive.LocalMachine, RegistryView.Registry64, sf2, "Path")]);
        const string vs32 = VirtualStore + @"\WOW6432Node\ThirdWire\Strike Fighters 2";
        const string vs64 = VirtualStore + @"\ThirdWire\Strike Fighters 2";
        yield return ("HKCU " + vs32, () => [env.GetString(RegistryHive.CurrentUser, RegistryView.Registry64, vs32, "Path")]);
        yield return ("HKCU " + vs64, () => [env.GetString(RegistryHive.CurrentUser, RegistryView.Registry64, vs64, "Path")]);
        foreach (var (hive, view, name) in new[]
        {
            (RegistryHive.LocalMachine, RegistryView.Registry32, "HKLM 32-bit"),
            (RegistryHive.LocalMachine, RegistryView.Registry64, "HKLM 64-bit"),
            (RegistryHive.CurrentUser, RegistryView.Registry64, "HKCU"),
            (RegistryHive.CurrentUser, RegistryView.Registry32, "HKCU 32-bit"),
        })
        {
            yield return ($"{name} Uninstall entries named \"Strike Fighters 2\"", () => UninstallCandidates(env, hive, view));
        }
        foreach (var (view, name) in new[] { (RegistryView.Registry32, "32-bit"), (RegistryView.Registry64, "64-bit") })
        {
            yield return ($"HKLM {name} other {ThirdWire} Strike Fighters 2 products", () =>
                env.GetSubKeyNames(RegistryHive.LocalMachine, view, ThirdWire)
                    .Where(n => n.Contains("Strike Fighters 2", StringComparison.OrdinalIgnoreCase)
                                && !n.Equals("Strike Fighters 2", StringComparison.OrdinalIgnoreCase))
                    .Select(n => env.GetString(RegistryHive.LocalMachine, view, ThirdWire + "\\" + n, "Path")));
        }
        yield return ($"saved folder (HKLM\\{SavedKey}\\{SavedValue}, then HKCU)", () =>
        [
            env.GetString(RegistryHive.LocalMachine, RegistryView.Registry64, SavedKey, SavedValue),
            env.GetString(RegistryHive.CurrentUser, RegistryView.Registry64, SavedKey, SavedValue),
        ]);
    }

    static IEnumerable<string?> UninstallCandidates(IGameEnvironment env, RegistryHive hive, RegistryView view)
    {
        foreach (string sub in env.GetSubKeyNames(hive, view, Uninstall))
        {
            string key = Uninstall + "\\" + sub;
            string? display = env.GetString(hive, view, key, "DisplayName");
            if (display == null || !display.Contains("Strike Fighters 2", StringComparison.OrdinalIgnoreCase)) continue;
            yield return env.GetString(hive, view, key, "InstallLocation");
            yield return env.GetString(hive, view, key, "DisplayIcon");
            yield return env.GetString(hive, view, key, "UninstallString");
        }
    }

    static string? TryCandidate(IGameEnvironment env, string label, string raw, Action<string> log)
    {
        string path = ExtractPath(raw);
        string? exeName = null, dir = path;
        if (path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
        {
            exeName = Path.GetFileName(path);
            dir = Path.GetDirectoryName(path);
        }
        if (string.IsNullOrEmpty(dir)) { log($"game folder: {label}: \"{raw}\" has no folder"); return null; }
        switch (CheckFolder(env, dir))
        {
            case Check.Ok:
                log($"game folder: {label}: found {ExeName} in {dir}");
                return dir;
            case Check.WrongBuild:
                log($"game folder: {label}: {dir} has a {ExeName} that is not the July 2013 build; skipped");
                return null;
        }
        string? other = null;
        if (exeName != null && !exeName.Equals(ExeName, StringComparison.OrdinalIgnoreCase) && env.FileExists(path)) other = exeName;
        other ??= env.FindOtherSf2Exes(dir).FirstOrDefault();
        log(other != null
            ? $"game folder: {label}: found {other} in {dir}, but the mod needs {ExeName} (July 2013 merged install)"
            : $"game folder: {label}: no {ExeName} in {dir}");
        return null;
    }

    /// <summary>
    /// The path in a registry value that may carry quotes, arguments or an icon index, such as
    /// "C:\x\a.exe",0 or C:\x\unins.exe /silent. Returns a folder or an exe path.
    /// </summary>
    public static string ExtractPath(string value)
    {
        string v = value.Trim();
        if (v.StartsWith('"'))
        {
            int end = v.IndexOf('"', 1);
            return (end > 0 ? v[1..end] : v[1..]).Trim();
        }
        int exe = v.IndexOf(".exe", StringComparison.OrdinalIgnoreCase);
        return (exe >= 0 ? v[..(exe + 4)] : v).TrimEnd('\\', '/');
    }

    /// <summary>Remembers the folder the player chose: HKLM when allowed (elevated), else HKCU. Returns where it went.</summary>
    public static string? SaveChoice(IGameEnvironment env, string dir)
    {
        if (env.SetString(RegistryHive.LocalMachine, RegistryView.Registry64, SavedKey, SavedValue, dir)) return "HKLM";
        return env.SetString(RegistryHive.CurrentUser, RegistryView.Registry64, SavedKey, SavedValue, dir) ? "HKCU" : null;
    }

    /// <summary>The folder saved by <see cref="SaveChoice"/>, if any (HKLM first), whether or not it is still valid.</summary>
    public static string? SavedChoice(IGameEnvironment env) =>
        env.GetString(RegistryHive.LocalMachine, RegistryView.Registry64, SavedKey, SavedValue)
        ?? env.GetString(RegistryHive.CurrentUser, RegistryView.Registry64, SavedKey, SavedValue);
}

/// <summary>The real registry and file system.</summary>
public sealed class SystemGameEnvironment : IGameEnvironment
{
    static bool Expected(Exception e) => e is System.Security.SecurityException or UnauthorizedAccessException or IOException;

    public string? GetString(RegistryHive hive, RegistryView view, string subKey, string name)
    {
        try
        {
            using var b = RegistryKey.OpenBaseKey(hive, view);
            using var k = b.OpenSubKey(subKey);
            return k?.GetValue(name) as string;
        }
        catch (Exception e) when (Expected(e)) { return null; }
    }

    public IReadOnlyList<string> GetSubKeyNames(RegistryHive hive, RegistryView view, string subKey)
    {
        try
        {
            using var b = RegistryKey.OpenBaseKey(hive, view);
            using var k = b.OpenSubKey(subKey);
            return k?.GetSubKeyNames() ?? [];
        }
        catch (Exception e) when (Expected(e)) { return []; }
    }

    public bool SetString(RegistryHive hive, RegistryView view, string subKey, string name, string value)
    {
        try
        {
            using var b = RegistryKey.OpenBaseKey(hive, view);
            using var k = b.CreateSubKey(subKey, writable: true);
            k.SetValue(name, value, RegistryValueKind.String);
            return true;
        }
        catch (Exception e) when (Expected(e)) { return false; }
    }

    public bool DeleteKey(RegistryHive hive, RegistryView view, string subKey)
    {
        try
        {
            using var b = RegistryKey.OpenBaseKey(hive, view);
            b.DeleteSubKeyTree(subKey, throwOnMissingSubKey: false);
            return true;
        }
        catch (Exception e) when (Expected(e)) { return false; }
    }

    public bool FileExists(string path)
    {
        try { return File.Exists(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    public IReadOnlyList<string> FindOtherSf2Exes(string dir)
    {
        try
        {
            return Directory.Exists(dir)
                ? Directory.GetFiles(dir, "StrikeFighters*.exe").Select(p => Path.GetFileName(p)).ToList()
                : [];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { return []; }
    }

    public (uint TimeDateStamp, uint SizeOfImage)? ReadPeStamp(string exePath)
    {
        try
        {
            using var fs = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new BinaryReader(fs);
            if (fs.Length < 0x40 || r.ReadUInt16() != 0x5A4D) return null;
            fs.Position = 0x3C;
            uint pe = r.ReadUInt32();
            if (pe + 24 + 60 > fs.Length) return null;
            fs.Position = pe;
            if (r.ReadUInt32() != 0x00004550) return null;
            fs.Position = pe + 8;
            uint stamp = r.ReadUInt32();
            fs.Position = pe + 24 + 56;
            return (stamp, r.ReadUInt32());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }
}
