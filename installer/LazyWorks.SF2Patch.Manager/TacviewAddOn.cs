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
    /// <summary>Deletes the key only when it has no values and no subkeys.</summary>
    void DeleteIfEmpty(string key);
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

    public void DeleteIfEmpty(string key)
    {
        using (var k = Registry.CurrentUser.OpenSubKey(key))
        {
            if (k == null || k.ValueCount > 0 || k.SubKeyCount > 0) return;
        }
        Registry.CurrentUser.DeleteSubKey(key, throwOnMissingSubKey: false);
    }
}

/// <summary>
/// Puts the mod's Tacview add-on in place for the installing user and enables it once: Tacview keeps a per-user
/// switch for every add-on folder (AddOns\[folder]\Enabled, a DWORD) and gives a new add-on 0. Also moves the
/// settings of the add-on's pre-rename folder (LazySF2) over. Never throws: every step is best-effort and logged.
/// </summary>
public static class TacviewAddOn
{
    public const string Folder = "LazySF2Patch";
    public const string OldFolder = "LazySF2";
    public const string AddOnsKey = @"Software\Raia Software Inc.\Tacview\Local\AddOns";
    public const string EnabledValue = "Enabled";
    public const string TheatreSwitchValue = "TheatreSwitch";
    const string ResourceName = "LazySF2Patch.main.lua";

    // Parents of our key, innermost first: removed on uninstall only when empty (Tacview may never have been installed).
    static readonly string[] Parents =
        [AddOnsKey, @"Software\Raia Software Inc.\Tacview\Local", @"Software\Raia Software Inc.\Tacview", @"Software\Raia Software Inc."];

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
    /// them, then, on a first install only, enables the add-on unless the player already has a value for it.
    /// </summary>
    /// <returns>Log lines.</returns>
    public static List<string> Install(IUserRegistry reg, string appData, byte[] content, bool firstInstall)
    {
        var log = new List<string>();
        Step(log, "write add-on", () => log.Add(WriteFile(appData, content)));
        Step(log, "remove old add-on files", () =>
        {
            int n = RemoveFiles(AddOnDir(appData, OldFolder));
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
            if (firstInstall && enabled == null)
            {
                reg.SetDword(key, EnabledValue, 1);
                log.Add("tacview: enabled the Lazy SF2 Patch add-on");
            }
            else
            {
                log.Add($"tacview: add-on switch left as it is ({(enabled == null ? "not set" : enabled.Value.ToString())})");
            }
        });
        return log;
    }

    /// <summary>Uninstall hook: removes the add-on's files, its folder if empty, and its registry key.</summary>
    /// <returns>Log lines.</returns>
    public static List<string> Uninstall(IUserRegistry reg, string appData)
    {
        var log = new List<string>();
        Step(log, "remove add-on", () => log.Add($"tacview: removed {RemoveFiles(AddOnDir(appData))} add-on file(s)"));
        Step(log, "remove add-on settings", () =>
        {
            reg.DeleteTree(AddOnsKey + "\\" + Folder);
            foreach (string p in Parents) reg.DeleteIfEmpty(p);
            log.Add("tacview: removed the add-on settings");
        });
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

    /// <summary>Deletes main.lua and main.lua.tmp in dir, then dir itself if empty. Returns the files removed.</summary>
    static int RemoveFiles(string dir)
    {
        if (!Directory.Exists(dir) || new DirectoryInfo(dir).Attributes.HasFlag(FileAttributes.ReparsePoint)) return 0;
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
