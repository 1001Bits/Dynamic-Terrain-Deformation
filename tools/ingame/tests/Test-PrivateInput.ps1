# Exercises the process-local guard without a game or changing the user's input.
$ErrorActionPreference='Stop'
$kit=Split-Path $PSScriptRoot
$repo=Split-Path (Split-Path $kit)
. "$kit/PrivateDesktop.ps1"
$dll=Join-Path $repo 'build/tools/ingame/driver/NMN_PrivateInputGuard.dll'
if(-not(Test-Path -LiteralPath $dll)){throw 'Build NMN_PrivateInputGuard first'}
$run=Join-Path $kit ('.state/private-input-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$source=@'
using System;
using System.Runtime.InteropServices;
public static class CursorGuardFixture {
 [UnmanagedFunctionPointer(CallingConvention.Winapi)] public delegate int Install();
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left,top,right,bottom; }
 [DllImport("user32.dll")] static extern bool GetClipCursor(out Rect rect);
 [DllImport("user32.dll")] static extern bool ClipCursor(ref Rect rect);
 [DllImport("user32.dll")] static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] static extern bool SetPhysicalCursorPos(int x,int y);
 public static string Clip() { if(!GetClipCursor(out var r)) throw new Exception("GetClipCursor"); return $"{r.left},{r.top},{r.right},{r.bottom}"; }
 public static bool Enable(string path) {
  _ = Clip(); // Load User32 and Win32u before inspecting their process-local exports.
  var library=NativeLibrary.Load(path);
  return Marshal.GetDelegateForFunctionPointer<Install>(NativeLibrary.GetExport(library,"NmnInstallPrivateInputGuard"))()!=0;
 }
 public static void Exercise() {
  foreach(var api in new[]{"ClipCursor","SetCursorPos","SetPhysicalCursorPos","NtUserClipCursor","NtUserSetCursorPos"}) {
   var dll=NativeLibrary.Load(api.StartsWith("Nt") ? "win32u.dll" : "user32.dll");
   var entry=NativeLibrary.GetExport(dll,api);
   if(unchecked((ulong)Marshal.ReadInt64(entry))!=0x9090C300000001B8UL) throw new Exception("Guard absent: "+api);
  }
  // Only call after proving each API returns directly without calling the OS.
  var rect=new Rect{left=100,top=100,right=101,bottom=101};
  for(int i=0;i<10000;i++) {
   if(!ClipCursor(ref rect)||!SetCursorPos(100,100)||!SetPhysicalCursorPos(100,100)) throw new Exception("Guarded call failed");
  }
 }
}
'@
$source | Set-Content -LiteralPath "$run/fixture.cs"
Add-Type -TypeDefinition $source
$before=[CursorGuardFixture]::Clip()
$previous=$env:NMN_TERRAIN_PRIVATE_DESKTOP
try {
 $env:NMN_TERRAIN_PRIVATE_DESKTOP=$null
 if([CursorGuardFixture]::Enable($dll)){throw 'Guard must remain inert without test opt-in'}
 $env:NMN_TERRAIN_PRIVATE_DESKTOP='NmnTerrain_'+[guid]::NewGuid().ToString('N')
 if([CursorGuardFixture]::Enable($dll)){throw 'Guard must refuse the visible/default desktop even with test opt-in'}
} finally { $env:NMN_TERRAIN_PRIVATE_DESKTOP=$previous }
$fixture=@'
param([string]$Run,[string]$Dll)
$ErrorActionPreference='Stop'
Add-Type -Path "$Run/fixture.cs"
$expected=$env:NMN_TERRAIN_PRIVATE_DESKTOP
$env:NMN_TERRAIN_PRIVATE_DESKTOP=$null
if([CursorGuardFixture]::Enable($Dll)){throw 'Private desktop alone must not activate the guard'}
$env:NMN_TERRAIN_PRIVATE_DESKTOP=$expected
if(-not [CursorGuardFixture]::Enable($Dll)){throw 'Private cursor guard did not install'}
[CursorGuardFixture]::Exercise()
@{guardedCalls=30000;allFiveExportsBlocked=$true;clip=[CursorGuardFixture]::Clip()} | ConvertTo-Json | Set-Content -LiteralPath "$Run/result.json"
'@
$fixture | Set-Content -LiteralPath "$run/fixture.ps1"
$desktop=$null
$child=$null
try {
 $desktop=[NmnIngame.PrivateDesktop]::new()
 $env:NMN_TERRAIN_PRIVATE_DESKTOP=$desktop.Name
 $child=$desktop.Launch((Join-Path $PSHOME 'pwsh.exe'),$run,
  [string[]]@('-NoProfile','-File',"$run/fixture.ps1",'-Run',$run,'-Dll',$dll))
 if(-not $child.WaitForExit(20000)){throw 'Private input fixture timed out'}
 if($child.ExitCode -ne 0 -or -not(Test-Path "$run/result.json")){throw "Private input fixture failed ($($child.ExitCode)); $run"}
 $result=Get-Content "$run/result.json" -Raw | ConvertFrom-Json
 if($result.clip -ne $before -or [CursorGuardFixture]::Clip() -ne $before){throw 'Visible cursor bounds changed'}
 if([NmnIngame.PrivateDesktop]::InputDesktopName() -ne $desktop.InputBefore){throw 'Input desktop changed'}
 $result | Add-Member -NotePropertyName visibleBoundsUnchanged -NotePropertyValue $true
 $result | Add-Member -NotePropertyName defaultDesktopRefused -NotePropertyValue $true
 $result | Add-Member -NotePropertyName missingOptInRefused -NotePropertyValue $true
 $result | Add-Member -NotePropertyName dllSha256 -NotePropertyValue (Get-FileHash $dll).Hash
 $result | ConvertTo-Json | Set-Content -LiteralPath "$run/result.json"
 "PASS default desktop refused; five cursor APIs blocked; 30,000 guarded calls; visible cursor bounds/input desktop unchanged. Evidence: $run"
} finally {
 $env:NMN_TERRAIN_PRIVATE_DESKTOP=$previous
 if($child){if(-not $child.HasExited){$child.Kill();[void]$child.WaitForExit(3000)};$child.Dispose()}
 if($desktop){$desktop.Dispose()}
}
