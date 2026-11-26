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

    public void DeleteIfEmpty(string key)
    {
        if (!Keys.TryGetValue(key, out var v) || v.Count > 0) return;
        if (Keys.Keys.Any(k => k.StartsWith(key + "\\", StringComparison.OrdinalIgnoreCase))) return;
        Keys.Remove(key);
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
    public void Fresh_install_without_Tacview_writes_the_file_and_enables_it()
    {
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void Install_keeps_a_value_the_player_already_has()
    {
        _reg.SetDword(Key, "Enabled", 0);
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void Update_never_enables()
    {
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: false);
        Assert.Null(_reg.GetDword(Key, "Enabled"));
        _reg.SetDword(Key, "Enabled", 0);
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: false);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
    }

    [Fact]
    public void Install_replaces_an_older_add_on_file()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AddOnFile)!);
        File.WriteAllText(AddOnFile, "old");
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: false);
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

        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);

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
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: false);
        Assert.Equal(1, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.False(_reg.KeyExists(OldKey));
    }

    [Fact]
    public void Old_folder_with_other_files_is_kept()
    {
        Directory.CreateDirectory(OldDir);
        File.WriteAllText(Path.Combine(OldDir, "main.lua"), "old");
        File.WriteAllText(Path.Combine(OldDir, "notes.txt"), "player's");
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        Assert.False(File.Exists(Path.Combine(OldDir, "main.lua")));
        Assert.True(File.Exists(Path.Combine(OldDir, "notes.txt")));
    }

    [Fact]
    public void Uninstall_removes_the_file_folder_and_key_only()
    {
        _reg.SetDword(TacviewAddOn.AddOnsKey + @"\other", "Enabled", 1);
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        _reg.SetDword(Key, "TheatreSwitch", 0);

        TacviewAddOn.Uninstall(_reg, _appData);

        Assert.False(File.Exists(AddOnFile));
        Assert.False(Directory.Exists(Path.GetDirectoryName(AddOnFile)));
        Assert.False(_reg.KeyExists(Key));
        Assert.Equal(1, _reg.GetDword(TacviewAddOn.AddOnsKey + @"\other", "Enabled"));
    }

    [Fact]
    public void Uninstall_without_Tacview_leaves_no_empty_keys()
    {
        TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        TacviewAddOn.Uninstall(_reg, _appData);
        Assert.Equal(["Software"], _reg.Keys.Keys);  // HKCU\Software itself always stays
    }

    [Fact]
    public void Uninstall_with_nothing_there_is_fine()
    {
        var log = TacviewAddOn.Uninstall(_reg, _appData);
        Assert.DoesNotContain(log, l => l.Contains("failed"));
    }

    [Fact]
    public void A_locked_add_on_file_is_logged_and_the_registry_still_set()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AddOnFile)!);
        File.WriteAllText(AddOnFile, "old");
        List<string> log;
        using (new FileStream(AddOnFile, FileMode.Open, FileAccess.Read, FileShare.None))
            log = TacviewAddOn.Install(_reg, _appData, _content, firstInstall: true);
        Assert.Contains(log, l => l.Contains("write add-on failed"));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
    }
}
