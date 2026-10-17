// Test harness only. Never switches the input desktop or injects global input.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace NmnIngame {
    public sealed class PrivateDesktop : IDisposable {
        const uint Rights = 0xC7; // read/write objects, enumerate, create window/menu; no SWITCHDESKTOP
        IntPtr handle;
        public string Name { get; private set; }
        public string InputBefore { get; private set; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct Startup {
            public uint cb;
            public string reserved, desktop, title;
            public uint x, y, width, height, charsX, charsY, fill, flags;
            public ushort show, reservedBytes;
            public IntPtr reservedPointer, input, output, error;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInfo { public IntPtr process, thread; public uint pid, tid; }
        [StructLayout(LayoutKind.Sequential)]
        public struct Rect { public int Left, Top, Right, Bottom; }
        delegate bool EnumWindow(IntPtr window, IntPtr argument);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, uint flags, uint access, IntPtr security);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr OpenDesktopW(string name, uint flags, bool inherit, uint access);
        [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("user32.dll", SetLastError = true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll")] static extern IntPtr GetProcessWindowStation();
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool GetUserObjectInformationW(IntPtr obj, int index, StringBuilder value, uint bytes, out uint needed);
        [DllImport("user32.dll", SetLastError = true)] static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindow callback, IntPtr argument);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
        [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
        [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessageW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processSecurity, IntPtr threadSecurity,
            bool inherit, uint flags, IntPtr environment, string directory, ref Startup startup, out ProcessInfo process);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        static Exception Error(string operation) { return new Win32Exception(Marshal.GetLastWin32Error(), operation); }
        static string ObjectName(IntPtr obj) {
            var value = new StringBuilder(512);
            if (obj == IntPtr.Zero || !GetUserObjectInformationW(obj, 2, value, 1024, out _)) throw Error("Read desktop name");
            return value.ToString();
        }
        public static string InputDesktopName() {
            var input = OpenInputDesktop(0, false, 1);
            if (input == IntPtr.Zero) throw Error("Open input desktop for verification");
            try { return ObjectName(input); } finally { CloseDesktop(input); }
        }
        public PrivateDesktop() {
            if (!String.Equals(ObjectName(GetProcessWindowStation()), "WinSta0", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Expected the interactive window station");
            InputBefore = InputDesktopName();
            Name = "NmnTerrain_" + Guid.NewGuid().ToString("N");
            handle = CreateDesktopW(Name, IntPtr.Zero, IntPtr.Zero, 0, Rights, IntPtr.Zero);
            if (handle == IntPtr.Zero) throw Error("Create private desktop");
        }
        public PrivateDesktop(string existingName) {
            if (existingName == null || !System.Text.RegularExpressions.Regex.IsMatch(existingName, "^NmnTerrain_[0-9a-f]{32}$"))
                throw new ArgumentException("Only a named terrain test desktop may be opened");
            Name = existingName;
            InputBefore = InputDesktopName();
            if (Name == InputBefore) throw new InvalidOperationException("Test desktop is currently receiving user input");
            handle = OpenDesktopW(Name, 0, false, Rights);
            if (handle == IntPtr.Zero) throw Error("Open private test desktop");
        }
        // Windows argv quoting, without a command shell or executable-name ambiguity.
        static string Quote(string value) {
            var output = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in value) {
                if (c == '\\') { slashes++; continue; }
                output.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
                output.Append(c); slashes = 0;
            }
            return output.Append('\\', slashes * 2).Append('"').ToString();
        }
        public Process Launch(string executable, string directory, string[] arguments) {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("PrivateDesktop");
            if (Name == InputDesktopName()) throw new InvalidOperationException("Private desktop is receiving input");
            var command = new StringBuilder(Quote(executable));
            foreach (string argument in arguments) command.Append(' ').Append(Quote(argument));
            var startup = new Startup { cb = (uint)Marshal.SizeOf<Startup>(), desktop = "WinSta0\\" + Name,
                flags = 0x81, show = 1 }; // no launch feedback cursor; visible only on private desktop
            if (!CreateProcessW(executable, command, IntPtr.Zero, IntPtr.Zero, false, 0x08000000,
                IntPtr.Zero, directory, ref startup, out var child)) throw Error("Launch on private desktop");
            try {
                var process = Process.GetProcessById((int)child.pid);
                _ = process.Handle; // Retain a real handle so exit code/identity survive a fast child exit.
                return process;
            }
            finally { CloseHandle(child.thread); CloseHandle(child.process); }
        }
        public IntPtr FindWindow(int processId) {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("PrivateDesktop");
            IntPtr found = IntPtr.Zero;
            long largest = 0;
            EnumWindow callback = (window, unused) => {
                GetWindowThreadProcessId(window, out uint pid);
                if (pid == processId && GetClientRect(window, out var rect)) {
                    long area = (long)(rect.Right - rect.Left) * (rect.Bottom - rect.Top);
                    if (area > largest) { found = window; largest = area; }
                }
                return true;
            };
            // An empty desktop can report FALSE with ERROR_SUCCESS before the child creates its window.
            if (!EnumDesktopWindows(handle, callback, IntPtr.Zero) && Marshal.GetLastWin32Error() != 0)
                throw Error("Enumerate private windows");
            return found;
        }
        public string VerifyWindow(IntPtr window, int processId) {
            var thread = GetWindowThreadProcessId(window, out uint pid);
            if (pid != processId || thread == 0) throw new InvalidOperationException("Window ownership changed");
            // Enumerating the actual desktop proves membership even where Windows refuses
            // GetThreadDesktop for a thread in another process.
            bool member = false;
            EnumWindow callback = (candidate, unused) => { if (candidate == window) member = true; return true; };
            if (!EnumDesktopWindows(handle, callback, IntPtr.Zero) && Marshal.GetLastWin32Error() != 0)
                throw Error("Verify private window membership");
            string desktop = ObjectName(handle);
            if (!member || desktop != Name || desktop == InputDesktopName()) throw new InvalidOperationException("Game is not isolated on the test desktop");
            return desktop;
        }
        public void CloseWindow(IntPtr window, int processId) {
            VerifyWindow(window, processId);
            if (!PostMessageW(window, 0x10, IntPtr.Zero, IntPtr.Zero)) throw Error("Close owned window");
        }
        public void Dispose() {
            if (handle != IntPtr.Zero) { CloseDesktop(handle); handle = IntPtr.Zero; }
        }
    }
}
