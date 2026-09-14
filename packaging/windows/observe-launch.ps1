[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable,[Parameter(Mandatory)][string]$LogRoot)
$ErrorActionPreference='Stop'
$LogRoot=[IO.Path]::GetFullPath($LogRoot)
New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ReliquaryWindowCapture {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
 [DllImport("user32.dll")] static extern bool EnumWindows(Callback callback, IntPtr data);
 delegate bool Callback(IntPtr window, IntPtr data);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int count);
 public static string Title(IntPtr window) {var text=new System.Text.StringBuilder(1024);GetWindowText(window,text,text.Capacity);return text.ToString();}
 public static IntPtr LargestWindow(int process) {
  IntPtr found=IntPtr.Zero; long area=0;
  EnumWindows((window,data)=>{uint owner; GetWindowThreadProcessId(window,out owner);Rect rect;
   if(owner==process && IsWindowVisible(window) && GetWindowRect(window,out rect)) {
    long size=(long)(rect.Right-rect.Left)*(rect.Bottom-rect.Top);
    if(size>area && size>200000){area=size;found=window;}
   } return true;
  },IntPtr.Zero); return found;
 }
}
'@
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=(Resolve-Path -LiteralPath $Executable).Path
$info.WorkingDirectory=$LogRoot
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
foreach($key in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','VCPKG_ROOT','QT_QPA_PLATFORM','QTDIR','QML2_IMPORT_PATH','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS')){$info.Environment.Remove($key) | Out-Null}
$info.Environment['PATH']="$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$info.Environment['TEMP']=$LogRoot
$info.Environment['TMP']=$LogRoot
$process=[Diagnostics.Process]::new();$process.StartInfo=$info
[void]$process.Start()
$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
try {
 $previousDpi=[ReliquaryWindowCapture]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
 $deadline=[DateTime]::UtcNow.AddSeconds(15)
 do {Start-Sleep -Milliseconds 250;$process.Refresh();$window=[ReliquaryWindowCapture]::LargestWindow($process.Id)} while(-not $process.HasExited -and $window -eq 0 -and [DateTime]::UtcNow -lt $deadline)
 if($process.HasExited -or $window -eq 0){throw 'Normal launch did not create a main window'}
 Start-Sleep -Seconds 3
 $process.Refresh()
 $rect=[ReliquaryWindowCapture+Rect]::new()
 if(-not [ReliquaryWindowCapture]::GetWindowRect($window,[ref]$rect)){throw 'Cannot inspect application window'}
 $bitmap=[Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
 $graphics=[Drawing.Graphics]::FromImage($bitmap)
 $dc=$graphics.GetHdc()
 try {$captured=[ReliquaryWindowCapture]::PrintWindow($window,$dc,2)} finally {$graphics.ReleaseHdc($dc)}
 if(-not $captured){throw 'Window capture failed'}
 $bitmap.Save("$LogRoot/normal-launch.png",[Drawing.Imaging.ImageFormat]::Png)
 $graphics.Dispose();$bitmap.Dispose()
 "Executable=$($info.FileName)`nArguments=(none)`nWindowTitle=$([ReliquaryWindowCapture]::Title($window))`nWindowHandle=$window`nIdentity=$([Security.Principal.WindowsIdentity]::GetCurrent().Name)`nPATH=$($info.Environment['PATH'])" | Set-Content "$LogRoot/result.txt"
 [void][ReliquaryWindowCapture]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if(-not $process.WaitForExit(15000)){throw 'Application did not close normally'}
 if($process.ExitCode -ne 0){throw 'Normal launch exit code was nonzero'}
} finally {
 [void][ReliquaryWindowCapture]::SetThreadDpiAwarenessContext($previousDpi)
 if(-not $process.HasExited){$process.Kill($true);$process.WaitForExit()}
 $stdout.GetAwaiter().GetResult() | Set-Content "$LogRoot/stdout.txt"
 $stderr.GetAwaiter().GetResult() | Set-Content "$LogRoot/stderr.txt"
 $process.Dispose()
}
