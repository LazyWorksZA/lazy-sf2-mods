using Microsoft.Win32;
using Xunit;

namespace LazyWorks.SF2Patch.Manager.Tests;

sealed class FakeGameEnvironment : IGameEnvironment
{
    public readonly Dictionary<string, string> Values = new(StringComparer.OrdinalIgnoreCase);
    public readonly HashSet<string> Files = new(StringComparer.OrdinalIgnoreCase);
    public readonly Dictionary<string, (uint, uint)> Stamps = new(StringComparer.OrdinalIgnoreCase);
    public bool HklmWritable = true;

    static string K(RegistryHive h, RegistryView v, string key, string name)
        => $"{h}|{(v == RegistryView.Registry32 ? "32" : "64")}|{key}|{name}";

    public void Set(RegistryHive h, RegistryView v, string key, string name, string value) => Values[K(h, v, key, name)] = value;

    public string? GetString(RegistryHive h, RegistryView v, string key, string name) => Values.GetValueOrDefault(K(h, v, key, name));

    public IReadOnlyList<string> GetSubKeyNames(RegistryHive h, RegistryView v, string key)
    {
        string prefix = $"{h}|{K(h, v, "", "").Split('|')[1]}|{key}\\";
        return Values.Keys.Where(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            .Select(k => k[prefix.Length..].Split('|')[0].Split('\\')[0]).Distinct().ToList();
    }

    public bool SetString(RegistryHive h, RegistryView v, string key, string name, string value)
    {
        if (h == RegistryHive.LocalMachine && !HklmWritable) return false;
        Set(h, v, key, name, value);
        return true;
    }

    public bool DeleteKey(RegistryHive h, RegistryView v, string key)
    {
        if (h == RegistryHive.LocalMachine && !HklmWritable) return false;
        foreach (string k in Values.Keys.Where(k => k.StartsWith($"{h}|{K(h, v, "", "").Split('|')[1]}|{key}|", StringComparison.OrdinalIgnoreCase)).ToList())
            Values.Remove(k);
        return true;
    }

    public bool FileExists(string path) => Files.Contains(path);

    public IReadOnlyList<string> FindOtherSf2Exes(string dir) =>
        Files.Where(f => Path.GetDirectoryName(f)!.Equals(dir, StringComparison.OrdinalIgnoreCase)
                         && Path.GetFileName(f).StartsWith("StrikeFighters", StringComparison.OrdinalIgnoreCase)
                         && !Path.GetFileName(f).Equals(GameLocator.ExeName, StringComparison.OrdinalIgnoreCase))
             .Select(f => Path.GetFileName(f)).ToList();

    public (uint TimeDateStamp, uint SizeOfImage)? ReadPeStamp(string exePath) => Stamps.TryGetValue(exePath, out var s) ? s : null;
}

public sealed class FolderDialogTests
{
    [Fact]
    public void The_folder_dialog_can_be_created_and_configured_on_an_STA_thread()
    {
        uint opts = 0;
        Exception? error = null;
        var t = new Thread(() => { try { opts = Native.ProbeFolderDialog(); } catch (Exception e) { error = e; } });
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
        t.Join();
        Assert.Null(error);
        Assert.Equal(0x20u | 0x40u, opts & (0x20u | 0x40u));
    }
}

public sealed class GameLocatorTests
{
    const string Sf2Key = @"SOFTWARE\ThirdWire\Strike Fighters 2";
    const string Uninst = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
    const RegistryHive LM = RegistryHive.LocalMachine, CU = RegistryHive.CurrentUser;
    const RegistryView V32 = RegistryView.Registry32, V64 = RegistryView.Registry64;

    readonly FakeGameEnvironment _env = new();
    readonly List<string> _log = [];

    string? Find() => GameLocator.Find(_env, _log.Add);

    string Game(string dir)
    {
        string exe = Path.Combine(dir, GameLocator.ExeName);
        _env.Files.Add(exe);
        _env.Stamps[exe] = (GameLocator.ExeTimestamp, GameLocator.ExeImageSize);
        return dir;
    }

    [Fact]
    public void Nothing_registered_finds_nothing_and_logs_every_place()
    {
        Assert.Null(Find());
        Assert.Equal("game folder: not found", _log[^1]);
        Assert.Contains(_log, l => l.Contains("HKLM 32-bit"));
        Assert.Contains(_log, l => l.Contains("HKLM 64-bit"));
        Assert.Contains(_log, l => l.Contains("VirtualStore"));
        Assert.Contains(_log, l => l.Contains("Uninstall"));
        Assert.Contains(_log, l => l.Contains("saved folder"));
    }

