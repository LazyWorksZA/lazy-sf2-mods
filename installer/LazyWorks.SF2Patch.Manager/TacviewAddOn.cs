using System.Reflection;
using System.Security;
using Microsoft.Win32;

namespace LazyWorks.SF2Patch.Manager;

/// <summary>The few registry operations the add-on setup needs, on keys relative to HKEY_CURRENT_USER.</summary>
public interface IUserRegistry
{
    bool KeyExists(string key);
    /// <summary>The DWORD value, or null when the key or the value is missing or not a DWORD.</summary>
    int? GetDword(string key, string name);
    /// <summary>Sets a DWORD value, creating the key if needed.</summary>
    void SetDword(string key, string name, int value);
    /// <summary>Deletes the key and everything under it; nothing when it is missing.</summary>
    void DeleteTree(string key);
}

/// <summary>The real HKEY_CURRENT_USER of the user running the helper.</summary>
public sealed class CurrentUserRegistry : IUserRegistry
{
    public bool KeyExists(string key)
    {
        using var k = Registry.CurrentUser.OpenSubKey(key);
        return k != null;
    }

    public int? GetDword(string key, string name)
    {
        using var k = Registry.CurrentUser.OpenSubKey(key);
        if (k?.GetValue(name) is not int v || k.GetValueKind(name) != RegistryValueKind.DWord) return null;
        return v;
    }

    public void SetDword(string key, string name, int value)
    {
        using var k = Registry.CurrentUser.CreateSubKey(key, writable: true);
        k.SetValue(name, value, RegistryValueKind.DWord);
    }

    public void DeleteTree(string key) => Registry.CurrentUser.DeleteSubKeyTree(key, throwOnMissingSubKey: false);
}

/// <summary>
/// Puts the mod's Tacview add-on in place for the installing user and enables it once: Tacview keeps a per-user
/// switch for every add-on folder (AddOns\[folder]\Enabled, a DWORD) and writes 0 for a new add-on itself, so an
/// existing 0 is not the player's choice. The installer sets Enabled=1 once, with the EnabledByInstaller marker, and
/// never again while the marker exists. The key is Tacview's: uninstall leaves it, so whichever order an upgrade runs
/// the hooks in, the key and the player's choice are kept. Also moves the settings of the add-on's pre-rename folder
/// (LazySF2) over. Nothing is done when Tacview is not found. Never throws: every step is best-effort and logged.
/// </summary>
public static class TacviewAddOn
{
    public const string Folder = "LazySF2Patch";
    public const string OldFolder = "LazySF2";
    public const string TacviewKey = @"Software\Raia Software Inc.\Tacview";
    public const string AddOnsKey = TacviewKey + @"\Local\AddOns";
    public const string EnabledValue = "Enabled";
    public const string TheatreSwitchValue = "TheatreSwitch";
    public const string MarkerValue = "EnabledByInstaller";
    const string ResourceName = "LazySF2Patch.main.lua";

