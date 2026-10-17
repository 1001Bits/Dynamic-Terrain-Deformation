# Long private session against one smoke run: cycles exterior travel, interior round
# trips, explosions, fires, dropped items, weather and WinterMode for -Minutes, while
# recording command latency (hang detection), the game's memory, and periodic captures.
param(
	[Parameter(Mandatory)][string]$Run,
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR')][string]$Runtime,
	[int]$Minutes = 40
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
. "$kit\IngameCommon.ps1"
. "$kit\PrivateDesktop.ps1"
$r = Get-Runtime $Runtime
$driver = Join-Path $Run 'driver'
$ini = Join-Path $r.Plugins 'NMN_DeformableTerrain.ini'
$log = Join-Path $Run 'soak.log'
$samples = Join-Path $Run 'soak-samples.csv'
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $Run 'soak.ps1') -Force

function Note([string]$Text) {
	$line = "[$((Get-Date).ToString('HH:mm:ss'))] $Text"
	Write-Host $line
	Add-Content -LiteralPath $log -Value $line
}
function Send([string[]]$Commands, [int]$Timeout = 60) {
	$watch = [Diagnostics.Stopwatch]::StartNew()
	$state = & "$kit\Send-TestCommand.ps1" -Runtime $Runtime -Directory $driver -Commands $Commands -TimeoutSeconds $Timeout
	$state | Add-Member -NotePropertyName latencyMs -NotePropertyValue $watch.ElapsedMilliseconds -Force
	$state
}

$deadline = (Get-Date).AddMinutes(8)
while (-not (Test-Path -LiteralPath (Join-Path $driver 'ready.json'))) {
	if ((Get-Date) -gt $deadline) { throw 'The driver never became ready.' }
	Start-Sleep -Seconds 2
}
while (-not ((Test-Path -LiteralPath $r.PluginLog) -and
		(Select-String -LiteralPath $r.PluginLog -Pattern 'Camera gate: ROUTING' -Quiet))) {
	if ((Get-Date) -gt $deadline) { throw 'No routed world before the deadline.' }
	Start-Sleep -Seconds 2
}
$game = Get-Process -Id ((Get-Content (Join-Path $driver 'ready.json') -Raw | ConvertFrom-Json).pid)
$baseIni = Get-Content -LiteralPath $ini -Raw
'utc,step,action,latencyMs,cell,exterior,x,y,z,land,menuMode,weather,privateMB,workingMB,handles' |
	Set-Content -LiteralPath $samples -Encoding utf8NoBOM

# Exterior grid cells along the Commonwealth (cow), and the interiors Codex verified.
$cells = @('-20 21', '-17 19', '-14 17', '-11 15', '-8 12', '-5 9', '-2 6', '-20 21')
$weathers = @('0002B52A', '001CA7E4', '001BD481', '0002B52A')
$step = 0
$end = (Get-Date).AddMinutes($Minutes)
$null = Send @('tgm', 'tai', 'set timescale to 20')
try {
	while ((Get-Date) -lt $end) {
		if ($game.HasExited) { Note "GAME EXITED (code $($game.ExitCode))"; break }
		$step++
		$cell = $cells[$step % $cells.Count]
		$actions = switch ($step % 6) {
			0 { @{ name = "interior"; commands = @('coc SanctuaryBasementJahani') } }
			1 { @{ name = "travel $cell"; commands = @("cow Commonwealth $cell") } }
			2 { @{ name = 'explosion'; commands = @('player.placeatme 000E574F 1') } }
			3 { @{ name = 'fire+rifle'; commands = @('player.placeatme 00048280 1', 'player.placeatme 0004F46A 1') } }
			4 { @{ name = "weather $($weathers[$step % 4])"; commands = @("fw $($weathers[$step % 4])") } }
			5 { @{ name = 'return'; commands = @("cow Commonwealth $cell") } }
		}
		# WinterMode alternates every third step, through the live reload.
		$winter = [int]([Math]::Floor($step / 3) % 2)
		Set-Content -LiteralPath $ini -Value ($baseIni.TrimEnd() + "`r`n`r`n[Soak]`r`nWinterMode=$winter`r`n") -Encoding utf8NoBOM -NoNewline
		try {
			$state = Send $actions.commands 90
			Start-Sleep -Seconds 20
			$state = Send @('@status') 90
		} catch {
			Note "COMMAND FAILED at step ${step}: $($_.Exception.Message)"
			if ($game.HasExited) { Note "GAME EXITED (code $($game.ExitCode))"; break }
			continue
		}
		$game.Refresh()
		$p = $state.position
		$row = @((Get-Date).ToUniversalTime().ToString('o'), $step, "$($actions.name) winter=$winter", $state.latencyMs,
			$state.cellId, $state.exterior, $p[0], $p[1], $p[2], $state.landHeight, $state.menuMode,
			('{0:X8}' -f [uint32]$state.weatherId), [int]($game.PrivateMemorySize64 / 1MB),
			[int]($game.WorkingSet64 / 1MB), $game.HandleCount) -join ','
		Add-Content -LiteralPath $samples -Value $row
		Note "step $step $($actions.name) winter=$winter latency $($state.latencyMs) ms cell $($state.cellId) private $([int]($game.PrivateMemorySize64 / 1MB)) MB"
		if ($step % 6 -eq 2) {
			try { Save-PrivateGameWindow -Run $Run -Path (Join-Path $Run ("soak-{0:D3}.png" -f $step)) } catch { Note "Capture failed: $($_.Exception.Message)" }
		}
	}
} finally {
	Set-Content -LiteralPath $ini -Value $baseIni -Encoding utf8NoBOM -NoNewline
	Note 'Soak finished; deployed INI restored'
}
