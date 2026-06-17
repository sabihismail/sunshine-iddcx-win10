// SetVoidTopology.cs - Attach void-driver IddCx displays to the desktop.
//
// Win10 22H2 (IddCx 1.0) does not auto-attach IddCx targets to the desktop
// even though the displays are in PnP with status OK. This tool calls
// ChangeDisplaySettingsEx for each void display to attach it at a position
// and set one of them as primary. Runs in the user session (the per-session
// ChangeDisplaySettings API only affects the calling session's desktop).
//
// Usage:
//   SetVoidTopology.exe <device_spec>...
//   where <device_spec> is DEVICENAME:WxH@Hz:X,Y[:P]   (P=1 makes it primary)
//
// Examples:
//   SetVoidTopology.exe --list
//   SetVoidTopology.exe "\\.\DISPLAY3:1920x1080@144:0,0:1" "\\.\DISPLAY4:1920x1080@60:1920,0"
//   SetVoidTopology.exe "\\.\DISPLAY3:1920x1080@144:0,0:1"  # single, primary
//
// --list prints discovered displays and exits 0.
// Use EnumDisplayDevicesW to map "DISPLAYx" -> void-display by PnP InstanceId.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

public static class SetVoidTopology
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DEVMODE
    {
        private const int CCHDEVICENAME = 32;
        private const int CCHFORMNAME = 32;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHDEVICENAME)] public string dmDeviceName;
        public ushort dmSpecVersion;
        public ushort dmDriverVersion;
        public ushort dmSize;
        public ushort dmDriverExtra;
        public uint dmFields;
        public int dmPositionX;
        public int dmPositionY;
        public uint dmDisplayOrientation;
        public uint dmDisplayFixedOutput;
        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = CCHFORMNAME)] public string dmFormName;
        public ushort dmLogPixels;
        public uint dmBitsPerPel;
        public uint dmPelsWidth;
        public uint dmPelsHeight;
        public uint dmDisplayFlags;
        public uint dmDisplayFrequency;
        public uint dmICMMethod;
        public uint dmICMIntent;
        public uint dmMediaType;
        public uint dmDitherType;
        public uint dmReserved1;
        public uint dmReserved2;
        public uint dmPanningWidth;
        public uint dmPanningHeight;
    }

    private const uint DM_POSITION = 0x00000020;
    private const uint DM_PELSWIDTH = 0x00080000;
    private const uint DM_PELSHEIGHT = 0x00100000;
    private const uint DM_DISPLAYFREQUENCY = 0x00400000;
    private const uint DM_DISPLAYFLAGS = 0x00000080;

    private const uint DISPLAY_DEVICE_ACTIVE = 0x00000001;
    private const uint DISPLAY_DEVICE_ATTACHED = 0x00000002;
    private const uint DISPLAY_DEVICE_PRIMARY_DEVICE = 0x00000004;

    private const uint CDS_UPDATEREGISTRY = 0x00000001;
    private const uint CDS_NORESET = 0x00000004;
    private const uint CDS_SET_PRIMARY = 0x00000010;
    private const uint CDS_RESET = 0x40000000;

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int ChangeDisplaySettingsEx(string lpszDeviceName, ref DEVMODE lpDevMode, IntPtr hwnd, uint dwflags, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int ChangeDisplaySettingsEx(string lpszDeviceName, IntPtr lpDevMode, IntPtr hwnd, uint dwflags, IntPtr lParam);

    public static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            PrintUsage();
            return 1;
        }

        if (args[0] == "--list" || args[0] == "-l")
        {
            var list = EnumerateDisplays();
            Console.WriteLine("Discovered displays (StateFlags: 1=ACTIVE 2=ATTACHED 4=PRIMARY):");
            foreach (var d in list)
            {
                string tag = "";
                if ((d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) tag += " PRIMARY";
                if ((d.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) tag += " (inactive)";
                Console.WriteLine($"  {d.DeviceName}  state=0x{d.StateFlags:X}{tag}  string='{d.DeviceString}'  id={d.DeviceID}");
            }
            return 0;
        }

        var targets = new List<(string Device, uint W, uint H, uint F, int X, int Y, bool P)>();
        foreach (var arg in args)
        {
            if (!TryParseSpec(arg, out var t))
            {
                Console.Error.WriteLine($"Bad spec: {arg} (expected DEVICENAME:WxH@Hz:X,Y[:P])");
                return 2;
            }
            targets.Add(t);
        }

        Console.WriteLine($"Targets:");
        foreach (var t in targets)
        {
            Console.WriteLine($"  {t.Device} {t.W}x{t.H}@{t.F} at {t.X},{t.Y}{(t.P ? " PRIMARY" : "")}");
        }

        var present = EnumerateDisplays();
        var presentNames = new HashSet<string>();
        foreach (var d in present) presentNames.Add(d.DeviceName);
        foreach (var t in targets)
        {
            if (!presentNames.Contains(t.Device))
            {
                Console.Error.WriteLine($"WARNING: {t.Device} not found in EnumDisplayDevices.");
            }
        }

        for (int i = 0; i < targets.Count; i++)
        {
            var t = targets[i];
            var dm = new DEVMODE();
            dm.dmSize = (ushort)Marshal.SizeOf<DEVMODE>();
            dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
            dm.dmPositionX = t.X;
            dm.dmPositionY = t.Y;
            dm.dmPelsWidth = t.W;
            dm.dmPelsHeight = t.H;
            dm.dmDisplayFrequency = t.F;
            dm.dmDisplayFlags = 0;
            uint flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (t.P) flags |= CDS_SET_PRIMARY;
            int rc = ChangeDisplaySettingsEx(t.Device, ref dm, IntPtr.Zero, flags, IntPtr.Zero);
            Console.WriteLine($"  {t.Device} -> {t.W}x{t.H}@{t.F} at {t.X},{t.Y}{(t.P ? " PRIMARY" : "")} (rc={rc})");
            if (rc != 0)
            {
                Console.Error.WriteLine($"    err: 0x{Marshal.GetLastWin32Error():X}");
            }
        }

        int resetRc = ChangeDisplaySettingsEx(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
        Console.WriteLine($"\nFinal ChangeDisplaySettingsEx (apply) rc={resetRc}");
        return resetRc == 0 ? 0 : 3;
    }

    private static bool TryParseSpec(string s, out (string Device, uint W, uint H, uint F, int X, int Y, bool P) t)
    {
        t = default;
        // \\.\DISPLAYx:WxH@Hz:X,Y[:P]
        // Note: .NET regex indexes unnamed groups BEFORE named groups, so use
        // named groups for everything to avoid getting h back when asking for X.
        var m = Regex.Match(s, @"^(?<dev>\\\\\.\\DISPLAY\d+):(?<w>\d+)x(?<h>\d+)@(?<f>\d+):(?<x>-?\d+),(?<y>-?\d+)(?::(?<p>[01]))?$");
        if (!m.Success) return false;
        t.Device = m.Groups["dev"].Value;
        t.W = uint.Parse(m.Groups["w"].Value);
        t.H = uint.Parse(m.Groups["h"].Value);
        t.F = uint.Parse(m.Groups["f"].Value);
        t.X = int.Parse(m.Groups["x"].Value);
        t.Y = int.Parse(m.Groups["y"].Value);
        t.P = m.Groups["p"].Success && m.Groups["p"].Value == "1";
        return true;
    }

    private static List<DISPLAY_DEVICE> EnumerateDisplays()
    {
        var list = new List<DISPLAY_DEVICE>();
        for (uint i = 0; ; i++)
        {
            var d = new DISPLAY_DEVICE { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };
            if (!EnumDisplayDevices(null, i, ref d, 0)) break;
            list.Add(d);
        }
        return list;
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine("Usage:");
        Console.Error.WriteLine("  SetVoidTopology.exe --list");
        Console.Error.WriteLine("  SetVoidTopology.exe <DEVICENAME:WxH@Hz:X,Y[:P]>...");
        Console.Error.WriteLine("");
        Console.Error.WriteLine("Examples:");
        Console.Error.WriteLine(@"  SetVoidTopology.exe \\.\DISPLAY3:1920x1080@144:0,0:1");
        Console.Error.WriteLine(@"  SetVoidTopology.exe ""\\.\DISPLAY3:1920x1080@144:0,0:1"" ""\\.\DISPLAY4:1920x1080@60:1920,0""");
    }
}
