<#
.SYNOPSIS
  Interactive session for a person playing: claims the install, deploys this build, launches
  the game on the visible desktop and, once the game exits, puts everything back.

.DESCRIPTION
  No private desktop, cursor guard or command driver: the game behaves as in normal play.
  Only sStartingConsoleCommand (with -Cell) and iPresentInterval=0 (with -NoVsync, for timing)
  are added to Fallout4Custom.ini, and the file is
  restored byte for byte afterwards. -Mods puts staged test mods (.state\mods\<Name>, see
  Deploy-TestMods.ps1) into Data and Plugins.txt for the session only. -WaitForGo installs
  everything and holds the reservation (renewed every minute) without launching, until
  .state\manual-go-<Runtime> appears (Send-ManualGo.ps1); .state\manual-cancel-<Runtime>
  or -MaxWaitMinutes without a go puts everything back unlaunched. -WaitForFreeMinutes waits
  for another project's reservation to end instead of failing at once. The reservation is renewed every minute while the game
  runs, so another project cannot start a game mid-session. Run it detached
  (Win32_Process.Create) so closing the controlling session does not skip the cleanup.
  A session that is killed outright still skips it; its record (.state\manual-session-
  <Runtime>.json) stays behind, and the next session finishes that cleanup as soon as it
  holds the reservation, before installing anything itself.

.EXAMPLE
  .\Start-ManualSession.ps1 -Runtime AE -Cell SanctuaryExt
  .\Start-ManualSession.ps1 -Runtime AE -Cell SanctuaryExt -Preset winter-mod -Mods WinterOverhaul
