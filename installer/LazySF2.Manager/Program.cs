using System.ComponentModel;
using System.Diagnostics;
using Velopack;

namespace LazySF2.Manager;

static class Program
{
    /// <summary>
    /// Velopack's install root (%LocalAppData%\LazySF2). The app itself runs from its "current"
    /// subfolder, which Velopack replaces on every update, so state lives one level up.
    /// </summary>
    static string StateDir => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    public static string PendingInstallMarker => Path.Combine(StateDir, "pending-install");
    static string LogPath => Path.Combine(StateDir, "manager.log");

    [STAThread]
    static int Main(string[] args)
    {
        // Must run first: handles --veloapp-* hook invocations (which exit inside Run()).
        VelopackApp.Build()
            .OnAfterInstallFastCallback(_ => HookInstall("install"))
            .OnAfterUpdateFastCallback(_ => HookInstall("update"))
            .OnBeforeUninstallFastCallback(_ => HookUninstall())
            .Run();

        // Elevated helpers, started by the window when the game folder needs admin rights.
        if (args.Length > 0)
        {
            string? game = ModInstaller.FindGameDir();
            switch (args[0])
            {
                case "--elevated-install": return Code(ModInstaller.Install(game));
                case "--elevated-uninstall": return Code(ModInstaller.Uninstall(game));
                case "--elevated-mouse" when args.Length > 1 && game != null: return Code(ModInstaller.SetMouseMode(game, args[1]));
            }
        }

        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
        return 0;
    }

    static int Code(ModInstaller.Result r) => (int)r;

    /// <summary>Runs this exe elevated with one argument and waits. Null if the UAC prompt was declined.</summary>
    public static ModInstaller.Result? RunElevated(params string[] args)
    {
        try
        {
            var psi = new ProcessStartInfo(Environment.ProcessPath!, string.Join(' ', args))
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            using var p = Process.Start(psi);
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
        // Without write access to the game folder (e.g. under Program Files), the window finishes the
        // install with a UAC prompt the next time it opens (Setup opens it right after installing).
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
}
