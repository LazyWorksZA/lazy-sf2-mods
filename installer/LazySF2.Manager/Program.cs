using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Velopack;

namespace LazySF2.Manager;

/// <summary>
/// Headless helper that Velopack installs with LazySF2. It has no window: it only runs Velopack's
/// install/update/uninstall hooks, which copy the mod into the Strike Fighters 2 folder and remove it
/// again. All settings live in the game (Options > Graphics and Options > Controls).
/// </summary>
static class Program
{
    /// <summary>
    /// Velopack's install root. The app runs from its "current" subfolder, which Velopack replaces on
    /// every update, so state lives one level up.
    /// </summary>
    static string StateDir => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    static string PendingInstallMarker => Path.Combine(StateDir, "pending-install");
    static string LogPath => Path.Combine(StateDir, "lazysf2-setup.log");

    static int Main(string[] args)
    {
        // Must run first: handles --veloapp-* hook invocations (which exit inside Run()).
        VelopackApp.Build()
            .OnAfterInstallFastCallback(_ => HookInstall("install"))
            .OnAfterUpdateFastCallback(_ => HookInstall("update"))
            .OnBeforeUninstallFastCallback(_ => HookUninstall())
            .Run();

        string? game = ModInstaller.FindGameDir();
        switch (args.FirstOrDefault())
        {
            case "--elevated-install": return (int)ModInstaller.Install(game);
            case "--elevated-uninstall": return (int)ModInstaller.Uninstall(game);
        }

        // Started normally: by Velopack right after a per-user install, or by hand. Make sure the mod
        // is in the game folder (with a UAC prompt if the folder needs it), then say where things are.
        bool firstRun = Environment.GetEnvironmentVariable("VELOPACK_FIRSTRUN") != null;
        using (var id = System.Security.Principal.WindowsIdentity.GetCurrent())
        {
            bool elevated = new System.Security.Principal.WindowsPrincipal(id).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            Log($"start: user={id.Name} system={id.IsSystem} elevated={elevated} firstRun={firstRun} session={Process.GetCurrentProcess().SessionId}");
        }
        var result = Ensure(game);
        File.Delete(PendingInstallMarker);
        if (!firstRun || result != ModInstaller.Result.Ok) Report(game, result);
        return (int)result;
    }

    static ModInstaller.Result Ensure(string? game)
    {
        var state = ModInstaller.GetState(game);
        if (state is ModInstaller.State.Installed or ModInstaller.State.Foreign or ModInstaller.State.GameNotFound)
            return state switch
            {
                ModInstaller.State.Foreign => ModInstaller.Result.Foreign,
                ModInstaller.State.GameNotFound => ModInstaller.Result.GameNotFound,
                _ => ModInstaller.Result.Ok,
            };
        var r = ModInstaller.Install(game);
        return r == ModInstaller.Result.AccessDenied ? RunElevated("--elevated-install") ?? r : r;
    }

    static void Report(string? game, ModInstaller.Result r)
    {
        string text = r switch
        {
            ModInstaller.Result.Ok =>
                $"LazySF2 is installed in:\n{game}\n\nSettings are in the game:\n" +
                "  Options > Graphics: Display Mode, Screen\n  Options > Controls: Mouse Pointer\n\n" +
                "To remove it, uninstall LazySF2 from Windows' Installed apps.",
            ModInstaller.Result.GameNotFound => "Strike Fighters 2 wasn't found (no installer registry entry).",
            ModInstaller.Result.Foreign => $"Another winmm.dll is already in\n{game}\nLazySF2 left it untouched.",
            ModInstaller.Result.AccessDenied => "Administrator rights are needed to copy the mod into the game folder.",
            _ => "Couldn't copy the mod into the game folder. Is Strike Fighters 2 running? Close it and run LazySF2 again.",
        };
        MessageBoxW(IntPtr.Zero, text, "LazySF2", r == ModInstaller.Result.Ok ? 0x40u : 0x30u);  // info / warning
    }

    /// <summary>Runs this exe elevated with one argument and waits. Null if the UAC prompt was declined.</summary>
    static ModInstaller.Result? RunElevated(string arg)
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo(Environment.ProcessPath!, arg) { UseShellExecute = true, Verb = "runas" });
            if (p == null) return null;
            p.WaitForExit();
            return (ModInstaller.Result)p.ExitCode;
        }
        catch (Win32Exception) { return null; }  // UAC declined
    }

    // --- Velopack hooks: no UI, must finish quickly (15-30 s) ---------------------------------

    static void HookInstall(string what)
    {
        string? game = ModInstaller.FindGameDir();
        var r = ModInstaller.Install(game);
        Log($"{what} hook: game={game ?? "(not found)"} -> {r}");
        // Per-user installs can't write a game folder under Program Files: finish on the next normal
        // start (Setup starts us right after installing), with a UAC prompt. The MSI runs elevated.
        if (r == ModInstaller.Result.AccessDenied) File.WriteAllText(PendingInstallMarker, game);
    }

    static void HookUninstall()
    {
        string? game = ModInstaller.FindGameDir();
        var r = ModInstaller.Uninstall(game);
        Log($"uninstall hook: game={game ?? "(not found)"} -> {r}");
        if (r != ModInstaller.Result.AccessDenied || game == null) return;

        // No UI is allowed here, but Windows' own UAC prompt is: delete exactly the manifest's files
        // with an elevated cmd that doesn't depend on our files (Velopack removes them next).
        var files = ModInstaller.InstalledFiles(game).Select(f => $"\"{f}\"");
        try
        {
            Process.Start(new ProcessStartInfo("cmd.exe", "/c del /f /q " + string.Join(' ', files))
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
            });
            Log("uninstall hook: started elevated removal");
        }
        catch (Win32Exception) { Log("uninstall hook: elevated removal declined; the mod files remain in the game folder"); }
    }

    static void Log(string line)
    {
        try { File.AppendAllText(LogPath, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}"); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);
}
