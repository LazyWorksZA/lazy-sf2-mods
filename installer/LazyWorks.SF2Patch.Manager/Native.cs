using System.Runtime.InteropServices;

namespace LazyWorks.SF2Patch.Manager;

/// <summary>
/// The few native Windows dialogs the helper uses, without WinForms: a TaskDialog (comctl32 v6, activated
/// by app.manifest) and the standard folder picker (IFileOpenDialog with FOS_PICKFOLDERS).
/// </summary>
static class Native
{
    public const int ButtonChoose = 1001;
    public const int ButtonClose = 8;  // IDCLOSE

    public enum Icon { Information = -3, Warning = -1, Error = -2 }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct TaskDialogConfig
    {
        public uint cbSize;
        public IntPtr hwndParent, hInstance;
        public uint dwFlags, dwCommonButtons;
        public IntPtr pszWindowTitle, pszMainIcon, pszMainInstruction, pszContent;
        public uint cButtons;
        public IntPtr pButtons;
        public int nDefaultButton;
        public uint cRadioButtons;
        public IntPtr pRadioButtons;
        public int nDefaultRadioButton;
        public IntPtr pszVerificationText, pszExpandedInformation, pszExpandedControlText, pszCollapsedControlText,
            pszFooterIcon, pszFooter, pfCallback, lpCallbackData;
        public uint cxWidth;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct TaskDialogButton { public int id; public IntPtr text; }

    [DllImport("comctl32.dll", ExactSpelling = true)]
    static extern int TaskDialogIndirect(ref TaskDialogConfig config, out int button, IntPtr radio, IntPtr verification);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);

    /// <summary>
    /// Shows a TaskDialog with a heading, a text, an optional "choose" command link and a Close button;
    /// returns the button pressed (<see cref="ButtonChoose"/> or <see cref="ButtonClose"/>). Falls back to a
    /// plain message box (always "close") if TaskDialog is unavailable.
    /// </summary>
    public static int Show(string heading, string text, Icon icon, string? chooseText)
    {
        var strings = new List<IntPtr>();
        IntPtr Str(string s) { IntPtr p = Marshal.StringToHGlobalUni(s); strings.Add(p); return p; }
        IntPtr buttons = IntPtr.Zero;
        try
        {
            var cfg = new TaskDialogConfig
            {
                cbSize = (uint)Marshal.SizeOf<TaskDialogConfig>(),
                dwFlags = 0x8 | (chooseText != null ? 0x10u : 0),  // allow cancel (Esc), command links
                dwCommonButtons = 0x20,                              // Close
                pszWindowTitle = Str("Lazy SF2 Patch"),
                pszMainIcon = (IntPtr)(ushort)(short)(int)icon,
                pszMainInstruction = Str(heading),
                pszContent = Str(text),
            };
            if (chooseText != null)
            {
                buttons = Marshal.AllocHGlobal(Marshal.SizeOf<TaskDialogButton>());
                Marshal.StructureToPtr(new TaskDialogButton { id = ButtonChoose, text = Str(chooseText) }, buttons, false);
                cfg.cButtons = 1;
                cfg.pButtons = buttons;
                cfg.nDefaultButton = ButtonChoose;
            }
            return TaskDialogIndirect(ref cfg, out int pressed, IntPtr.Zero, IntPtr.Zero) == 0 ? pressed : ButtonClose;
        }
        catch (Exception e) when (e is EntryPointNotFoundException or DllNotFoundException or BadImageFormatException)
        {
            MessageBoxW(IntPtr.Zero, heading + "\n\n" + text, "Lazy SF2 Patch", icon == Icon.Information ? 0x40u : 0x30u);
            return ButtonClose;
        }
        finally
        {
            foreach (IntPtr p in strings) Marshal.FreeHGlobal(p);
            if (buttons != IntPtr.Zero) Marshal.FreeHGlobal(buttons);
        }
    }

    public static void Message(string text, bool warning) =>
        MessageBoxW(IntPtr.Zero, text, "Lazy SF2 Patch", warning ? 0x30u : 0x40u);

    /// <summary>The standard folder picker. Null when cancelled or unavailable.</summary>
    public static string? PickFolder(string title)
    {
        IFileOpenDialog? dlg = null;
        try
        {
            dlg = (IFileOpenDialog)new FileOpenDialogCoClass();
            dlg.GetOptions(out uint opts);
            dlg.SetOptions(opts | 0x20 | 0x40 | 0x1000000);  // PICKFOLDERS | FORCEFILESYSTEM | DONTADDTORECENT
            dlg.SetTitle(title);
            if (dlg.Show(IntPtr.Zero) != 0) return null;  // cancelled (HRESULT_FROM_WIN32(ERROR_CANCELLED)) or failed
            dlg.GetResult(out IShellItem item);
            try
            {
                item.GetDisplayName(0x80058000, out IntPtr p);  // SIGDN_FILESYSPATH
                try { return Marshal.PtrToStringUni(p); } finally { Marshal.FreeCoTaskMem(p); }
            }
            finally { Marshal.ReleaseComObject(item); }
        }
        catch (Exception e) when (e is COMException or InvalidCastException) { return null; }
        finally { if (dlg != null) Marshal.ReleaseComObject(dlg); }
    }

    [ComImport, Guid("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7")]
    class FileOpenDialogCoClass { }

    [ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem
    {
        void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
        void GetParent(out IShellItem ppsi);
        void GetDisplayName(uint sigdnName, out IntPtr ppszName);
        void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        void Compare(IShellItem psi, uint hint, out int piOrder);
    }

    // IModalWindow + IFileDialog + IFileOpenDialog, flattened in vtable order. Members we don't call
    // are declared with IntPtr parameters.
    [ComImport, Guid("D57C7288-D4AD-4768-BE02-9D969532D960"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog
    {
        [PreserveSig] int Show(IntPtr hwndOwner);
        void SetFileTypes(uint cFileTypes, IntPtr rgFilterSpec);
        void SetFileTypeIndex(uint iFileType);
        void GetFileTypeIndex(out uint piFileType);
        void Advise(IntPtr pfde, out uint pdwCookie);
        void Unadvise(uint dwCookie);
        void SetOptions(uint fos);
        void GetOptions(out uint pfos);
        void SetDefaultFolder(IShellItem psi);
        void SetFolder(IShellItem psi);
        void GetFolder(out IShellItem ppsi);
        void GetCurrentSelection(out IShellItem ppsi);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
        void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string pszName);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
        void GetResult(out IShellItem ppsi);
        void AddPlace(IShellItem psi, uint fdap);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
        void Close(int hr);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr pFilter);
        void GetResults(out IntPtr ppenum);
        void GetSelectedItems(out IntPtr ppsai);
    }
}
