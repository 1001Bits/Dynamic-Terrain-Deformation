# Drives one private smoke run's scene through native console commands and live INI
# phases, capturing only the owned window. Run it while Invoke-SmokeTest.ps1 is active.
#   scene-probe.ps1 -Run <run dir> -Runtime OG -Plan <plan.psd1>
# The plan holds: Setup (commands), SettleSeconds, Views = @( @{ Name; Commands; Phases =
# @( @{ Name; Settings = @{ Key = Value }; Wait } ) } ).
param(
	[Parameter(Mandatory)][string]$Run,
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR')][string]$Runtime,
	[Parameter(Mandatory)][string]$Plan
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
. "$kit\IngameCommon.ps1"
. "$kit\PrivateDesktop.ps1"
$r = Get-Runtime $Runtime
$Run = (Resolve-Path -LiteralPath $Run).Path
$deployment = Join-Path $script:StateDir "deploy-$Runtime.json"
function Test-SceneSession {
	if (-not (Test-Path -LiteralPath $deployment)) { return $false }
	$manifest = Get-Content -LiteralPath $deployment -Raw | ConvertFrom-Json -AsHashtable
	return $manifest['session'] -eq $Run
}
if (-not (Test-SceneSession)) { throw 'The scene plan does not own the current deployment.' }
$spec = Import-PowerShellDataFile -LiteralPath $Plan
$driver = Join-Path $Run 'driver'
$ini = Join-Path $r.Plugins 'NMN_DeformableTerrain.ini'
# A phase's Profile text becomes this surface profile module; the folder is only ever
# created and removed by this script, never adopted.
$profileDir = Join-Path $r.Plugins 'NMN_DeformableTerrain\NMNTerrainProbe'
$profileOwned = -not (Test-Path -LiteralPath $profileDir)
$log = Join-Path $Run 'probe.log'
Copy-Item -LiteralPath $Plan -Destination (Join-Path $Run 'probe-plan.psd1') -Force
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $Run 'scene-probe.ps1') -Force

