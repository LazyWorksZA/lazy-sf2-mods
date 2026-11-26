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

    // Tacview present: its AppData folder exists.
    public TacviewAddOnTests() => Directory.CreateDirectory(Path.Combine(_appData, "Tacview"));

    public void Dispose()
    {
        // Junctions first (only these two places get one), so the recursive delete cannot follow them.
        foreach (string d in new[] { OldDir, Path.Combine(_appData, "Tacview") })
            if (Directory.Exists(d) && new DirectoryInfo(d).Attributes.HasFlag(FileAttributes.ReparsePoint)) Directory.Delete(d);
        try { Directory.Delete(_appData, recursive: true); } catch (IOException) { }
        if (_target != null) try { Directory.Delete(_target, recursive: true); } catch (IOException) { }
    }

    string? _target;

    void MakeJunction(string link)
    {
        _target = Path.Combine(Path.GetTempPath(), "lsf2j-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(_target);
        File.WriteAllText(Path.Combine(_target, "main.lua"), "elsewhere");
        Directory.CreateDirectory(Path.GetDirectoryName(link)!);
        using var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{_target}\"")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
        p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        Assert.True(Directory.Exists(link), "mklink /J failed");
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
    public void Fresh_install_writes_the_file_and_enables_it_with_the_marker()
    {
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
    }

    [Fact]
    public void Tacviews_own_0_without_the_marker_is_turned_on_once()
    {
        _reg.SetDword(Key, "Enabled", 0);  // Tacview writes this itself for a new add-on (and 1.0 players have it)
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
    }

    [Fact]
    public void With_the_marker_a_players_0_stays()
    {
        _reg.SetDword(Key, "Enabled", 0);
        _reg.SetDword(Key, "EnabledByInstaller", 1);
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void Turned_off_after_install_stays_off_on_the_next_hook()
    {
        TacviewAddOn.Install(_reg, _appData, _content, false);
        _reg.SetDword(Key, "Enabled", 0);  // the player turns it off in Tacview
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void Uninstall_and_install_in_either_order_keep_the_players_choice()
    {
        TacviewAddOn.Install(_reg, _appData, _content, false);
        _reg.SetDword(Key, "Enabled", 0);
        _reg.SetDword(Key, "TheatreSwitch", 0);
        TacviewAddOn.Uninstall(_appData);
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(0, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
        TacviewAddOn.Install(_reg, _appData, _content, false);
        TacviewAddOn.Uninstall(_appData);
        Assert.Equal(0, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(0, _reg.GetDword(Key, "TheatreSwitch"));
    }

    [Fact]
    public void Without_Tacview_nothing_is_done()
    {
        Directory.Delete(Path.Combine(_appData, "Tacview"));
        var log = TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Contains(log, l => l.Contains("Tacview not found"));
        Assert.False(Directory.Exists(Path.Combine(_appData, "Tacview")));
        Assert.Empty(_reg.Keys);
    }

    [Fact]
    public void Tacviews_registry_key_alone_counts_as_present()
    {
        Directory.Delete(Path.Combine(_appData, "Tacview"));
        _reg.SetDword(TacviewAddOn.TacviewKey, "x", 1);
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(_content, File.ReadAllBytes(AddOnFile));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
    }

    [Fact]
    public void While_Tacview_runs_the_marker_is_not_written()
    {
        _reg.SetDword(Key, "Enabled", 0);
        var log = TacviewAddOn.Install(_reg, _appData, _content, tacviewRunning: true);
        Assert.Contains(log, l => l.Contains("Tacview is running"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Null(_reg.GetDword(Key, "EnabledByInstaller"));
        _reg.SetDword(Key, "Enabled", 0);  // Tacview wrote its settings back on closing
        TacviewAddOn.Install(_reg, _appData, _content, tacviewRunning: false);
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
        Assert.Equal(1, _reg.GetDword(Key, "EnabledByInstaller"));
    }

    [Fact]
    public void A_junctioned_old_add_on_folder_is_not_emptied()
    {
        MakeJunction(OldDir);
        var log = TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Contains(log, l => l.Contains("remove old add-on files failed"));
        Assert.True(File.Exists(Path.Combine(_target!, "main.lua")));
    }

    [Fact]
    public void A_junctioned_Tacview_folder_is_refused_on_uninstall()
    {
        Directory.Delete(Path.Combine(_appData, "Tacview"));
        MakeJunction(Path.Combine(_appData, "Tacview"));
        Directory.CreateDirectory(Path.Combine(_target!, "AddOns", "LazySF2Patch"));
        File.WriteAllText(Path.Combine(_target!, "AddOns", "LazySF2Patch", "main.lua"), "elsewhere");
        var log = TacviewAddOn.Uninstall(_appData);
        Assert.Contains(log, l => l.Contains("remove add-on failed"));
        Assert.True(File.Exists(Path.Combine(_target!, "AddOns", "LazySF2Patch", "main.lua")));
    }

    [Fact]
    public void Install_replaces_an_older_add_on_file()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(AddOnFile)!);
        File.WriteAllText(AddOnFile, "old");
        TacviewAddOn.Install(_reg, _appData, _content, false);
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

        TacviewAddOn.Install(_reg, _appData, _content, false);

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
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Equal(1, _reg.GetDword(Key, "TheatreSwitch"));
        Assert.False(_reg.KeyExists(OldKey));
    }

    [Fact]
    public void Old_folder_with_other_files_is_kept()
    {
        Directory.CreateDirectory(OldDir);
        File.WriteAllText(Path.Combine(OldDir, "main.lua"), "old");
        File.WriteAllText(Path.Combine(OldDir, "notes.txt"), "player's");
        TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.False(File.Exists(Path.Combine(OldDir, "main.lua")));
        Assert.True(File.Exists(Path.Combine(OldDir, "notes.txt")));
    }

    [Fact]
    public void Uninstall_removes_the_file_and_folder_and_leaves_Tacviews_key()
    {
        TacviewAddOn.Install(_reg, _appData, _content, false);
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
        TacviewAddOn.Install(_reg, _appData, _content, false);
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
            log = TacviewAddOn.Install(_reg, _appData, _content, false);
        Assert.Contains(log, l => l.Contains("write add-on failed"));
        Assert.False(File.Exists(AddOnFile + ".tmp"));
        Assert.Equal(1, _reg.GetDword(Key, "Enabled"));
    }
}
