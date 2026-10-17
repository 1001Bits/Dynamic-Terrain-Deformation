# Runs a plan's INI phases against the visible manual session (Start-ManualSession.ps1), for a
# timed run the player watches: each phase appends its settings to the deployed INI (live
# reload), waits, and leaves the record analyze-phases.py reads in the session's run folder.
# There is no command driver in a manual session: the player sets the scene by hand first.
#   Invoke-ManualPhases.ps1 -Runtime AE -Plan plans\plan-visible-tops-deathclaw.psd1
# The plan holds Phases = @( @{ Name; Settings = @{ Key = Value }; Wait } ).
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE')][string]$Runtime,
	[Parameter(Mandatory)][string]$Plan
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\IngameCommon.ps1"
$r = Get-Runtime $Runtime
$record = Join-Path $script:StateDir "manual-session-$Runtime.json"
if (-not (Test-Path -LiteralPath $record)) { throw "No manual session on $Runtime is running." }
$session = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json -AsHashtable
$runDir = $session['runDir']
if (-not (Get-Process -Id $session['pid'] -ErrorAction SilentlyContinue)) { throw 'The manual session has ended.' }
$spec = Import-PowerShellDataFile -LiteralPath $Plan
$ini = Join-Path $r.Plugins 'NMN_DeformableTerrain.ini'
$log = Join-Path $runDir 'phases.log'
Copy-Item -LiteralPath $Plan -Destination (Join-Path $runDir 'phases-plan.psd1') -Force
function Note([string]$Text) {
	$line = "[$((Get-Date).ToString('HH:mm:ss'))] $Text"
	Write-Host $line
	Add-Content -LiteralPath $log -Value $line
}
$baseIni = Get-Content -LiteralPath $ini -Raw
try {
	foreach ($phase in $spec.Phases) {
		if (-not (Get-Process -Id $session['pid'] -ErrorAction SilentlyContinue)) { throw 'The manual session ended mid-plan.' }
		$extra = @($phase.Settings.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" })
		$writtenAt = Get-Date
		[IO.File]::WriteAllText($ini, $baseIni.TrimEnd() + "`r`n`r`n[Phase $($phase.Name)]`r`n" + ($extra -join "`r`n") + "`r`n",
			[Text.UTF8Encoding]::new($false))
		Note "Phase $($phase.Name) [$($extra -join ' ')]"
		Start-Sleep -Seconds ([int]($phase.Wait ?? 40))
		$measuredAt = Get-Date
		Write-AtomicJson (Join-Path $runDir "$($phase.Name).json") ([ordered]@{
				name = $phase.Name; settings = $extra
				written = $writtenAt.ToString('HH:mm:ss.fff'); measuredUntil = $measuredAt.ToString('HH:mm:ss.fff')
				writtenAt = $writtenAt.ToString('o'); measuredUntilAt = $measuredAt.ToString('o')
			})
	}
} finally {
	[IO.File]::WriteAllText($ini, $baseIni, [Text.UTF8Encoding]::new($false))
	Note 'Deployed INI restored to the session preset'
}
