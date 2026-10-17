<#
.SYNOPSIS
  Tells a Start-ManualSession.ps1 -WaitForGo session to launch the game, or (-Cancel) to put
  everything back without launching.
#>
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE')][string]$Runtime,
	[switch]$Cancel
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\IngameCommon.ps1"
$waiting = Join-Path $script:StateDir "manual-waiting-$Runtime"
if (-not (Test-Path -LiteralPath $waiting)) { throw "No session is waiting on $Runtime." }
# A marker left by a session that was killed: nobody would read the signal.
$record = Join-Path $script:StateDir "manual-session-$Runtime.json"
if (Test-Path -LiteralPath $record) {
	$session = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json -AsHashtable
	$alive = Get-Process -Id ([int]$session['pid']) -ErrorAction SilentlyContinue
	if (-not $alive -or $alive.StartTime.ToUniversalTime().Ticks -ne [long]$session['processStart']) {
		throw "The session waiting on $Runtime (PID $($session['pid'])) is gone; start a new one, which also finishes its cleanup."
	}
}
$signal = Join-Path $script:StateDir ("manual-{0}-$Runtime" -f $(if ($Cancel) { 'cancel' } else { 'go' }))
Set-Content -LiteralPath $signal -Value (Get-Date).ToString('o')
"Sent $(if ($Cancel) { 'cancel' } else { 'go' }) to the session in $(Get-Content -LiteralPath $waiting)"
