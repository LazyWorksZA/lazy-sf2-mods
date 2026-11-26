using Xunit;

namespace LazyWorks.SF2Patch.Manager.Tests;

/// <summary>HKCU in memory: key paths (case-insensitive) to their DWORD values.</summary>
sealed class FakeRegistry : IUserRegistry
{
    public readonly Dictionary<string, Dictionary<string, int>> Keys = new(StringComparer.OrdinalIgnoreCase);

    public bool KeyExists(string key) => Keys.ContainsKey(key);

    public int? GetDword(string key, string name) =>
        Keys.TryGetValue(key, out var v) && v.TryGetValue(name, out int i) ? i : null;

    public void SetDword(string key, string name, int value)
    {
        // Like CreateSubKey: the parents come too.
        for (string? k = key; !string.IsNullOrEmpty(k); k = k.Contains('\\') ? k[..k.LastIndexOf('\\')] : null)
            if (!Keys.ContainsKey(k)) Keys[k] = new(StringComparer.OrdinalIgnoreCase);
        Keys[key][name] = value;
    }

    public void DeleteTree(string key)
    {
        foreach (string k in Keys.Keys.Where(k => k.Equals(key, StringComparison.OrdinalIgnoreCase) ||
                                                  k.StartsWith(key + "\\", StringComparison.OrdinalIgnoreCase)).ToList())
            Keys.Remove(k);
    }
}

public sealed class TacviewAddOnTests : IDisposable
{
    const string Key = TacviewAddOn.AddOnsKey + @"\LazySF2Patch";
    const string OldKey = TacviewAddOn.AddOnsKey + @"\LazySF2";
    readonly string _appData = Path.Combine(Path.GetTempPath(), "lsf2a-" + Guid.NewGuid().ToString("N")[..8]);
    readonly FakeRegistry _reg = new();
    readonly byte[] _content = "-- add-on\n"u8.ToArray();

    public TacviewAddOnTests() => Directory.CreateDirectory(_appData);

    public void Dispose()
    {
        try { Directory.Delete(_appData, recursive: true); } catch (IOException) { }
    }

    string AddOnFile => Path.Combine(_appData, "Tacview", "AddOns", "LazySF2Patch", "main.lua");
    string OldDir => Path.Combine(_appData, "Tacview", "AddOns", "LazySF2");

    [Fact]
    public void Content_is_the_add_on_the_mod_embeds()
    {
        string? dir = AppContext.BaseDirectory;
        while (dir != null && !File.Exists(Path.Combine(dir, "tacview", "addon", "LazySF2Patch", "main.lua")))
            dir = Path.GetDirectoryName(dir);
        Assert.NotNull(dir);
        Assert.Equal(File.ReadAllBytes(Path.Combine(dir, "tacview", "addon", "LazySF2Patch", "main.lua")), TacviewAddOn.Content);
    }

    [Fact]
    public void Fresh_install_without_Tacview_writes_the_file_and_enables_it_with_the_marker()
    {
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
    }

    [Fact]
    public void Tacviews_own_0_without_the_marker_is_turned_on_once()
    {
        _reg.SetDword(Key, "Enabled", 0);  // Tacview writes this itself for a new add-on (and 1.0 players have it)
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
    }

    [Fact]
    public void With_the_marker_a_players_0_stays()
    {
        _reg.SetDword(Key, "Enabled", 0);
        _reg.SetDword(Key, "EnabledByInstaller", 1);
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void Turned_off_after_install_stays_off_on_the_next_hook()
    {
        TacviewAddOn.Install(_reg, _appData, _content);
        _reg.SetDword(Key, "Enabled", 0);  // the player turns it off in Tacview
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void An_MSI_upgrade_keeps_the_players_choice()
    {
        // The MSI runs the old version's uninstall hook, then the new version's install hook.
        TacviewAddOn.Install(_reg, _appData, _content);
        _reg.SetDword(Key, "Enabled", 0);
        _reg.SetDword(Key, "TheatreSwitch", 0);
        TacviewAddOn.Uninstall(_appData);
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(0, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
    }

    [Fact]
    public void Install_replaces_an_older_add_on_file()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AddOnFile)!);
        File.WriteAllText(AddOnFile, "old");
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
    }

    [Fact]
    public void Old_LazySF2_settings_are_carried_over_and_removed()
    {
        _reg.SetDword(OldKey, "Enabled", 1);
        _reg.SetDword(OldKey, "TheatreSwitch", 0);
        Directory.CreateDirectory(OldDir);
        File.WriteAllText(Path.Combine(OldDir, "main.lua"), "old");
        File.WriteAllText(Path.Combine(OldDir, "main.lua.tmp"), "old");

        TacviewAddOn.Install(_reg, _appData, _content);

        Assert.False(_reg.KeyExists(OldKey));
        Assert.Equal(0, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.False(Directory.Exists(OldDir));
    }

    [Fact]
    public void Old_TheatreSwitch_does_not_override_a_new_one()
    {
        _reg.SetDword(OldKey, "TheatreSwitch", 0);
        _reg.SetDword(Key, "TheatreSwitch", 1);
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Equal(1, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.False(_reg.KeyExists(OldKey));
    }

    [Fact]
    public void Old_folder_with_other_files_is_kept()
    {
        Directory.CreateDirectory(OldDir);
        File.WriteAllText(Path.Combine(OldDir, "main.lua"), "old");
        File.WriteAllText(Path.Combine(OldDir, "notes.txt"), "player's");
        TacviewAddOn.Install(_reg, _appData, _content);
        Assert.False(File.Exists(Path.Combine(OldDir, "main.lua")));
        Assert.True(File.Exists(Path.Combine(OldDir, "notes.txt")));
    }

    [Fact]
    public void Uninstall_removes_the_file_and_folder_and_leaves_Tacviews_key()
    {
        TacviewAddOn.Install(_reg, _appData, _content);
        _reg.SetDword(Key, "TheatreSwitch", 0);

        TacviewAddOn.Uninstall(_appData);

        Assert.False(File.Exists(AddOnFile));
        Assert.False(Directory.Exists(Path.GetDirectoryName(AddOnFile)));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
        Assert.Equal(0, _reg.GetDword(Key, "TheatreSwitch"));
    }

    [Fact]
    public void Uninstall_keeps_a_folder_with_other_files()
    {
        TacviewAddOn.Install(_reg, _appData, _content);
        File.WriteAllText(AddOnFile + ".tmp", "partial");
        string other = Path.Combine(Path.GetDirectoryName(AddOnFile)!, "notes.txt");
        File.WriteAllText(other, "player's");
        TacviewAddOn.Uninstall(_appData);
        Assert.False(File.Exists(AddOnFile));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.True(File.Exists(other));
    }

    [Fact]
    public void Uninstall_with_nothing_there_is_fine()
    {
        var log = TacviewAddOn.Uninstall(_appData);
        Assert.DoesNotContain(log, l => l.Contains("failed"));
    }

    [Fact]
    public void A_locked_add_on_file_is_logged_and_the_registry_still_set()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AddOnFile)!);
        File.WriteAllText(AddOnFile, "old");
        List<string> log;
        using (new FileStream(AddOnFile, FileMode.Open, FileAccess.Read, FileShare.None))
            log = TacviewAddOn.Install(_reg, _appData, _content);
        Assert.Contains(log, l => l.Contains("write add-on failed"));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
    }
}
