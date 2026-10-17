# Finishes an Invoke-SmokeTest run whose runner process died: closes the run's own game
# (verified by PID, path and start time), collects logs, restores the custom INI,
# undeploys, releases the kit's lock and runs the log check. Mirrors the runner's finally.
param(
	[Parameter(Mandatory)][string]$Run,
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR')][string]$Runtime
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
. "$kit\IngameCommon.ps1"
. "$kit\PrivateDesktop.ps1"
. "$kit\PrivateSteamVR.ps1"
$r = Get-Runtime $Runtime
$Run = (Resolve-Path -LiteralPath $Run).Path
$null = Get-ContainedPath (Join-Path $script:StateDir 'runs') ([IO.Path]::GetRelativePath((Join-Path $script:StateDir 'runs'), $Run))
$runnerRecord = Join-Path $Run 'runner.json'
$owner = $null
if (Test-Path -LiteralPath $runnerRecord) {
	$owner = Get-Content -LiteralPath $runnerRecord -Raw | ConvertFrom-Json
	$runner = Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
	if ($runner -and $runner.StartTime.ToUniversalTime().Ticks -eq [long]$owner.startUtcTicks) {
		throw 'This run still has a live controller; recovery must not race its cleanup.'
	}
}
$releaseLease = $false
if (Test-Path -LiteralPath $script:LeaseFile) {
	$lease = Get-Content -LiteralPath $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
	if (-not $owner -or -not $owner.PSObject.Properties['claim_token'] -or $lease['claim_token'] -ne $owner.claim_token) {
		throw 'The current kit lease belongs to another or unidentified run; refusing recovery.'
	}
	$releaseLease = $true
}
# A stale run must never restore another run's current install or user INI.
foreach ($manifestName in @("deploy-$Runtime.json", "test-mods-$Runtime.json")) {
	$manifestPath = Join-Path $script:StateDir $manifestName
	if (Test-Path -LiteralPath $manifestPath) {
		$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
		if ($manifest['session'] -ne $Run) { throw "$manifestName belongs to another or unidentified session; refusing recovery." }
	}
}
$transcript = Join-Path $Run 'smoke.log'
function Say([string]$Text) {
	$line = "[$((Get-Date).ToString('HH:mm:ss'))] $Text"
	Write-Host $line
	Add-Content -LiteralPath $transcript -Value $line
}
Say 'RECOVERY: the runner process ended before its cleanup; finishing it from the run record.'
$desktopRecordPath = Join-Path $Run 'private-desktop.json'
$record = if (Test-Path -LiteralPath $desktopRecordPath) { Get-Content -LiteralPath $desktopRecordPath -Raw | ConvertFrom-Json } else { $null }
if ($record) {
	if ($record.gamePath -ne (Join-Path $r.Game $r.Exe) -or $record.desktop -notlike 'NmnTerrain_*') {
		throw 'The private desktop record does not belong to this runtime/test kit.'
	}
	$otherGames = @(Get-GameProcesses | Where-Object {
		$_.Id -ne $record.gamePid -or $_.Path -ne $record.gamePath -or
		$_.StartTime.ToUniversalTime().Ticks -ne [long]$record.gameStartUtcTicks
	})
	if ($otherGames.Count) { throw 'Another game is running; recovery must not modify its shared files.' }
	$started = [datetime]::new([long]$record.gameStartUtcTicks, [DateTimeKind]::Utc).ToLocalTime().AddMinutes(-1)
	$game = Get-Process -Id $record.gamePid -ErrorAction SilentlyContinue
	if ($game -and $game.Path -eq $record.gamePath -and
		$game.StartTime.ToUniversalTime().Ticks -eq [long]$record.gameStartUtcTicks) {
		Say "Closing the run's game (PID $($game.Id))"
		try {
			$desktop = [NmnIngame.PrivateDesktop]::new($record.desktop)
			try {
				$window = $desktop.FindWindow($game.Id)
				if ($window -ne [IntPtr]::Zero) { $desktop.CloseWindow($window, $game.Id) }
			} finally { $desktop.Dispose() }
		} catch { Say "Window close failed: $($_.Exception.Message)" }
		if (-not $game.WaitForExit(20000)) { $game.Kill(); [void]$game.WaitForExit(10000); Say 'Game did not close; killed it' }
		$record | Add-Member -NotePropertyName gameExited -NotePropertyValue $game.HasExited -Force
	} else {
		Say 'The run''s game is no longer running.'
		$record | Add-Member -NotePropertyName gameExited -NotePropertyValue $true -Force
	}
	$record | Add-Member -NotePropertyName inputAfter -NotePropertyValue ([NmnIngame.PrivateDesktop]::InputDesktopName()) -Force
	$record | Add-Member -NotePropertyName recovered -NotePropertyValue $true -Force
	Write-AtomicJson (Join-Path $Run 'private-desktop.json') $record
} else {
	# SteamVR starts before the game. A dead controller may have changed its settings
	# without ever creating a game record; no current game can be owned in that case.
	if (Get-GameProcesses) { throw 'No owned game record exists; refusing recovery while a game is running.' }
	$started = if (Test-Path -LiteralPath $runnerRecord) {
		[datetime]::new([long]$owner.startUtcTicks, [DateTimeKind]::Utc).ToLocalTime()
	} else { (Get-Item -LiteralPath $Run).CreationTime }
	Say 'No game was recorded; recovering deployment and any staged SteamVR session only.'
}
if ($Runtime -eq 'VR') {
	Restore-PrivateSteamVR $Run
	Say 'Persisted SteamVR ownership checked; settings restored or already original.'
}

foreach ($file in @($r.PluginLog, (Join-Path $r.LogDir 'f4se.log'), (Join-Path $r.LogDir 'f4se_loader.log'),
		(Join-Path $r.LogDir 'f4sevr.log'), (Join-Path $r.LogDir 'Buffout4.log'))) {
	if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).LastWriteTime -ge $started) {
		Copy-Item -LiteralPath $file -Destination $Run -Force
	}
}
foreach ($crash in @(Get-ChildItem -LiteralPath $r.LogDir, (Split-Path $r.LogDir) -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
		Where-Object { $_.LastWriteTime -ge $started })) {
	Copy-Item -LiteralPath $crash.FullName -Destination $Run -Force; Say "CRASH LOG: $($crash.Name)"
}
Copy-Item -LiteralPath (Join-Path $script:StateDir "deploy-$Runtime.json") -Destination $Run -ErrorAction SilentlyContinue