#>
param(
	[ValidateSet('OG', 'AE')][string]$Runtime = 'AE',
	[string]$Preset = 'manual',
	[string]$Cell = '',
	[string[]]$Mods = @(),
	[string]$ModsRoot = '',
	[string[]]$Disable = @(),
	[switch]$WaitForGo,
	[switch]$NoVsync,
	[int]$WaitForFreeMinutes = 0,
	[int]$MaxWaitMinutes = 240,
	[int]$StartTimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\IngameCommon.ps1"
# pwsh -File passes "-Mods A,B" as the single string 'A,B'.
$Mods = @($Mods | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Disable = @($Disable | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$r = Get-Runtime $Runtime
$runDir = Join-Path $script:StateDir ("runs\{0}-{1}-manual" -f (Get-Date).ToString('yyyyMMdd-HHmmss'), $Runtime)
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$log = Join-Path $runDir 'session.log'
function Say([string]$Text) { Add-Content -LiteralPath $log -Value "[$((Get-Date).ToString('HH:mm:ss'))] $Text" }

$iniBackup = Join-Path $runDir 'Fallout4Custom.ini.original'
$waiting = Join-Path $script:StateDir "manual-waiting-$Runtime"
$record = Join-Path $script:StateDir "manual-session-$Runtime.json"
$iniExisted = Test-Path -LiteralPath $r.CustomIni
$iniTouched = $false
$deployed = $false
$modsDeployed = $false
$locked = $false
$started = Get-Date
$processStart = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks
$renewedAt = Get-Date
$game = $null
$recorded = $false

# What this session has changed so far, rewritten after each step: all a later session needs
# to put the install back if this one is killed before its own cleanup runs.
function Save-Record {
	$script:recorded = $true
	Write-AtomicJson $record ([ordered]@{
		pid = $PID; processStart = $processStart; runDir = $runDir; iniExisted = $iniExisted
		iniTouched = $iniTouched; deployed = $deployed; modsDeployed = $modsDeployed
	})
}

function Restore-InterruptedSession {
	$old = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json -AsHashtable
	$alive = Get-Process -Id ([int]$old['pid']) -ErrorAction SilentlyContinue
	if ($alive -and $alive.StartTime.ToUniversalTime().Ticks -eq [long]$old['processStart']) {
		throw "Another manual session (PID $($old['pid'])) is still running."
	}
	foreach ($manifestName in @("deploy-$Runtime.json", "test-mods-$Runtime.json")) {
		$manifestPath = Join-Path $script:StateDir $manifestName
		if (Test-Path -LiteralPath $manifestPath) {
			$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
			if ($manifest['session'] -ne $old['runDir']) { throw 'Interrupted manual session no longer owns the deployed files.' }
		}
	}
	$oldLog = Join-Path $old['runDir'] 'session.log'
	function Both([string]$Text) {
		Say $Text
		if (Test-Path -LiteralPath $old['runDir']) {
			Add-Content -LiteralPath $oldLog -Value "[$((Get-Date).ToString('HH:mm:ss'))] (cleanup by $runDir) $Text"
		}
	}
	Both "Finishing the cleanup of an interrupted session ($($old['runDir']), PID $($old['pid']) gone)"
	if ($old['iniTouched']) {
		# Put the file back only if the starting command is still the sole change; anything
		# else means someone edited it since, and it is left alone.
		$ours = '(?i)^\s*(sStartingConsoleCommand|iPresentInterval)\s*='
		$current = if (Test-Path -LiteralPath $r.CustomIni) { @(Get-Content -LiteralPath $r.CustomIni) } else { @() }
		$rest = @($current | Where-Object { $_ -notmatch $ours })
		$oldBackup = Join-Path $old['runDir'] 'Fallout4Custom.ini.original'
		if ($old['iniExisted'] -and (Test-Path -LiteralPath $oldBackup)) {
			$before = @(Get-Content -LiteralPath $oldBackup | Where-Object { $_ -notmatch $ours })
			if (($rest -join "`n") -eq ($before -join "`n")) {
				Copy-Item -LiteralPath $oldBackup -Destination $r.CustomIni -Force; Both 'Fallout4Custom.ini restored'
			} else { Both 'WARNING: Fallout4Custom.ini was edited since; left as it is' }
		} elseif (-not $old['iniExisted']) {
			if (-not ($rest | Where-Object { $_ -notmatch '(?i)^\s*\[(General|Display)\]\s*$' -and $_.Trim() })) {
				Remove-Item -LiteralPath $r.CustomIni -Force -ErrorAction SilentlyContinue
				Both 'Fallout4Custom.ini removed (it did not exist before)'
			} else { Both 'WARNING: Fallout4Custom.ini was edited since; left as it is' }
		}
	}
	if ($old['modsDeployed']) { & "$PSScriptRoot\Deploy-TestMods.ps1" -Runtime $Runtime -Remove -Session $old['runDir'] | ForEach-Object { Both $_ } }
	if ($old['deployed']) { & "$PSScriptRoot\Deploy-Plugin.ps1" -Runtime $Runtime -Remove -RestoreEditedIni -Session $old['runDir'] | ForEach-Object { Both $_ } }
	Remove-Item -LiteralPath $waiting, $record -Force -ErrorAction SilentlyContinue
}

try {
	# With -WaitForFreeMinutes, another project's reservation is waited out, never touched.
	$lease = $null
	$freeBy = (Get-Date).AddMinutes($WaitForFreeMinutes)
	$noted = $false
	while (-not $lease) {
		try { $lease = Enter-GameLock $r -Minutes 30 -Scope "Interactive visual test of NMN Deformable Terrain on $($r.Name)" }
		catch {
			if ($WaitForFreeMinutes -le 0 -or (Get-Date) -gt $freeBy -or $_.Exception.Message -notmatch 'is busy') { throw }
			if (-not $noted) { Say "Waiting up to $WaitForFreeMinutes minutes for the install: $($_.Exception.Message)"; $noted = $true }
			Start-Sleep -Seconds 30
		}
	}
	$locked = $true
	Say "Lock claimed until $($lease.expires)"
	if (Test-Path -LiteralPath $record) {
		Restore-InterruptedSession
		# That cleanup may have restored or removed the INI.
		$iniExisted = Test-Path -LiteralPath $r.CustomIni
	}
	Save-Record
	# Marked before the deploy when nothing of this kit was there yet, so a kill midway is
	# still undone; a deploy already there makes Deploy-Plugin refuse and stays untouched.
	$deployed = -not (Test-Path -LiteralPath (Join-Path $script:StateDir "deploy-$Runtime.json"))
	Save-Record
	& "$PSScriptRoot\Deploy-Plugin.ps1" -Runtime $Runtime -Preset $Preset -Session $runDir | ForEach-Object { Say $_ }
	if ($Mods.Count -or $Disable.Count) {
		$modsDeployed = -not (Test-Path -LiteralPath (Join-Path $script:StateDir "test-mods-$Runtime.json"))
		Save-Record
		& "$PSScriptRoot\Deploy-TestMods.ps1" -Runtime $Runtime -Mod $Mods -ModsRoot $ModsRoot -Disable $Disable -Session $runDir | ForEach-Object { Say $_ }
	}

	if ($Cell -or $NoVsync) {
		if ($iniExisted) { Copy-Item -LiteralPath $r.CustomIni -Destination $iniBackup -Force }
		$iniTouched = $true
		Save-Record
	}
	if ($NoVsync) {
		Set-IniValue $r.CustomIni Display iPresentInterval 0
		Say 'Fallout4Custom.ini: iPresentInterval=0, vsync off for timing (original saved)'
	}
	if ($Cell) {
		$lines = if (Test-Path -LiteralPath $r.CustomIni) { @(Get-Content -LiteralPath $r.CustomIni) } else { @() }
		$lines = @($lines | Where-Object { $_ -notmatch '(?i)^\s*sStartingConsoleCommand\s*=' })
		$general = [Array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match '(?i)^\s*\[General\]\s*$' })
		$entry = "sStartingConsoleCommand=coc $Cell"
		$lines = if ($general -ge 0) {
			@($lines[0..$general]) + $entry + @(if ($general + 1 -lt $lines.Count) { $lines[($general + 1)..($lines.Count - 1)] })
		} else { @('[General]', $entry) + $lines }
		Set-Content -LiteralPath $r.CustomIni -Value $lines -Encoding utf8NoBOM
		Say "Fallout4Custom.ini: $entry (original saved)"
	}

	if ($WaitForGo) {
		$go = Join-Path $script:StateDir "manual-go-$Runtime"
		$cancel = Join-Path $script:StateDir "manual-cancel-$Runtime"
		Remove-Item -LiteralPath $go, $cancel -Force -ErrorAction SilentlyContinue
		Set-Content -LiteralPath $waiting -Value $runDir
		Say "Installed; holding the install until the go signal ($go), at most $MaxWaitMinutes minutes"
		$waitUntil = (Get-Date).AddMinutes($MaxWaitMinutes)
		$renewed = Get-Date
		while (-not (Test-Path -LiteralPath $go)) {
			if (Test-Path -LiteralPath $cancel) { throw 'Cancelled before launch.' }
			if ((Get-Date) -gt $waitUntil) { throw "No go signal within $MaxWaitMinutes minutes." }
			Start-Sleep -Seconds 5
			if (((Get-Date) - $renewed).TotalSeconds -ge 60) { Update-GameLock -Minutes 30; $renewed = Get-Date }
		}
		Remove-Item -LiteralPath $go, $waiting -Force -ErrorAction SilentlyContinue
		Say 'Go signal received; launching'
	}

	$started = Get-Date
	# An earlier game ended in abort() during startup with ~3.8 GB of commit left: logged so
	# a crash can be read against it.
	try { Say ('Memory at launch: {0:N1} GB RAM free, {1:N1} GB commit free' -f (Get-FreePhysicalMemoryGB), (Get-CommitFreeGB)) }
	catch { Say "Memory at launch: unavailable ($($_.Exception.Message))" }
	Start-Process -FilePath (Join-Path $r.Game $r.Loader) -WorkingDirectory $r.Game | Out-Null
	Say "Launched $($r.Loader)"
	$game = $null
	$deadline = $started.AddSeconds($StartTimeoutSeconds)
	while (-not $game -and (Get-Date) -lt $deadline) {
		Start-Sleep -Seconds 2
		$game = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($r.Exe)) -ErrorAction SilentlyContinue |
			Where-Object { $_.StartTime -ge $started -and $_.Path -eq (Join-Path $r.Game $r.Exe) } | Select-Object -First 1
	}
	if (-not $game) { throw 'The game process never appeared.' }
	Say "Game running (PID $($game.Id)); the session lasts until it exits"
	# Nothing is taken out of the install while the game runs: a renewal that fails (a lock
	# file another process holds open for a moment, or another project's reservation) is
	# logged and tried again, and the session keeps waiting for the game to end.
	$renewWarned = 0
	while (-not $game.HasExited) {
		Start-Sleep -Seconds 60
		if ($game.HasExited) { break }
		try { Update-GameLock -Minutes 30; $renewWarned = 0 }
		catch { if ($renewWarned++ -lt 3) { Say "WARNING: could not renew the reservation ($($_.Exception.Message)); retrying while the game runs" } }
	}
	Say "Game exited (code $($game.ExitCode))"
} catch {
	Say "ERROR: $($_.Exception.Message)"
} finally {
	# The tracked process can report its exit while the game is still shutting down; files
	# it holds cannot be taken out until every process of this install has gone.
	# A game this session launched is waited for however long it runs (an error above must
	# not pull files out from under a player); otherwise two minutes for a closing one.
	$exe = Join-Path $r.Game $r.Exe
	$running = { Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($r.Exe)) -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } }
	$ours = $game -and -not $game.HasExited
	if ($ours) { Say 'Waiting for the game to end before cleaning up' }
	$quietBy = if ($ours) { (Get-Date).AddHours(12) } else { (Get-Date).AddSeconds(120) }
	while ((Get-Date) -lt $quietBy -and (& $running)) {
		Start-Sleep -Seconds 5
		if ($ours -and ((Get-Date) - $renewedAt).TotalSeconds -ge 60) {
			try { Update-GameLock -Minutes 30 } catch { }
			$renewedAt = Get-Date
		}
	}
	$cleanupFailed = $false
	foreach ($file in @($r.PluginLog, (Join-Path $r.LogDir 'f4se.log'))) {
		if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).LastWriteTime -ge $started) {
			Copy-Item -LiteralPath $file -Destination $runDir -Force
		}
	}
	foreach ($crash in @(Get-ChildItem -LiteralPath $r.LogDir -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
			Where-Object { $_.LastWriteTime -ge $started })) {
		Copy-Item -LiteralPath $crash.FullName -Destination $runDir -Force; Say "CRASH LOG: $($crash.Name)"
	}
	if ($iniTouched -and $iniExisted -and (Test-Path -LiteralPath $iniBackup)) {
		Copy-Item -LiteralPath $iniBackup -Destination $r.CustomIni -Force; Say 'Fallout4Custom.ini restored'
	} elseif ($iniTouched -and -not $iniExisted) {
		Remove-Item -LiteralPath $r.CustomIni -Force -ErrorAction SilentlyContinue; Say 'Fallout4Custom.ini removed (it did not exist before)'
	}
	if ($modsDeployed) {
		try { & "$PSScriptRoot\Deploy-TestMods.ps1" -Runtime $Runtime -Remove -Session $runDir | ForEach-Object { Say $_ } }
		catch { Say "ERROR: test-mod removal failed: $($_.Exception.Message)"; $cleanupFailed = $true }
	}
	if ($deployed) {
		try { & "$PSScriptRoot\Deploy-Plugin.ps1" -Runtime $Runtime -Remove -RestoreEditedIni -Session $runDir | ForEach-Object { Say $_ } }
		catch { Say "ERROR: undeploy failed: $($_.Exception.Message)"; $cleanupFailed = $true }
	}
	Remove-Item -LiteralPath $waiting -Force -ErrorAction SilentlyContinue
	# A failed step keeps the record, so the next session finishes it before installing.
	if ($recorded -and -not $cleanupFailed) { Remove-Item -LiteralPath $record -Force -ErrorAction SilentlyContinue }
	if ($cleanupFailed) { Say "Cleanup incomplete; $record kept for the next session" }
	if ($locked) { Exit-GameLock; Say 'Lock released' }
}