    [Fact]
    public void Hklm32_comes_before_hklm64()
    {
        _env.Set(LM, V32, Sf2Key, "Path", Path.Combine(Game(@"C:\a"), GameLocator.ExeName));
        _env.Set(LM, V64, Sf2Key, "Path", Path.Combine(Game(@"C:\b"), GameLocator.ExeName));
        Assert.Equal(@"C:\a", Find());
    }

    [Fact]
    public void Hklm64_is_used_when_32_is_missing()
    {
        _env.Set(LM, V64, Sf2Key, "Path", Path.Combine(Game(@"C:\b"), GameLocator.ExeName));
        Assert.Equal(@"C:\b", Find());
    }

    [Fact]
    public void Stale_hklm_path_falls_through_to_virtual_store()
    {
        _env.Set(LM, V32, Sf2Key, "Path", @"C:\gone\StrikeFighters2.exe");
        _env.Set(CU, V64, @"Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\ThirdWire\Strike Fighters 2", "Path",
            Path.Combine(Game(@"C:\vs"), GameLocator.ExeName));
        Assert.Equal(@"C:\vs", Find());
        Assert.Contains(_log, l => l.Contains(@"C:\gone") && l.Contains("no StrikeFighters2.exe"));
    }

    [Fact]
    public void Virtual_store_non_wow_key_is_tried_too()
    {
        _env.Set(CU, V64, @"Software\Classes\VirtualStore\MACHINE\SOFTWARE\ThirdWire\Strike Fighters 2", "Path",
            Path.Combine(Game(@"C:\vs2"), GameLocator.ExeName));
        Assert.Equal(@"C:\vs2", Find());
    }

    [Fact]
    public void Uninstall_entry_uses_install_location()
    {
        _env.Set(LM, V32, Uninst + @"\SF2", "DisplayName", "Strike Fighters 2 Merged");
        _env.Set(LM, V32, Uninst + @"\SF2", "InstallLocation", Game(@"D:\Games\SF2"));
        _env.Set(LM, V32, Uninst + @"\Other", "DisplayName", "Something else");
        Assert.Equal(@"D:\Games\SF2", Find());
    }

    [Theory]
    [InlineData("\"D:\\g\\Uninstall.exe\" /S")]
    [InlineData("D:\\g\\Uninstall.exe /S")]
    public void Uninstall_entry_falls_back_to_the_uninstall_string_folder(string uninstallString)
    {
        Game(@"D:\g");
        _env.Set(CU, V64, Uninst + @"\SF2", "DisplayName", "Strike Fighters 2");
        _env.Set(CU, V64, Uninst + @"\SF2", "UninstallString", uninstallString);
        Assert.Equal(@"D:\g", Find());
    }

    [Fact]
    public void Uninstall_entry_uses_the_display_icon_folder()
    {
        Game(@"E:\sf2");
        _env.Set(LM, V64, Uninst + @"\SF2", "DisplayName", "Strike Fighters 2");
        _env.Set(LM, V64, Uninst + @"\SF2", "DisplayIcon", @"E:\sf2\StrikeFighters2.exe,0");
        Assert.Equal(@"E:\sf2", Find());
    }

    [Fact]
    public void Other_product_key_is_accepted_only_with_StrikeFighters2_exe()
    {
        _env.Set(LM, V32, @"SOFTWARE\ThirdWire\Strike Fighters 2 Vietnam", "Path", Path.Combine(Game(@"F:\viet"), GameLocator.ExeName));
        Assert.Equal(@"F:\viet", Find());
    }

    [Fact]
    public void A_different_sf2_exe_is_logged_clearly_and_not_accepted()
    {
        _env.Files.Add(@"F:\viet\StrikeFighters2 Vietnam.exe");
        _env.Set(LM, V32, @"SOFTWARE\ThirdWire\Strike Fighters 2 Vietnam", "Path", @"F:\viet\StrikeFighters2 Vietnam.exe");
        Assert.Null(Find());
        Assert.Contains(_log, l => l.Contains(@"found StrikeFighters2 Vietnam.exe in F:\viet, but the mod needs StrikeFighters2.exe (July 2013 merged install)"));
    }

    [Fact]
    public void A_wrong_build_is_skipped()
    {
        string dir = Game(@"C:\old");
        _env.Stamps[Path.Combine(dir, GameLocator.ExeName)] = (0x12345678, 0x62000);
        _env.Set(LM, V32, Sf2Key, "Path", Path.Combine(dir, GameLocator.ExeName));
        Assert.Null(Find());
        Assert.Contains(_log, l => l.Contains("not the July 2013 build"));
        Assert.Equal(GameLocator.Check.WrongBuild, GameLocator.CheckFolder(_env, dir));
    }