$backup = Join-Path $Run 'Fallout4Custom.ini.original'
$testIni = Join-Path $Run 'Fallout4Custom.ini.test'
if ((Test-Path -LiteralPath $r.CustomIni) -and (Test-Path -LiteralPath $testIni) -and
	(Get-FileHash -LiteralPath $r.CustomIni).Hash -ne (Get-FileHash -LiteralPath $testIni).Hash -and
	(-not (Test-Path -LiteralPath $backup) -or (Get-FileHash -LiteralPath $r.CustomIni).Hash -ne (Get-FileHash -LiteralPath $backup).Hash)) {
	throw 'Fallout4Custom.ini was changed after this run; refusing to overwrite it during recovery.'
}
if (Test-Path -LiteralPath $backup) {
	Copy-Item -LiteralPath $backup -Destination $r.CustomIni -Force
	Say 'Fallout4Custom.ini restored'
} else {
	Say 'No Fallout4Custom.ini backup in the run folder; left as it is for review.'
}
# Test mods the run installed (or other mods it took out) go back before the plugin.
if (Test-Path -LiteralPath (Join-Path $script:StateDir "test-mods-$Runtime.json")) {
	& "$kit\Deploy-TestMods.ps1" -Runtime $Runtime -Remove -Session $Run | ForEach-Object { Say $_ }
}
& "$kit\Deploy-Plugin.ps1" -Runtime $Runtime -Remove -RestoreEditedIni -Session $Run | ForEach-Object { Say $_ }
if ($releaseLease) { Exit-GameLock; Say 'Owned lock released' }
else { Say 'No owned lock remains to release' }

$logCopy = Join-Path $Run "$script:PluginName.log"
$pass = $false
if (Test-Path -LiteralPath $logCopy) {
	& "$kit\Check-PluginLog.ps1" -Runtime $Runtime -Log $logCopy -Json (Join-Path $Run 'check.json') | ForEach-Object { Say $_ }
	$pass = $LASTEXITCODE -eq 0
}
$art = Get-BuildArtifacts
[ordered]@{
	runtime = $Runtime; preset = 'smoke'; commit = $art.Commit
	outcome = 'runner process ended early; recovered after the soak finished'; logCheckPass = $pass
	runDir = $Run; finished = (Get-Date).ToString('o'); recovered = $true
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'summary.json') -Encoding utf8NoBOM