function Note([string]$Text) {
	$line = "[$((Get-Date).ToString('HH:mm:ss'))] $Text"
	Write-Host $line
	Add-Content -LiteralPath $log -Value $line
}
function Send([string[]]$Commands) {
	if (-not (Test-SceneSession)) { throw 'The scene session ended or its deployment changed.' }
	& "$kit\Send-TestCommand.ps1" -Runtime $Runtime -Directory $driver -Commands $Commands
}
function Settled([int]$Seconds) {
	# Commands take effect after their acknowledgement: wait, then require two equal states.
	Start-Sleep -Seconds $Seconds
	$previous = Send '@status'
	for ($i = 0; $i -lt 10; $i++) {
		Start-Sleep -Seconds 2
		$state = Send '@status'
		$moved = 0.0
		for ($k = 0; $k -lt 3; $k++) { $moved = [Math]::Max($moved, [Math]::Abs($state.position[$k] - $previous.position[$k])) }
		if ($moved -lt 0.5 -and $state.menuMode -eq 0) { return $state }
		$previous = $state
	}
	Note "WARNING: scene did not settle (last move $moved, menuMode $($state.menuMode))"
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
Note "World routed; driver ready"
$baseIni = Get-Content -LiteralPath $ini -Raw
$phases = @()
try {
	# Toggles execute once per run, individually: the feature plan needs tgm, while a
	# later performance plan additionally needs tai. A single "setup done" flag used
	# to skip that later tai and benchmark moving actors instead of a fixed scene.
	$setupDone = Join-Path $Run 'probe-setup.done'
	[string[]]$sentSetup = @(if (Test-Path -LiteralPath $setupDone) { Get-Content -LiteralPath $setupDone })
	foreach ($command in @($spec.Setup)) {
		$isToggle = $command.Trim() -match '^(?i:tgm|tcl|tai)$'
		if ($isToggle -and $sentSetup -contains $command) { continue }
		$null = Send @($command)
		if ($isToggle) {
			$sentSetup += $command
			Set-Content -LiteralPath $setupDone -Value $sentSetup
		}
	}
	foreach ($view in $spec.Views) {
		$null = Send $view.Commands
		$state = Settled ([int]$spec.SettleSeconds)
		Note ("View {0}: pos {1} angle {2} land {3} water {4} texture {5}" -f $view.Name, ($state.position -join ','),
			($state.angleDegrees -join ','), $state.landHeight, $state.cellWaterHeight, $state.diffuseTexture)
		Write-AtomicJson (Join-Path $Run "view-$($view.Name).json") $state
		foreach ($phase in $view.Phases) {
			if (-not (Test-SceneSession)) { throw 'The scene session ended before its next INI phase.' }
			$name = "$($view.Name)-$($phase.Name)"
			$extra = @($phase.Settings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" })
			$text = $baseIni.TrimEnd() + "`r`n`r`n[Probe $name]`r`n" + ($extra -join "`r`n") + "`r`n"
			if ($phase.ContainsKey('Profile')) {
				if (-not $profileOwned) { throw "$profileDir existed before this probe; not touching it." }
				$profileTarget = Join-Path $profileDir 'Surfaces.ini'
				$profileSnapshot = Join-Path $Run "$name.Surfaces.ini"
				[IO.File]::WriteAllText($profileSnapshot, [string]$phase.Profile, [Text.UTF8Encoding]::new($false))
				# Include probe files in the runner's recovery manifest before writing them,
				# so a killed controller cannot leave a surface override in the game install.
				$manifest = Get-Content -LiteralPath $deployment -Raw | ConvertFrom-Json -AsHashtable
				if ($manifest['session'] -ne $Run) { throw 'Scene deployment changed before profile staging.' }
				$entry = @($manifest['files'] | Where-Object { $_['target'] -eq $profileTarget })
				if (-not $entry.Count) {
					$manifest['files'] += @{ target = $profileTarget; backup = $null; sha256 = (Get-FileHash -LiteralPath $profileSnapshot).Hash }
				} else { $entry[0]['sha256'] = (Get-FileHash -LiteralPath $profileSnapshot).Hash }
				if (-not (Test-Path -LiteralPath $profileDir) -and $manifest['createdDirs'] -notcontains $profileDir) {
					$manifest['createdDirs'] += $profileDir
				}
				Write-AtomicJson $deployment $manifest
				New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
				Copy-Item -LiteralPath $profileSnapshot -Destination $profileTarget -Force
			} elseif ($profileOwned -and (Test-Path -LiteralPath $profileDir)) {
				Remove-Item -LiteralPath $profileDir -Recurse -Force
			}
			Set-Content -LiteralPath $ini -Value $text -Encoding utf8NoBOM -NoNewline
			# Local wall-clock times, as the plugin log stamps its lines, so profiler windows
			# can be matched to the phase that was live.
			$writtenAt = Get-Date
			$written = $writtenAt.ToString('HH:mm:ss.fff')
			Copy-Item -LiteralPath $ini -Destination (Join-Path $Run "$name.ini") -Force
			Start-Sleep -Seconds ([int]($phase.Wait ?? 12))
			$before = Send '@status'
			$measuredAt = Get-Date
			$measured = $measuredAt.ToString('HH:mm:ss.fff')
			$png = Join-Path $Run "$name.png"
			if (-not ($phase.ContainsKey('NoCapture') -and $phase.NoCapture)) {
				try { Save-PrivateGameWindow -Run $Run -Path $png } catch { Note "Capture failed: $($_.Exception.Message)" }
			}
			$after = Send '@status'
			$record = [ordered]@{ name = $name; settings = $extra; written = $written; measuredUntil = $measured
				writtenAt = $writtenAt.ToString('o'); measuredUntilAt = $measuredAt.ToString('o')
				before = $before; after = $after
				image = if (Test-Path -LiteralPath $png) { (Get-FileHash -LiteralPath $png).Hash } else { $null } }
			Write-AtomicJson (Join-Path $Run "$name.json") $record
			$phases += $record
			Note ("Phase {0} [{1}] pos {2} hour {3} weather {4:X8}" -f $name, ($extra -join ' '),
				($after.position -join ','), $after.gameHour, $after.weatherId)
		}
	}
} finally {
	# The runner may have finished while a phase waited. Never recreate its deleted
	# INI or overwrite a later session after the deployment has been restored.
	if (Test-SceneSession) {
		Set-Content -LiteralPath $ini -Value $baseIni -Encoding utf8NoBOM -NoNewline
		if ($profileOwned -and (Test-Path -LiteralPath $profileDir)) {
			Remove-Item -LiteralPath $profileDir -Recurse -Force
			Note 'Probe surface profile removed'
		}
		Note 'Deployed INI restored to the run preset'
	} else {
		Note 'Scene deployment already ended; no install files changed by controller cleanup'
	}
	if ($phases.Count) { Write-AtomicJson (Join-Path $Run 'probe-phases.json') $phases }
}