    /// <summary>The add-on as the mod embeds it: tacview/addon/LazySF2Patch/main.lua, built into this assembly.</summary>
    public static byte[] Content
    {
        get
        {
            using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName)
                          ?? throw new InvalidOperationException("the Tacview add-on resource is missing");
            using var ms = new MemoryStream();
            s.CopyTo(ms);
            return ms.ToArray();
        }
    }

    public static string AddOnDir(string appData, string folder = Folder) => Path.Combine(appData, "Tacview", "AddOns", folder);

    /// <summary>
    /// Install and update hook. Writes the add-on (when it differs), carries the old LazySF2 settings over and removes
    /// them, then enables the add-on once: only while the EnabledByInstaller marker is missing. Only when Tacview is
    /// present (its AppData folder or its HKCU key). While Tacview runs, the marker is not written, so the next hook
    /// enables it again (Tacview may write its settings back when it closes).
    /// </summary>
    /// <returns>Log lines.</returns>
    public static List<string> Install(IUserRegistry reg, string appData, byte[] content, bool tacviewRunning)
    {
        var log = new List<string>();
        if (!Directory.Exists(Path.Combine(appData, "Tacview")) && !reg.KeyExists(TacviewKey))
        {
            log.Add("tacview: Tacview not found; add-on not installed (the mod writes it on the first recorded flight)");
            return log;
        }
        if (tacviewRunning) log.Add("tacview: Tacview is running; the add-on switch is set but will be set again by the next install or update");
        Step(log, "write add-on", () => log.Add(WriteFile(appData, content)));
        Step(log, "remove old add-on files", () =>
        {
            int n = RemoveFiles(appData, AddOnDir(appData, OldFolder));
            if (n > 0) log.Add($"tacview: removed {n} file(s) of the old LazySF2 add-on");
        });
        string key = AddOnsKey + "\\" + Folder, oldKey = AddOnsKey + "\\" + OldFolder;
        Step(log, "move old settings", () =>
        {
            if (!reg.KeyExists(oldKey)) return;
            int? ts = reg.GetDword(oldKey, TheatreSwitchValue);
            if (ts != null && reg.GetDword(key, TheatreSwitchValue) == null)
            {
                reg.SetDword(key, TheatreSwitchValue, ts.Value);
                log.Add($"tacview: carried TheatreSwitch={ts.Value} over from the old LazySF2 add-on");
            }
            reg.DeleteTree(oldKey);
            log.Add("tacview: removed the old LazySF2 add-on settings");
        });
        Step(log, "enable add-on", () =>
        {
            int? enabled = reg.GetDword(key, EnabledValue);
            if (reg.GetDword(key, MarkerValue) == null)
            {
                reg.SetDword(key, EnabledValue, 1);
                // After Enabled, so a failed write is retried by the next hook; not while Tacview runs, for the same reason.
                if (!tacviewRunning) reg.SetDword(key, MarkerValue, 1);
                log.Add($"tacview: enabled the Lazy SF2 Patch add-on (was {(enabled == null ? "not set" : enabled.Value.ToString())})");
            }
            else
            {
                log.Add($"tacview: add-on switch left as the player set it ({(enabled == null ? "not set" : enabled.Value.ToString())})");
            }
        });
        return log;
    }

    /// <summary>
    /// Uninstall hook: removes the add-on's files and its folder if empty. Tacview's key for it (with the switch and
    /// the marker) stays, so the player's choice survives an upgrade whichever order its hooks run in.
    /// </summary>
    /// <returns>Log lines.</returns>
    public static List<string> Uninstall(string appData)
    {
        var log = new List<string>();
        Step(log, "remove add-on", () => log.Add($"tacview: removed {RemoveFiles(appData, AddOnDir(appData))} add-on file(s)"));
        return log;
    }

    static void Step(List<string> log, string what, Action a)
    {
        try { a(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or SecurityException or ArgumentException)
        {
            log.Add($"tacview: {what} failed: {e.GetType().Name}: {e.Message}");
        }
    }

    static string WriteFile(string appData, byte[] content)
    {
        string dir = AddOnDir(appData);
        if (HasReparsePoint(appData, dir)) return "tacview: add-on not written: a reparse point in its path";
        Directory.CreateDirectory(dir);
        string path = Path.Combine(dir, "main.lua"), tmp = path + ".tmp";
        if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(content)) return "tacview: add-on already up to date";
        try
        {
            File.WriteAllBytes(tmp, content);
            File.Move(tmp, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp)) File.Delete(tmp);
        }
        return "tacview: wrote the add-on (AddOns\\LazySF2Patch\\main.lua)";
    }

    /// <summary>
    /// Deletes main.lua and main.lua.tmp in dir, then dir itself if empty. Returns the files removed. Refuses (throws,
    /// so the step logs it) when a folder in the path below appData is a reparse point.
    /// </summary>
    static int RemoveFiles(string appData, string dir)
    {
        if (!Directory.Exists(dir)) return 0;
        if (HasReparsePoint(appData, dir)) throw new IOException("a reparse point in the add-on path; nothing removed");
        int n = 0;
        foreach (string name in new[] { "main.lua", "main.lua.tmp" })
        {
            string f = Path.Combine(dir, name);
            if (!File.Exists(f)) continue;
            File.Delete(f);
            ++n;
        }
        if (!Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir);
        return n;
    }

    // True when a folder from below appData down to dir is a junction or link: the hook may run elevated.
    static bool HasReparsePoint(string appData, string dir)
    {
        string root = Path.GetFullPath(appData).TrimEnd('\\');
        for (string? d = Path.GetFullPath(dir); d != null && d.Length > root.Length; d = Path.GetDirectoryName(d))
            if (Directory.Exists(d) && new DirectoryInfo(d).Attributes.HasFlag(FileAttributes.ReparsePoint)) return true;
        return false;
    }
}
