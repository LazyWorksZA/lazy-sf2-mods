using System.Diagnostics;
using Xunit;

namespace LazyWorks.SF2Patch.Manager.Tests;

public sealed class ModInstallerTests : IDisposable
{
    readonly string _root = Path.Combine(Path.GetTempPath(), "lsf2t-" + Guid.NewGuid().ToString("N")[..8]);

    public ModInstallerTests() => Directory.CreateDirectory(_root);

    public void Dispose()
    {
        // Remove junctions first so the recursive delete cannot follow them.
        foreach (string d in Directory.GetDirectories(_root))
            if (new DirectoryInfo(d).Attributes.HasFlag(FileAttributes.ReparsePoint)) Directory.Delete(d);
        try { Directory.Delete(_root, recursive: true); } catch (IOException) { }
    }

    string MakeGameDir()
    {
        string dir = Path.Combine(_root, "game");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "winmm.dll"), "dll");
        File.WriteAllText(Path.Combine(dir, "lazysf2.log"), "log");
        File.WriteAllText(Path.Combine(dir, "other.txt"), "other");
        File.WriteAllLines(Path.Combine(dir, ModInstaller.ManifestName),
            ["winmm.dll", "C:evil.dll", @"..\x", @"sub\winmm.dll", "winmm.dll:ads", " LAZYSF2.LOG ", "other.txt"]);
        return dir;
    }

    string MakeJunction(string target)
    {
        string link = Path.Combine(_root, "junction");
        using var p = Process.Start(new ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
        p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        Assert.True(Directory.Exists(link), "mklink /J failed");
        return link;
    }

    [Fact]
    public void InstalledFiles_returns_only_allow_listed_names()
    {
        string dir = MakeGameDir();
        var files = ModInstaller.InstalledFiles(dir).Select(f => Path.GetFileName(f)!).Order().ToArray();
        Assert.Equal(["lazysf2.log", "winmm.dll"], files);
    }

    [Fact]
    public void Uninstall_deletes_listed_files_and_keeps_others()
    {
        string dir = MakeGameDir();
        Assert.Equal(ModInstaller.Result.Ok, ModInstaller.Uninstall(dir));
        Assert.False(File.Exists(Path.Combine(dir, "winmm.dll")));
        Assert.False(File.Exists(Path.Combine(dir, "lazysf2.log")));
        Assert.True(File.Exists(Path.Combine(dir, "other.txt")));
    }

    [Fact]
    public void Junctioned_game_folder_is_refused()
    {
        string dir = MakeGameDir();
        string payload = Path.Combine(_root, "payload.dll");
        File.WriteAllText(payload, "new");
        string link = MakeJunction(dir);

        Assert.True(ModInstaller.HasReparsePoint(link));
        Assert.Equal(ModInstaller.Result.Failed, ModInstaller.Install(link, payload));
        Assert.Equal(ModInstaller.Result.Failed, ModInstaller.Uninstall(link));
        Assert.Equal("dll", File.ReadAllText(Path.Combine(dir, "winmm.dll")));
        Assert.True(File.Exists(Path.Combine(dir, "lazysf2.log")));
    }

    [Fact]
    public void Junctioned_parent_is_refused()
    {
        string dir = MakeGameDir();
        string link = MakeJunction(_root);
        string viaLink = Path.Combine(link, "game");
        Assert.True(ModInstaller.HasReparsePoint(viaLink));
        Assert.Equal(ModInstaller.Result.Failed, ModInstaller.Uninstall(viaLink));
        Assert.True(File.Exists(Path.Combine(dir, "winmm.dll")));
    }

    [Fact]
    public void Plain_folder_installs()
    {
        string dir = Path.Combine(_root, "fresh");
        Directory.CreateDirectory(dir);
        string payload = Path.Combine(_root, "payload.dll");
        File.WriteAllText(payload, "new");
        Assert.False(ModInstaller.HasReparsePoint(dir));
        Assert.Equal(ModInstaller.Result.Ok, ModInstaller.Install(dir, payload));
        Assert.Equal("new", File.ReadAllText(Path.Combine(dir, "winmm.dll")));
    }

    [Fact]
    public void Install_replaces_an_older_dll()
    {
        string dir = MakeGameDir();
        string payload = Path.Combine(_root, "payload.dll");
        File.WriteAllText(payload, "newer");
        Assert.Equal(ModInstaller.Result.Ok, ModInstaller.Install(dir, payload));
        Assert.Equal("newer", File.ReadAllText(Path.Combine(dir, "winmm.dll")));
        Assert.Contains("winmm.dll", File.ReadAllLines(Path.Combine(dir, ModInstaller.ManifestName)));
    }

    [Fact]
    public void Install_with_dll_held_open_fails_without_asking_for_admin()
    {
        string dir = MakeGameDir();
        string payload = Path.Combine(_root, "payload.dll");
        File.WriteAllText(payload, "newer");
        using (new FileStream(Path.Combine(dir, "winmm.dll"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            Assert.Equal(ModInstaller.Result.Failed, ModInstaller.Install(dir, payload));
        }
    }

    [Fact]
    public void Install_with_manifest_held_open_fails_and_keeps_the_old_dll()
    {
        string dir = MakeGameDir();
        string payload = Path.Combine(_root, "payload.dll");
        File.WriteAllText(payload, "newer");
        string manifest = Path.Combine(dir, ModInstaller.ManifestName);
        using (new FileStream(manifest, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            Assert.Equal(ModInstaller.Result.Failed, ModInstaller.Install(dir, payload));
        }
        Assert.Equal("dll", File.ReadAllText(Path.Combine(dir, "winmm.dll")));
        Assert.True(File.Exists(manifest));
    }

    [Fact]
    public void ElevatedDeleteArgs_refuses_a_percent_in_the_path()
    {
        string dir = Path.Combine(_root, "g%PATH%ame");
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "winmm.dll"), "dll");
        File.WriteAllLines(Path.Combine(dir, ModInstaller.ManifestName), ["winmm.dll"]);
        Assert.Null(Program.ElevatedDeleteArgs(dir));
    }

    [Fact]
    public void ElevatedDeleteArgs_quotes_each_full_path()
    {
        string dir = MakeGameDir();
        File.WriteAllLines(Path.Combine(dir, ModInstaller.ManifestName), ["winmm.dll", "lazysf2.log", ModInstaller.ManifestName]);
        string? args = Program.ElevatedDeleteArgs(dir);
        Assert.NotNull(args);
        Assert.StartsWith("/c del /f /q ", args);
        Assert.Contains($"\"{Path.Combine(dir, "winmm.dll")}\"", args);
        Assert.Contains($"\"{Path.Combine(dir, ModInstaller.ManifestName)}\"", args);
        Assert.Contains($"\"{Path.Combine(dir, "lazysf2.log")}\"", args);
    }
}
