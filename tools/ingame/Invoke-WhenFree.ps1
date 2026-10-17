<#
.SYNOPSIS
  Waits until a runtime is free, then runs one private smoke test with a scene plan or a
  soak beside it.

.DESCRIPTION
  The smoke runner is started through Win32_Process.Create, outside this process's job,
  so stopping this controller (or the session that started it) cannot kill the runner
  mid-run: the runner always reaches its own cleanup (game closed, install and INI
  restored, lock released). Only the scene controller stops with this script.

.EXAMPLE
  .\Invoke-WhenFree.ps1 -Runtime AE -Plan .\plans\plan-ae-perf.psd1 -WorldSeconds 1800 -NoVsync
  .\Invoke-WhenFree.ps1 -Runtime AE -SoakMinutes 40 -WorldSeconds 2760
#>
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR')][string]$Runtime,
	[string]$Plan,
	[int]$SoakMinutes = 0,
	[int]$WorldSeconds = 600,
	[string]$Preset = 'smoke',
	[switch]$NoVsync,
	[int]$WaitMinutes = 180
)
$ErrorActionPreference = 'Stop'
if (-not $Plan -and $SoakMinutes -le 0) { throw 'Give -Plan or -SoakMinutes.' }
if ($Plan) { $Plan = (Resolve-Path -LiteralPath $Plan).Path }
$repo = Split-Path (Split-Path $PSScriptRoot)
function Say([string]$Text) { "[$((Get-Date).ToString('HH:mm:ss'))] $Text" }

$deadline = (Get-Date).AddMinutes($WaitMinutes)
while ($true) {
	# The checker reports on the host; its exit code says whether the runtime is free.
	& "$PSScriptRoot\Test-GameFree.ps1" -Runtime $Runtime *> $null
	if ($LASTEXITCODE -eq 0) { break }
	if ((Get-Date) -gt $deadline) { throw "$Runtime still busy after $WaitMinutes minutes" }
	Start-Sleep -Seconds 5
}
Say "$Runtime free; starting"

$runs = Join-Path $PSScriptRoot '.state\runs'
$before = @(Get-ChildItem -LiteralPath $runs -Directory | ForEach-Object Name)
$pwsh = (Get-Command pwsh).Source
$arguments = '-NoProfile -File "{0}" -Runtime {1} -TestDriver -Preset {2} -WorldSeconds {3} -Screenshots 1' -f
	(Join-Path $PSScriptRoot 'Invoke-SmokeTest.ps1'), $Runtime, $Preset, $WorldSeconds
if ($Runtime -eq 'VR') { $arguments += ' -SteamVRNull' }
if ($NoVsync) { $arguments += ' -NoVsync' }
$startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
$created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
	CommandLine = "`"$pwsh`" $arguments"; CurrentDirectory = $repo; ProcessStartupInformation = $startup
}
if ($created.ReturnValue -ne 0) { throw "Win32_Process.Create failed with $($created.ReturnValue)" }
$runner = Get-Process -Id $created.ProcessId
$null = $runner.Handle
Say "Runner PID $($runner.Id) (detached)"

$run = $null
for ($i = 0; $i -lt 90 -and -not $run; $i++) {
	Start-Sleep -Seconds 1
	$run = Get-ChildItem -LiteralPath $runs -Directory |
		Where-Object { $_.Name -notin $before -and $_.Name -like "*-$Runtime-$Preset" } | Select-Object -First 1
}
if (-not $run) { throw 'The smoke run never created its folder.' }
Say "Run: $($run.FullName)"
try {
	if ($SoakMinutes -gt 0) {
		& "$PSScriptRoot\Invoke-Soak.ps1" -Run $run.FullName -Runtime $Runtime -Minutes $SoakMinutes
	} else {
		& "$PSScriptRoot\Invoke-ScenePlan.ps1" -Run $run.FullName -Runtime $Runtime -Plan $Plan
	}
} catch {
	Say "Controller failed: $($_.Exception.Message)"
}
$runner.WaitForExit()
Say "Runner exited with $($runner.ExitCode)"
$summary = Join-Path $run.FullName 'summary.json'
if (Test-Path -LiteralPath $summary) { Get-Content -LiteralPath $summary -Raw }
