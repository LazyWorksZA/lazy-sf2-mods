using System.ComponentModel;
using System.Diagnostics;
using Velopack;

namespace LazyWorks.SF2Patch.Manager;

/// <summary>
/// Headless helper that Velopack installs with the Lazy SF2 Patch. It has no window: it only runs Velopack's
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

    [STAThread]  // the folder picker is a COM dialog
    static int Main(string[] args)
    {
        // Must run first: handles --veloapp-* hook invocations (which exit inside Run()).
        VelopackApp.Build()
            .OnAfterInstallFastCallback(_ => HookInstall("install"))
            .OnAfterUpdateFastCallback(_ => HookInstall("update"))
            .OnBeforeUninstallFastCallback(_ => HookUninstall())
            .Run();

        switch (args.FirstOrDefault())
        {
            case "--elevated-install": return (int)ElevatedInstall(args.Length > 1 ? args[1] : null, Env, Log);
            case "--elevated-uninstall": return (int)ModInstaller.Uninstall(ModInstaller.FindGameDir(Log));
        }

        // Started normally: by Velopack right after a per-user install, from the Start menu, or by hand.
        // Make sure the mod is in the game folder (with a UAC prompt if the folder needs it), then say
        // where things are, and offer to choose the folder when the game wasn't found.
        bool firstRun = Environment.GetEnvironmentVariable("VELOPACK_FIRSTRUN") != null;
        using (var id = System.Security.Principal.WindowsIdentity.GetCurrent())
        {
            bool elevated = new System.Security.Principal.WindowsPrincipal(id).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            Log($"start: user={id.Name} system={id.IsSystem} elevated={elevated} firstRun={firstRun} session={Process.GetCurrentProcess().SessionId}");
        }
        string? game = ModInstaller.FindGameDir(Log);
        var result = Ensure(game);
        File.Delete(PendingInstallMarker);
        if (firstRun && result == ModInstaller.Result.Ok) return (int)result;
        return (int)Interact(game, result);
    }

    static readonly SystemGameEnvironment Env = new();

    /// <summary>The elevated half of a manual install: the folder came from the command line, so it is checked again.</summary>
    internal static ModInstaller.Result ElevatedInstall(string? folder, IGameEnvironment env, Action<string> log)
    {
        if (folder != null && GameLocator.CheckFolder(env, folder) is var c && c != GameLocator.Check.Ok)
        {
            log($"elevated install: {folder} is not a valid game folder ({c})");
            return ModInstaller.Result.GameNotFound;
        }
        var r = ModInstaller.Install(folder ?? GameLocator.Find(env, log));
        if (r == ModInstaller.Result.Ok && folder != null) log($"saved game folder in {GameLocator.SaveChoice(env, folder) ?? "(nowhere)"}");
        return r;
    }

    /// <summary>Shows the status in a TaskDialog and lets the player choose the game folder, until they close it.</summary>
    static ModInstaller.Result Interact(string? game, ModInstaller.Result result)
    {
        while (true)
        {
            var (heading, text, icon, choose) = Describe(game, result);
            if (Native.Show(heading, text, icon, choose) != Native.ButtonChoose) return result;
            string? picked = Native.PickFolder("Choose your Strike Fighters 2 folder (the one with StrikeFighters2.exe)");
            if (picked == null) continue;
            var check = GameLocator.CheckFolder(Env, picked);
            Log($"chosen folder {picked}: {check}");
            if (check != GameLocator.Check.Ok)
            {
                Native.Message(check == GameLocator.Check.NoExe
                    ? $"{GameLocator.ExeName} isn't in\n{picked}\n\nChoose the folder that contains it. The Lazy SF2 Patch is for the July 2013 version of Strike Fighters 2 (the merged install)."
                    : check == GameLocator.Check.Unsafe
                    ? $"{picked}\nis (or is inside) a junction or symbolic link, which the installer won't write through. Choose the real folder."
                    : $"The {GameLocator.ExeName} in\n{picked}\nisn't the July 2013 version of Strike Fighters 2, so the Lazy SF2 Patch can't be used with it.", warning: true);
                continue;
            }
            game = picked;
            result = InstallChosen(picked);
            Log($"chosen folder install -> {result}");
        }
    }

    /// <summary>A quoted command-line argument for a folder. A trailing backslash would escape the quote, so it is dropped, except on a drive root, where it is doubled.</summary>
    internal static string QuoteArg(string dir)
    {
        bool root = Path.GetPathRoot(dir) is { } r && r.Equals(dir, StringComparison.OrdinalIgnoreCase);
        string d = dir.TrimEnd('\\', '/');
        return root ? "\"" + d + "\\\\\"" : "\"" + d + "\"";
    }

    /// <summary>Installs into a folder the player chose and remembers it. Elevates (UAC) when the folder needs it.</summary>
    static ModInstaller.Result InstallChosen(string dir)
    {
        if (GameLocator.CheckFolder(Env, dir) != GameLocator.Check.Ok) return ModInstaller.Result.GameNotFound;
        var r = ModInstaller.Install(dir);
        if (r == ModInstaller.Result.AccessDenied)
        {
            // The elevated copy checks the folder again and saves it (HKLM) itself.
            return RunElevated("--elevated-install " + QuoteArg(dir)) ?? r;
        }
        if (r == ModInstaller.Result.Ok) Log($"saved game folder in {GameLocator.SaveChoice(Env, dir) ?? "(nowhere)"}");
        return r;
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

    static (string Heading, string Text, Native.Icon Icon, string Choose) Describe(string? game, ModInstaller.Result r) => r switch
    {
        ModInstaller.Result.Ok => ("Lazy SF2 Patch is installed",
            $"Game folder:\n{game}\n\nSettings are in the game:\n" +
            "  Options > Graphics: Display Mode, Screen\n  Options > Controls: Mouse Pointer, Pilot Twist\n" +
            "  Options > Gameplay: Tacview Recording\n\n" +
            "To remove it, uninstall Lazy SF2 Patch from Windows' Installed apps.",
            Native.Icon.Information, "Use a different game folder..."),
        ModInstaller.Result.GameNotFound => ("Strike Fighters 2 wasn't found",
            "The installer looked for the game and didn't find the July 2013 StrikeFighters2.exe.\n\n" +
            "Choose the folder that contains StrikeFighters2.exe. The places it looked are in lazysf2-setup.log.",
            Native.Icon.Warning, "Choose the game folder..."),
        ModInstaller.Result.Foreign => ("Another winmm.dll is already there",
            $"Another winmm.dll is already in\n{game}\nThe Lazy SF2 Patch left it untouched.",
            Native.Icon.Warning, "Use a different game folder..."),
        ModInstaller.Result.AccessDenied => ("Administrator rights are needed",
            "Administrator rights are needed to copy the mod into the game folder.",
            Native.Icon.Warning, "Choose the game folder..."),
        _ => ("Couldn't copy the mod into the game folder",
            "Is Strike Fighters 2 running? Close it and run Lazy SF2 Patch again.",
            Native.Icon.Warning, "Use a different game folder..."),
    };

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
        Log($"{what} hook: user={CurrentUser()}");
        // First, so nothing in the game-folder part can keep it from running.
        TacviewHook(() => TacviewAddOn.Install(new CurrentUserRegistry(), AppData, TacviewAddOn.Content, TacviewRunning()));
        string? game = ModInstaller.FindGameDir(Log);
        if (game != null && ModInstaller.HasReparsePoint(game)) Log("install hook: refused, reparse point in game path");
        var r = ModInstaller.Install(game);
        Log($"{what} hook: game={game ?? "(not found)"} -> {r}");
        // Per-user installs can't write a game folder under Program Files: finish on the next normal
        // start (Setup starts us right after installing), with a UAC prompt. The MSI runs elevated.
        if (r == ModInstaller.Result.AccessDenied)
        {
            try { File.WriteAllText(PendingInstallMarker, game); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Log($"{what} hook: could not write the pending-install marker: {e.Message}"); }
        }
    }

    static string CurrentUser()
    {
        using var id = System.Security.Principal.WindowsIdentity.GetCurrent();
        return id.Name;
    }

    /// <summary>True when a Tacview process (Tacview.exe or Tacview64.exe) is running in any session.</summary>
    static bool TacviewRunning()
    {
        foreach (string name in new[] { "Tacview", "Tacview64" })
        {
            var ps = Process.GetProcessesByName(name);
            bool any = ps.Length > 0;
            foreach (var p in ps) p.Dispose();
            if (any) return true;
        }
        return false;
    }

    static string AppData => Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);

    /// <summary>
    /// The Tacview add-on for the user running the hook (the MSI runs it impersonated, as the installing user).
    /// Whatever happens, the hook goes on.
    /// </summary>
    static void TacviewHook(Func<List<string>> step)
    {
        try
        {
            if (string.IsNullOrEmpty(AppData)) { Log("tacview: no AppData folder; add-on skipped"); return; }
            foreach (string line in step()) Log(line);
        }
        catch (Exception e) { Log($"tacview: add-on step failed: {e.GetType().Name}: {e.Message}"); }
    }

    static void HookUninstall()
    {
        Log($"uninstall hook: user={CurrentUser()}");
        TacviewHook(() => TacviewAddOn.Uninstall(AppData));
        // The detected folder and the one the player chose (usually the same). The saved choice stays in the
        // registry: an upgrade may run the old uninstall after the new install, and a reinstall finds it.
        foreach (string game in UninstallTargets(Env, Log)) UninstallFrom(game);
    }

    internal static IEnumerable<string> UninstallTargets(IGameEnvironment env, Action<string> log)
    {
        var targets = new List<string>();
        if (GameLocator.Find(env, log) is { } found) targets.Add(found);
        // The saved value is only a hint: check it like any folder before deleting anything in it.
        if (GameLocator.SavedChoice(env) is { } saved && !targets.Contains(saved, StringComparer.OrdinalIgnoreCase))
        {
            var c = GameLocator.CheckFolder(env, saved);
            if (c == GameLocator.Check.Ok) targets.Add(saved);
            else log($"uninstall hook: saved folder {saved} skipped ({c})");
        }
        return targets;
    }

    static void UninstallFrom(string game)
    {
        if (ModInstaller.HasReparsePoint(game)) Log("uninstall hook: refused, reparse point in game path");
        var r = ModInstaller.Uninstall(game);
        Log($"uninstall hook: game={game} -> {r}");
        if (r != ModInstaller.Result.AccessDenied) return;

        // No UI is allowed here, but Windows' own UAC prompt is: delete exactly the manifest's files
        // with an elevated cmd that doesn't depend on our files (Velopack removes them next).
        // Only allow-listed absolute paths go in, and nothing that cmd would interpret.
        string? args = ElevatedDeleteArgs(game);
        if (args == null)
        {
            Log("uninstall hook: elevated removal refused (nothing to delete, unsafe path or reparse point); the mod files remain in the game folder");
            return;
        }
        try
        {
            Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"), args)
            {
                UseShellExecute = true,
                Verb = "runas",
                WindowStyle = ProcessWindowStyle.Hidden,
            });
            Log("uninstall hook: started elevated removal");
        }
        catch (Win32Exception) { Log("uninstall hook: elevated removal declined; the mod files remain in the game folder"); }
    }

    /// <summary>
    /// Arguments for the elevated "cmd /c del", or null to refuse: no files, a character cmd would
    /// interpret in any path, or a reparse point in the game path.
    /// </summary>
    internal static string? ElevatedDeleteArgs(string game)
    {
        var paths = ModInstaller.InstalledFiles(game);
        if (paths.Count == 0) return null;
        if (paths.Any(p => p.IndexOfAny(['%', '^', '!', '&', '"', '<', '>', '|']) >= 0) || ModInstaller.HasReparsePoint(game)) return null;
        return "/c del /f /q " + string.Join(' ', paths.Select(f => $"\"{f}\""));
    }

    static void Log(string line)
    {
        try { File.AppendAllText(LogPath, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}"); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}