    [Fact]
    public void The_known_build_passes_and_an_unparseable_exe_does_not()
    {
        string a = Game(@"C:\ok");
        _env.Stamps[Path.Combine(a, GameLocator.ExeName)] = (GameLocator.ExeTimestamp, GameLocator.ExeImageSize);
        Assert.Equal(GameLocator.Check.Ok, GameLocator.CheckFolder(_env, a));
        _env.Files.Add(@"C:\notpe\StrikeFighters2.exe");
        Assert.Equal(GameLocator.Check.WrongBuild, GameLocator.CheckFolder(_env, @"C:\notpe"));
        Assert.Equal(GameLocator.Check.NoExe, GameLocator.CheckFolder(_env, @"C:\empty"));
    }

    [Fact]
    public void Saved_folder_is_the_last_resort()
    {
        _env.Set(LM, V64, GameLocator.SavedKey, GameLocator.SavedValue, Game(@"G:\mine"));
        Assert.Equal(@"G:\mine", Find());
        _env.Set(LM, V32, Sf2Key, "Path", Path.Combine(Game(@"C:\reg"), GameLocator.ExeName));
        Assert.Equal(@"C:\reg", Find());
    }

    [Fact]
    public void Saved_folder_round_trips_through_hkcu()
    {
        Assert.Equal("HKCU", GameLocator.SaveChoice(_env, Game(@"G:\mine")));
        Assert.Equal(@"G:\mine", GameLocator.SavedChoice(_env));
        Assert.Equal(@"G:\mine", Find());
    }

    [Fact]
    public void Uninstall_skips_an_invalid_saved_folder()
    {
        _env.Set(LM, V64, GameLocator.SavedKey, GameLocator.SavedValue, @"C:\Windows\System32");
        Assert.Empty(Program.UninstallTargets(_env, _log.Add));
        Assert.Contains(_log, l => l.Contains("saved folder") && l.Contains("skipped"));
    }

    [Fact]
    public void Elevated_install_refuses_an_invalid_folder_and_saves_nothing()
    {
        Assert.Equal(ModInstaller.Result.GameNotFound, Program.ElevatedInstall(@"C:\nowhere", _env, _log.Add));
        Assert.Null(GameLocator.SavedChoice(_env));
    }

    [Theory]
    [InlineData(@"C:\Games\SF2", "\"C:\\Games\\SF2\"")]
    [InlineData(@"C:\Games\SF2\", "\"C:\\Games\\SF2\"")]
    [InlineData(@"C:\", "\"C:\\\\\"")]
    public void QuoteArg_never_lets_a_backslash_escape_the_quote(string dir, string expected) =>
        Assert.Equal(expected, Program.QuoteArg(dir));

    [Fact]
    public void Uninstall_targets_include_the_saved_folder_and_keep_the_key()
    {
        GameLocator.SaveChoice(_env, Game(@"G:\mine"));
        Assert.Equal([@"G:\mine"], Program.UninstallTargets(_env, _log.Add));
        Assert.Equal(@"G:\mine", GameLocator.SavedChoice(_env));
    }

    [Fact]
    public void Saved_folder_goes_to_hkcu_whatever_hklm_allows()
    {
        _env.HklmWritable = false;
        Assert.Equal("HKCU", GameLocator.SaveChoice(_env, Game(@"G:\mine")));
        Assert.Equal(@"G:\mine", GameLocator.SavedChoice(_env));
        Assert.Equal(@"G:\mine", Find());
    }

    [Theory]
    [InlineData("\"C:\\a b\\x.exe\",0", "C:\\a b\\x.exe")]
    [InlineData("C:\\a\\x.exe,0", "C:\\a\\x.exe")]
    [InlineData("C:\\a b\\", "C:\\a b")]
    [InlineData("  \"C:\\a\"  ", "C:\\a")]
    public void ExtractPath_strips_quotes_arguments_and_icon_index(string raw, string expected) =>
        Assert.Equal(expected, GameLocator.ExtractPath(raw));

    [Fact]
    public void ReadPeStamp_reads_the_header_of_a_real_layout()
    {
        string path = Path.Combine(Path.GetTempPath(), "lsf2pe-" + Guid.NewGuid().ToString("N")[..8] + ".exe");
        try
        {
            var b = new byte[0x200];
            b[0] = 0x4D; b[1] = 0x5A; b[0x3C] = 0x80;
            b[0x80] = 0x50; b[0x81] = 0x45;
            BitConverter.GetBytes(GameLocator.ExeTimestamp).CopyTo(b, 0x80 + 8);
            BitConverter.GetBytes(GameLocator.ExeImageSize).CopyTo(b, 0x80 + 24 + 56);
            File.WriteAllBytes(path, b);
            Assert.Equal((GameLocator.ExeTimestamp, GameLocator.ExeImageSize), new SystemGameEnvironment().ReadPeStamp(path));
            File.WriteAllBytes(path, new byte[10]);
            Assert.Null(new SystemGameEnvironment().ReadPeStamp(path));
        }
        finally { File.Delete(path); }
    }
}
