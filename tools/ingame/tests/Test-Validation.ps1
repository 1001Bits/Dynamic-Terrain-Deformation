# Regression checks for validation itself: a disabled baseline, dropped stamps, and
# smoke-test failure cleanup. Uses generated log fixtures and a fake process only.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$sandbox = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-validation-' + [guid]::NewGuid().ToString('N'))
$previousSandbox = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $sandbox
$fails = 0
function Check([bool]$Ok, [string]$What) {
	if ($Ok) { "PASS $What" } else { "FAIL $What"; $script:fails++ }
}

try {
	New-Item -ItemType Directory -Force -Path $sandbox | Out-Null
	$startup = @'
[INFO] NMN_DeformableTerrain v0.1.0 (test) is loading
[INFO] Detected runtime: Fallout 4 1.10.163
[INFO] ShaderRegistry: watching device creation (D3D11CreateDeviceAndSwapChain)
[INFO] NMN_DeformableTerrain loaded
[INFO] D3D ready: device=0x123 context=0x456
[INFO] ShaderRegistry: 100 vertex shader(s) recorded
[INFO] Hooked BSDFPrePassShader::SetupGeometry/RestoreGeometry
[INFO] Hooked DrawWorld::DeferredPrePass
[INFO] Hooked DrawWorld::Forward
[INFO] Hooked BSGraphics::Renderer::RegisterForZPrePass
[INFO] Impacts: explosions hooked, bullets hooked, lasers hooked, arrows hooked
'@
	$world = @'
[INFO] Clipmap ready: 2 level(s)
[INFO] Routed vertex shader 0x123 landscape layout recognised
[INFO] Terrain shader pair: compiler 10.0 ms, ready
[INFO] Camera gate: ROUTING
[INFO] Depth pre-pass: near terrain is drawn by the G-buffer pass only
'@
	$baseline = @'
[INFO] Exterior world frame: terrain=false clipmap=false
[INFO] GPU frame span    avg  16.667 ms
'@
	$log = Join-Path $sandbox 'fixture.log'
	$json = Join-Path $sandbox 'check.json'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $world)
	& "$kit\Check-PluginLog.ps1" -Log $log -Json $json | Out-Null
	Check ($LASTEXITCODE -eq 0) 'enabled world fixture passes'
	Set-Content -LiteralPath $log -Value (($startup -replace 'Impacts: explosions hooked, bullets hooked, lasers hooked, arrows hooked', 'Impacts: explosions=true') + "`n" + $world)
	& "$kit\Check-PluginLog.ps1" -Log $log | Out-Null
	Check ($LASTEXITCODE -eq 1) 'impact settings summary cannot substitute for installed hooks'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $world)
	Add-Content -LiteralPath $log -Value '[INFO] Shader builds: reload dropped the cache'
	& "$kit\Check-PluginLog.ps1" -Log $log | Out-Null
	Check ($LASTEXITCODE -eq 0) 'expected shader cache reset is not a dropped stamp'
	Add-Content -LiteralPath $log -Value '[WARN] Stamps: 64 pressed, 4 DROPPED - the frame wanted more'
	& "$kit\Check-PluginLog.ps1" -Log $log | Out-Null
	Check ($LASTEXITCODE -eq 1) 'dropped stamps fail validation'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $baseline)
	& "$kit\Check-PluginLog.ps1" -Log $log -Baseline -Json $json | Out-Null
	Check ($LASTEXITCODE -eq 0) 'disabled baseline passes without shaders or clipmap'
	& "$kit\Check-PluginLog.ps1" -Log $log | Out-Null
	Check ($LASTEXITCODE -eq 1) 'enabled validation rejects disabled baseline'
	Set-Content -LiteralPath $log -Value $startup
	& "$kit\Check-PluginLog.ps1" -Log $log -Baseline | Out-Null
	Check ($LASTEXITCODE -eq 1) 'baseline requires exterior evidence and profiler'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $baseline + "`n" + $world)
	& "$kit\Check-PluginLog.ps1" -Log $log -Baseline | Out-Null
	Check ($LASTEXITCODE -eq 1) 'baseline rejects routed terrain'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $baseline + "`n[INFO] Clipmap dispatch  avg  0.123 ms")
	& "$kit\Check-PluginLog.ps1" -Log $log -Baseline | Out-Null
	Check ($LASTEXITCODE -eq 1) 'baseline rejects field dispatch'
	Set-Content -LiteralPath $log -Value ($startup + "`n" + $world + "`n[ERROR] test failure")
	& "$kit\Check-PluginLog.ps1" -Log $log | Out-Null
	Check ($LASTEXITCODE -eq 1) 'error line fails validation'

	# A pre-existing game blocks the run. Mock it so even broken cleanup cannot
	# terminate anything real; the script methods record any attempt to close it.
	$sentinel = [pscustomobject]@{ Id = 999999; Name = 'Fallout4'; CloseCalls = 0; HasExited = $false }
	$sentinel | Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value { $this.CloseCalls++; return $false }
	$sentinel | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($Timeout) return $true }
	function Get-Process { param($Name, $Id, $ErrorAction) return $sentinel }
	$custom = Join-Path $sandbox 'Documents\My Games\Fallout4\Fallout4Custom.ini'
	$oldLog = Join-Path $sandbox 'Documents\My Games\Fallout4\F4SE\NMN_DeformableTerrain.log'
	New-Item -ItemType Directory -Force -Path (Split-Path $oldLog) | Out-Null
	Set-Content -LiteralPath $custom -Value '[General] untouched' -NoNewline
	Set-Content -LiteralPath $oldLog -Value ($startup + "`n" + $world)
	$before = (Get-FileHash -LiteralPath $custom).Hash
	& "$kit\Invoke-SmokeTest.ps1" -Runtime OG -MinFreeGB 0 -WorldSeconds 0 -Screenshots 0 | Out-Null
	Check ($LASTEXITCODE -eq 1) 'busy game refuses smoke test'
	Check ($sentinel.CloseCalls -eq 0) 'lock refusal never closes a pre-existing game'
	Check ((Get-FileHash -LiteralPath $custom).Hash -eq $before) 'lock refusal preserves custom INI'
	$runs = @(Get-ChildItem -LiteralPath (Join-Path $sandbox '.state\runs') -Directory)
	Check ($runs.Count -eq 1 -and -not (Test-Path (Join-Path $runs[0].FullName 'NMN_DeformableTerrain.log'))) 'failed launch does not reuse a stale plugin log'
	Check (-not (Test-Path (Join-Path $sandbox 'GAME_LOCK.json'))) 'busy refusal creates no game lock'
} finally {
	$env:NMN_INGAME_SANDBOX = $previousSandbox
	# The unique target is explicitly under TEMP; verify it before recursive removal.
	$resolved = [IO.Path]::GetFullPath($sandbox)
	$allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected sandbox cleanup path' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
"`n$(if ($fails) { "$fails FAILED" } else { 'ALL VALIDATION TESTS PASS' })"
exit $(if ($fails) { 1 } else { 0 })
