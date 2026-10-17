<#
.SYNOPSIS
  Unattended in-game smoke test of the plugin on one flat runtime.

.DESCRIPTION
  1. Checks the install is free (no game running, GAME_LOCK.json free) and claims the lock.
  2. Deploys this build with a settings preset (Deploy-Plugin.ps1).
  3. Sets [General] sStartingConsoleCommand=coc <Cell> in Fallout4Custom.ini so the game
     goes straight from start-up into an exterior cell (the file is restored byte for byte).
  4. Launches through F4SE, waits for the world (the log's "Camera gate: ROUTING"), stays
     there -WorldSeconds, takes screenshots, then closes the game.
  5. Collects the plugin log, F4SE logs and any crash log into tools\ingame\.state\runs\...,
     undeploys, restores the INI, releases the lock, and runs Check-PluginLog.ps1.
  Every clean-up step runs in finally blocks, also on Ctrl+C or an error.

  The player stands still, so footprints come from the player's own feet and any NPCs
  nearby; it proves start-up, hooks, shaders, routing and the field update, not visuals.
  The visual checks are manual (docs/TESTING.md).

.EXAMPLE
  .\Invoke-SmokeTest.ps1 -Runtime OG
  .\Invoke-SmokeTest.ps1 -Runtime AE -Preset winter -Cell SanctuaryExt -WorldSeconds 180 -Screenshots 3
#>
param(
	[ValidateSet('OG', 'AE', 'VR', 'NG')][string]$Runtime = 'OG',
	[string]$Preset = 'smoke',
	[string]$Cell = 'SanctuaryExt',
	[int]$WorldSeconds = 120,
	[int]$StartTimeoutSeconds = 300,
	[int]$Screenshots = 2,
	[double]$MinFreeGB = 4.0,  # refuse to start with less free physical memory than this
	[switch]$AllowVR,
	[switch]$SteamVRNull, # private, owned null-headset session with byte-exact settings restoration
	[switch]$DrawDiagnostics,
	[switch]$TestDriver, # opt-in native console mailbox, never included in the release mod
	[switch]$NoVsync,      # performance runs: iPresentInterval=0 in the test's custom INI, so frames are not capped
	[switch]$KeepDeployed, # leave the plugin installed afterwards (the lock is still released)
	[string[]]$Mods = @(),  # staged test mods for this run only (Deploy-TestMods.ps1), e.g. a winter texture mod
	[string]$ModsRoot = '',
	[string[]]$Disable = @()  # other mods taken out for this run only (Deploy-TestMods.ps1 -Disable)
)

. "$PSScriptRoot\IngameCommon.ps1"
. "$PSScriptRoot\PrivateDesktop.ps1"
. "$PSScriptRoot\PrivateSteamVR.ps1"
$r = Get-Runtime $Runtime
if ($SteamVRNull -and -not $r.VR) { throw '-SteamVRNull requires -Runtime VR.' }
if ($r.VR -and -not ($AllowVR -or $SteamVRNull)) {
	throw 'VR needs SteamVR and a headset (or the null driver another agent owns); run it by hand (docs/TESTING.md) or pass -AllowVR.'
}

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$runDir = Join-Path $script:StateDir "runs\$stamp-$Runtime-$Preset"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Write-AtomicJson (Join-Path $runDir 'runner.json') @{
	pid = $PID; startUtcTicks = [Diagnostics.Process]::GetCurrentProcess().StartTime.ToUniversalTime().Ticks
	runtime = $Runtime; runDir = $runDir
}
$transcript = Join-Path $runDir 'smoke.log'
function Say([string]$Text) {
	$line = "[$((Get-Date).ToString('HH:mm:ss'))] $Text"
	Write-Host $line
	Add-Content -LiteralPath $transcript -Value $line
}

function Save-Screenshot([string]$Path) {
	try { Save-PrivateGameWindow -Run $runDir -Path $Path; Say "Screenshot: $Path" }
	catch { Say "Screenshot failed: $($_.Exception.Message)" }
}

$art = Get-BuildArtifacts
Say "Smoke test: $($r.Name), preset '$Preset', cell '$Cell', build $($art.Commit) dll $($art.Sha256.Substring(0, 12))"

$iniBackup = Join-Path $runDir 'Fallout4Custom.ini.original'
$iniExisted = Test-Path -LiteralPath $r.CustomIni
$iniTouched = $false
$deployed = $false
$modsDeployed = $false
# pwsh -File passes '-Mods A,B' as the single string 'A,B'.
$Mods = @($Mods | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Disable = @($Disable | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$locked = $false
$game = $null
$desktop = $null
$steamSession = $null
$launcher = $null
$desktopRecord = $null
$started = Get-Date
$launched = $false
$baseline = $Preset -eq 'perf-off'
$worldPattern = if ($baseline) { 'Exterior world frame: terrain=false clipmap=false' } else { 'Camera gate: ROUTING' }
$outcome = 'not started'

try {
	$freeGB = Get-FreePhysicalMemoryGB
	if ($freeGB -lt $MinFreeGB) {
		$outcome = 'not enough free memory'
		throw ('Only {0:N1} GB of memory free (need {1} GB); the machine is not free for a game run.' -f $freeGB, $MinFreeGB)
	}
	Say ('Free memory: {0:N1} GB' -f $freeGB)

	$lockMinutes = [int][Math]::Ceiling(($StartTimeoutSeconds + $WorldSeconds) / 60) + 15
	$lease = Enter-GameLock $r -Minutes $lockMinutes -Scope "NMN Deformable Terrain smoke test on $($r.Name) (preset $Preset, coc $Cell)"
	$locked = $true
	$runnerOwnershipPath = Join-Path $runDir 'runner.json'
	$runnerOwnership = Get-Content -LiteralPath $runnerOwnershipPath -Raw | ConvertFrom-Json -AsHashtable
	$runnerOwnership['claim_token'] = $lease.claim_token
	Write-AtomicJson $runnerOwnershipPath $runnerOwnership
	Say "Lock claimed until $($lease.expires)"

	# Record ownership before mutation so a partially failed deployment is restored,
	# while a pre-existing deployment belongs to its previous session.
	$deployed = -not (Test-Path -LiteralPath (Join-Path $script:StateDir "deploy-$Runtime.json"))
	& "$PSScriptRoot\Deploy-Plugin.ps1" -Runtime $Runtime -Preset $Preset -TestDriver:$TestDriver -PrivateInputGuard -DrawDiagnostics:$DrawDiagnostics -Session $runDir | ForEach-Object { Say $_ }
	if ($Mods.Count -or $Disable.Count) {
		$modsDeployed = -not (Test-Path -LiteralPath (Join-Path $script:StateDir "test-mods-$Runtime.json"))
		& "$PSScriptRoot\Deploy-TestMods.ps1" -Runtime $Runtime -Mod $Mods -ModsRoot $ModsRoot -Disable $Disable -Session $runDir | ForEach-Object { Say $_ }
	}

	if ($iniExisted) { Copy-Item -LiteralPath $r.CustomIni -Destination $iniBackup -Force }
	$iniTouched = $true
	Set-IniValue $r.CustomIni General sStartingConsoleCommand "coc $Cell"
	Set-IniValue $r.CustomIni General bAlwaysActive 1
	Set-IniValue $r.CustomIni Controls bBackgroundMouse 0
	Set-IniValue $r.CustomIni Display 'bFull Screen' 0
	Set-IniValue $r.CustomIni Display bBorderless 0
	if ($NoVsync) { Set-IniValue $r.CustomIni Display iPresentInterval 0 }
	Copy-Item -LiteralPath $r.CustomIni -Destination (Join-Path $runDir 'Fallout4Custom.ini.test')
	$desktop = [NmnIngame.PrivateDesktop]::new()
	Say "Private desktop: $($desktop.Name); input desktop: $($desktop.InputBefore)"
	if ($SteamVRNull) {
		$steamSession = Start-PrivateSteamVR $desktop $runDir
		Say 'Owned SteamVR null session started from the private desktop.'
	}
	Say "Fallout4Custom.ini: sStartingConsoleCommand=coc $Cell (original saved to the run folder)"

	$started = Get-Date
	$previousDriverDirectory = $env:NMN_TERRAIN_TEST_DIR
	$previousPrivateDesktop = $env:NMN_TERRAIN_PRIVATE_DESKTOP
	$previousGuardFile = $env:NMN_TERRAIN_PRIVATE_GUARD_FILE
	try {
		$env:NMN_TERRAIN_PRIVATE_DESKTOP = $desktop.Name
		$env:NMN_TERRAIN_PRIVATE_GUARD_FILE = Join-Path $runDir 'private-input-guard.json'
		if ($TestDriver) {
			$env:NMN_TERRAIN_TEST_DIR = Join-Path $runDir 'driver'
			New-Item -ItemType Directory -Path $env:NMN_TERRAIN_TEST_DIR | Out-Null
		} else { $env:NMN_TERRAIN_TEST_DIR = $null }
		$launcher = $desktop.Launch((Join-Path $r.Game $r.Loader), $r.Game, [string[]]@())
	} finally {
		$env:NMN_TERRAIN_TEST_DIR = $previousDriverDirectory
		$env:NMN_TERRAIN_PRIVATE_DESKTOP = $previousPrivateDesktop
		$env:NMN_TERRAIN_PRIVATE_GUARD_FILE = $previousGuardFile
	}
	$launched = $true
	Say "Launched $($r.Loader)"

	$deadline = $started.AddSeconds($StartTimeoutSeconds)
	while (-not $game -and (Get-Date) -lt $deadline) {
		Start-Sleep -Seconds 2
		$game = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($r.Exe)) -ErrorAction SilentlyContinue |
			Where-Object { $_.StartTime -ge $started -and $_.Path -eq (Join-Path $r.Game $r.Exe) } | Select-Object -First 1
	}
	if (-not $game) { $outcome = 'game process never appeared'; throw $outcome }
	Say "Game running (PID $($game.Id))"
	$desktopRecord = [ordered]@{
		desktop = $desktop.Name; inputBefore = $desktop.InputBefore; gamePid = $game.Id
		gamePath = $game.Path; gameStartUtcTicks = $game.StartTime.ToUniversalTime().Ticks
		launcherPid = $launcher.Id; windowVerified = $false
	}
	# Persist identity as soon as the process is known, before waiting for its window.
	Write-AtomicJson (Join-Path $runDir 'private-desktop.json') $desktopRecord
	$window = [IntPtr]::Zero
	while ($window -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline -and -not $game.HasExited) {
		$window = $desktop.FindWindow($game.Id)
		if ($window -eq [IntPtr]::Zero) { Start-Sleep -Seconds 1 }
	}
	if ($window -eq [IntPtr]::Zero) { throw 'No game window on the private desktop' }
	$desktopRecord.windowDesktop = $desktop.VerifyWindow($window, $game.Id)
	$desktopRecord.windowVerified = $true
	Write-AtomicJson (Join-Path $runDir 'private-desktop.json') $desktopRecord
	Say "Verified game window on $($desktopRecord.windowDesktop)"
	$guardFile = Join-Path $runDir 'private-input-guard.json'
	if (-not (Test-Path -LiteralPath $guardFile)) { throw 'Private cursor protection is absent; ending owned game.' }
	$guardRecord = Get-Content -LiteralPath $guardFile -Raw | ConvertFrom-Json
	if ($guardRecord.pid -ne $game.Id -or -not $guardRecord.cursorWritesBlocked -or $guardRecord.apiCount -ne 5) {
		throw 'Private cursor protection could not be verified; ending owned game.'
	}
	$desktopRecord.cursorWritesBlocked = $true
	Write-AtomicJson (Join-Path $runDir 'private-desktop.json') $desktopRecord
	Say 'Private cursor guard verified: game cannot clip or reposition the shared mouse.'

	# Wait for the world: the first routed landscape draw logs the camera gate.
	$inWorld = $null
	$nextShot = $null
	$shots = 0
	while ((Get-Date) -lt $deadline -or $inWorld) {
		Start-Sleep -Seconds 5
		Update-GameLock -Minutes $lockMinutes
		if ($steamSession) { Update-PrivateSteamVR $steamSession }
		if ($game.HasExited) { $outcome = "game exited (code $($game.ExitCode)) - crash?"; break }

		if (-not $inWorld -and (Test-Path -LiteralPath $r.PluginLog) -and
			(Get-Item -LiteralPath $r.PluginLog).LastWriteTime -ge $started -and
			(Select-String -LiteralPath $r.PluginLog -Pattern $worldPattern -Quiet)) {
			$inWorld = Get-Date
			$nextShot = $inWorld.AddSeconds([Math]::Min(30, $WorldSeconds / [Math]::Max($Screenshots, 1)))
			Say "In the world: $worldPattern"
		}
		if ($inWorld) {
			if ($shots -lt $Screenshots -and (Get-Date) -ge $nextShot) {
				$shots++
				Save-Screenshot (Join-Path $runDir "screen-$shots.png")
				$nextShot = (Get-Date).AddSeconds($WorldSeconds / [Math]::Max($Screenshots, 1))
			}
			if ((Get-Date) -ge $inWorld.AddSeconds($WorldSeconds)) { $outcome = 'ran the full time'; break }
		}
	}
	if (-not $inWorld -and $outcome -eq 'not started') { $outcome = "no world evidence ($worldPattern) within $StartTimeoutSeconds s" }
	Say "Outcome: $outcome"
} catch {
	Say "ERROR: $($_.Exception.Message)"
	if ($outcome -eq 'not started') { $outcome = "error: $($_.Exception.Message)" }
} finally {
	# Only the process positively identified after our successful launch belongs to us.
	# A lock refusal or missing process must never close another session's game.
	if ($game -and -not $game.HasExited) {
		$p = $game
		if ($outcome -ne 'ran the full time') {
			Save-Screenshot (Join-Path $runDir 'failure-window.png')
		}
		Say "Closing the game (PID $($p.Id))"
		try {
			$window = $desktop.FindWindow($p.Id)
			if ($window -ne [IntPtr]::Zero) { $desktop.CloseWindow($window, $p.Id) }
		} catch { Say "Window close failed: $($_.Exception.Message)" }
		if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force; Say 'Game did not close; killed it' }
	}

	if ($launcher) {
		if (-not $launcher.HasExited) { $launcher.Kill(); [void]$launcher.WaitForExit(3000) }
		$launcher.Dispose()
	}
	if ($steamSession) {
		try {
			Stop-PrivateSteamVR $steamSession $runDir
			Say 'Owned SteamVR session closed and settings restored.'
		} catch {
			$outcome = "SteamVR cleanup failed: $($_.Exception.Message)"
			Say $outcome
		}
	}
	if ($desktop) {
		try {
			if ($desktopRecord) {
				$desktopRecord.inputAfter = [NmnIngame.PrivateDesktop]::InputDesktopName()
				$desktopRecord.gameExited = $null -eq $game -or $game.HasExited
				Write-AtomicJson (Join-Path $runDir 'private-desktop.json') $desktopRecord
			}
		} finally { $desktop.Dispose() }
	}

	# Evidence first, then put the machine back.
	foreach ($file in @($r.PluginLog, (Join-Path $r.LogDir 'f4se.log'), (Join-Path $r.LogDir 'f4se_loader.log'),
			(Join-Path $r.LogDir 'f4sevr.log'), (Join-Path $r.LogDir 'Buffout4.log'))) {
		if ($launched -and (Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).LastWriteTime -ge $started) {
			Copy-Item -LiteralPath $file -Destination $runDir -Force
		}
	}
	$crashes = @(Get-ChildItem -LiteralPath $r.LogDir -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
		Where-Object { $launched -and $_.LastWriteTime -ge $started })
	$crashes += @(Get-ChildItem -LiteralPath (Split-Path $r.LogDir) -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
		Where-Object { $launched -and $_.LastWriteTime -ge $started })
	foreach ($crash in $crashes) { Copy-Item -LiteralPath $crash.FullName -Destination $runDir -Force; Say "CRASH LOG: $($crash.Name)" }
	if ($deployed) { Copy-Item -LiteralPath (Join-Path $script:StateDir "deploy-$Runtime.json") -Destination $runDir -ErrorAction SilentlyContinue }

	if ($iniTouched -and $iniExisted -and (Test-Path -LiteralPath $iniBackup)) {
		Copy-Item -LiteralPath $iniBackup -Destination $r.CustomIni -Force
		Say 'Fallout4Custom.ini restored'
	} elseif ($iniTouched -and -not $iniExisted -and (Test-Path -LiteralPath $r.CustomIni)) {
		Remove-Item -LiteralPath $r.CustomIni -Force
		Say 'Fallout4Custom.ini removed (it did not exist before)'
	}
	if ($modsDeployed) {
		try { & "$PSScriptRoot\Deploy-TestMods.ps1" -Runtime $Runtime -Remove -Session $runDir | ForEach-Object { Say $_ } }
		catch { $outcome = "test-mod removal failed: $($_.Exception.Message)"; Say "ERROR: $outcome" }
	}
	if ($deployed -and -not $KeepDeployed) {
		# A failed removal is reported, but the locks are still released below.
		try { & "$PSScriptRoot\Deploy-Plugin.ps1" -Runtime $Runtime -Remove -RestoreEditedIni -Session $runDir | ForEach-Object { Say $_ } }
		catch { $outcome = "undeploy failed: $($_.Exception.Message)"; Say "ERROR: $outcome" }
	}
	if ($locked) { Exit-GameLock; Say 'Lock released' }
}

$logCopy = Join-Path $runDir "$script:PluginName.log"
$checkExit = 1
if (Test-Path -LiteralPath $logCopy) {
	& "$PSScriptRoot\Check-PluginLog.ps1" -Runtime $Runtime -Log $logCopy -Baseline:$baseline -Json (Join-Path $runDir 'check.json') |
		ForEach-Object { Say $_ }
	$checkExit = $LASTEXITCODE
} else {
	Say "No fresh plugin log collected (outcome: $outcome)."
}

[ordered]@{
	runtime = $Runtime; preset = $Preset; cell = $Cell; commit = $art.Commit; dllSha256 = $art.Sha256
	privateDesktop = if ($desktopRecord) { $desktopRecord.desktop } else { $null }
	outcome = $outcome; logCheckPass = $checkExit -eq 0; runDir = $runDir; finished = (Get-Date).ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding utf8NoBOM

Say "Run folder: $runDir"
exit $(if ($checkExit -eq 0 -and $outcome -eq 'ran the full time') { 0 } else { 1 })
