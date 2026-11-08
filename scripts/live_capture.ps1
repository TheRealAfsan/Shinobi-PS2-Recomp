# Takes a screenshot of the running Shinobi game window (found by its title, not the console window) every N seconds into
# a folder until the game exits.
# usage: live_capture.ps1 -Dir <folder> [-Every 5]
param([string]$Dir, [int]$Every = 5, [string]$Proc = 'ps2EntryRunner', [string]$Title = 'Shinobi')
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class LC {
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x,int y,int cx,int cy,uint f);
 public struct RECT{public int L,T,R,B;}
 public static IntPtr FindGame(uint pid, string prefix) {
   IntPtr found = IntPtr.Zero;
   EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p != pid || !IsWindowVisible(h)) return true;
     var sb = new StringBuilder(256); GetWindowText(h, sb, 256); if (sb.Length > 0 && (prefix.Length == 0 || prefix == "*" || sb.ToString().StartsWith(prefix))) { found = h; return false; } return true; }, IntPtr.Zero);
   return found;
 }
}
"@
New-Item -ItemType Directory -Force $Dir | Out-Null
$n = 0
while ($true) {
    $p = Get-Process $Proc -ErrorAction SilentlyContinue | Select -First 1
    if (-not $p) { if ($n -gt 0) { break } else { Start-Sleep 1; continue } }
    $h = [LC]::FindGame([uint32]$p.Id, $Title)
    if ($h -ne [IntPtr]::Zero) {
        # briefly topmost so nothing covers it, then back to normal
        [void][LC]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0003)
        Start-Sleep -Milliseconds 150
        $r = New-Object LC+RECT; [void][LC]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        if ($w -gt 100 -and $ht -gt 100) {
            $bmp = New-Object System.Drawing.Bitmap $w, $ht
            $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
            $n++; $bmp.Save(("{0}\shot_{1:000}.png" -f $Dir, $n)); $g.Dispose(); $bmp.Dispose()
        }
        [void][LC]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0003)
    }
    Start-Sleep $Every
}
