namespace LazySF2.Manager;

/// <summary>Minimal status window: game folder, mod status, Install/Repair, Remove, mouse mode.</summary>
public sealed class MainForm : Form
{
    readonly Label _game = new() { AutoSize = true };
    readonly Label _status = new() { AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont!, FontStyle.Bold) };
    readonly Button _install = new() { AutoSize = true };
    readonly Button _remove = new() { Text = "Remove from game", AutoSize = true };
    readonly ComboBox _mouse = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 320 };
    readonly Label _mouseHelp = new() { AutoSize = true, MaximumSize = new Size(380, 0), ForeColor = SystemColors.GrayText };
    readonly string? _gameDir = ModInstaller.FindGameDir();
    bool _loading;

    public MainForm()
    {
        Text = "LazySF2";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        StartPosition = FormStartPosition.CenterScreen;
        Padding = new Padding(12);

        // Index 0 = Cursor mode, 1 = Stock (lazysf2.ini [Mouse] Mode=Cursor / Stock).
        _mouse.Items.AddRange(["Match the Windows cursor (recommended)", "Stock: the game moves its own cursor"]);
        _install.Click += (_, _) => Act(() => ModInstaller.Install(_gameDir), "--elevated-install");
        _remove.Click += (_, _) => Act(() => ModInstaller.Uninstall(_gameDir), "--elevated-uninstall");
        _mouse.SelectedIndexChanged += (_, _) => SetMouse();

        var grid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Fill };
        grid.Controls.Add(new Label { Text = "Game folder:", AutoSize = true }, 0, 0);
        grid.Controls.Add(_game, 1, 0);
        grid.Controls.Add(new Label { Text = "Mod:", AutoSize = true }, 0, 1);
        grid.Controls.Add(_status, 1, 1);
        grid.Controls.Add(new Label { Text = "Mouse pointer:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
        grid.Controls.Add(_mouse, 1, 2);
        grid.Controls.Add(_mouseHelp, 1, 3);
        var buttons = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight };
        buttons.Controls.AddRange([_install, _remove]);
        grid.Controls.Add(buttons, 1, 4);
        grid.Controls.Add(new Label
        {
            Text = "Display mode and screen are chosen in the game: Options > Graphics.",
            AutoSize = true, ForeColor = SystemColors.GrayText,
        }, 1, 5);
        Controls.Add(grid);

        Shown += (_, _) =>
        {
            Activate();  // Setup opens us after installing; don't stay behind its window
            FinishPendingInstall();
        };
        Refresh_();
    }

    void Refresh_()
    {
        _loading = true;
        var state = ModInstaller.GetState(_gameDir);
        _game.Text = _gameDir ?? "Strike Fighters 2 not found (no installer registry entry)";
        _status.Text = state switch
        {
            ModInstaller.State.Installed => "Installed",
            ModInstaller.State.Outdated => "Installed, older version: Repair to update",
            ModInstaller.State.NotInstalled => "Not installed",
            ModInstaller.State.Foreign => "Another winmm.dll is in the game folder: not touching it",
            _ => "Game not found",
        };
        _install.Text = state == ModInstaller.State.NotInstalled ? "Install into game" : "Repair";
        _install.Enabled = state is ModInstaller.State.NotInstalled or ModInstaller.State.Installed or ModInstaller.State.Outdated;
        _remove.Enabled = state is ModInstaller.State.Installed or ModInstaller.State.Outdated;
        _mouse.Enabled = _remove.Enabled;
        bool cursor = _gameDir != null && ModInstaller.GetMouseMode(_gameDir) == "Cursor";
        _mouse.SelectedIndex = cursor ? 0 : 1;
        _mouseHelp.Text = cursor
            ? "The game's pointer sits exactly where Windows' pointer is, with Windows' speed and acceleration."
            : "The game builds its pointer from raw mouse movement with its own speed, so it can drift from where " +
              "Windows' pointer is and feels off; over remote desktop it wanders by itself.";
        _loading = false;
    }

    /// <summary>Tries an action directly; if the game folder needs admin rights, repeats it elevated.</summary>
    void Act(Func<ModInstaller.Result> direct, params string[] elevatedArgs)
    {
        var r = direct();
        if (r == ModInstaller.Result.AccessDenied)
        {
            r = Program.RunElevated(elevatedArgs) ?? ModInstaller.Result.AccessDenied;
        }
        if (r == ModInstaller.Result.Failed)
        {
            MessageBox.Show(this, "Couldn't change the game folder. Is Strike Fighters 2 running? Close it and try again.",
                Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        else if (r == ModInstaller.Result.AccessDenied)
        {
            MessageBox.Show(this, "Administrator rights are needed to change the game folder.", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        Refresh_();
    }

    void SetMouse()
    {
        if (_loading || _gameDir == null) return;
        string mode = _mouse.SelectedIndex == 0 ? "Cursor" : "Stock";
        Act(() => ModInstaller.SetMouseMode(_gameDir, mode), "--elevated-mouse", mode);
    }

    /// <summary>The install hook couldn't write the game folder (needs admin): finish it now.</summary>
    void FinishPendingInstall()
    {
        if (!File.Exists(Program.PendingInstallMarker)) return;
        File.Delete(Program.PendingInstallMarker);
        if (ModInstaller.GetState(_gameDir) != ModInstaller.State.NotInstalled) return;
        MessageBox.Show(this, "To finish installing, LazySF2 needs administrator rights to copy the mod into the game folder.",
            Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
        Act(() => ModInstaller.Install(_gameDir), "--elevated-install");
    }
}
