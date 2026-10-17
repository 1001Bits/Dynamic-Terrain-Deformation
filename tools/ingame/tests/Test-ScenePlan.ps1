# Exercise the real controller with an inert command client in a unique sandbox.
# No game, desktop, process, GPU or shared lock is used.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$sandbox = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-scene-plan-' + [guid]::NewGuid().ToString('N'))
$previous = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $sandbox
function Start-Sleep { param($Seconds, $Milliseconds) }
function Check([bool]$Ok, [string]$Message) { if (-not $Ok) { throw "FAIL $Message" }; "PASS $Message" }
try {
	$fixtureKit = Join-Path $sandbox 'tools\ingame'
	$run = Join-Path $sandbox '.state\runs\fixture'
	$ini = Join-Path $sandbox 'OG\Data\F4SE\Plugins\NMN_DeformableTerrain.ini'
	$gameLog = Join-Path $sandbox 'Documents\My Games\Fallout4\F4SE\NMN_DeformableTerrain.log'
	$manifest = Join-Path $sandbox '.state\deploy-OG.json'
	New-Item -ItemType Directory -Force -Path $fixtureKit, (Join-Path $run 'driver'), (Split-Path $ini), (Split-Path $gameLog) | Out-Null
	foreach ($file in @('Invoke-ScenePlan.ps1', 'IngameCommon.ps1')) { Copy-Item -LiteralPath (Join-Path $kit $file) -Destination $fixtureKit }
	Set-Content -LiteralPath (Join-Path $fixtureKit 'PrivateDesktop.ps1') -Value '# No desktop in a fixture.'
	Set-Content -LiteralPath (Join-Path $run 'driver\ready.json') -Value '{}'
	Set-Content -LiteralPath $gameLog -Value 'Camera gate: ROUTING'
	Set-Content -LiteralPath $ini -Value 'original preset' -NoNewline
	@{ session = $run; files = @(); createdDirs = @() } | ConvertTo-Json | Set-Content -LiteralPath $manifest
	$client = @'
param($Runtime, $Directory, [string[]]$Commands)
Add-Content -LiteralPath (Join-Path $env:NMN_INGAME_SANDBOX 'commands.log') -Value $Commands
if (Test-Path -LiteralPath (Join-Path $env:NMN_INGAME_SANDBOX 'end-session')) {
    Remove-Item -LiteralPath (Join-Path $env:NMN_INGAME_SANDBOX '.state\deploy-OG.json') -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $env:NMN_INGAME_SANDBOX 'OG\Data\F4SE\Plugins\NMN_DeformableTerrain.ini') -ErrorAction SilentlyContinue
}
[pscustomobject]@{ position = @(0,0,0); angleDegrees = @(0,0,0); menuMode = 0; landHeight = 0; cellWaterHeight = 0; diffuseTexture = ''; gameHour = 12; weatherId = 0 }
'@
	Set-Content -LiteralPath (Join-Path $fixtureKit 'Send-TestCommand.ps1') -Value $client
	$plan = Join-Path $sandbox 'first.psd1'
	Set-Content -LiteralPath $plan -Value @'
@{ Setup = @('set gamehour to 12', 'tgm'); SettleSeconds = 0; Views = @(
    @{ Name = 'first'; Commands = @('@status'); Phases = @(
        @{ Name = 'profile'; Settings = @{ WinterMode = 2 }; Wait = 0; NoCapture = $true; Profile = "[Fixture]`nTexture=dirt01`nType=mud`n" }
    ) }
) }
'@
	& (Join-Path $fixtureKit 'Invoke-ScenePlan.ps1') -Runtime OG -Run $run -Plan $plan | Out-Null
	$m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
	Check ($m.files.Count -eq 1 -and $m.files[0].sha256) 'probe profile is journalled for runner recovery'
	Check (-not (Test-Path -LiteralPath $m.files[0].target)) 'probe profile removed after its controller completes'
	Check ((Get-Content -LiteralPath $ini -Raw) -eq 'original preset') 'phase restores original preset exactly'
	$phase = Get-Content -LiteralPath (Join-Path $run 'first-profile.json') -Raw | ConvertFrom-Json
	Check ([bool]$phase.writtenAt -and [bool]$phase.measuredUntilAt) 'phase records full dates as well as log clock times'
	$second = Join-Path $sandbox 'second.psd1'
	Set-Content -LiteralPath $second -Value "@{ Setup = @('set gamehour to 12', 'tai', 'tgm'); SettleSeconds = 0; Views = @() }"
	& (Join-Path $fixtureKit 'Invoke-ScenePlan.ps1') -Runtime OG -Run $run -Plan $second | Out-Null
	$commands = @(Get-Content -LiteralPath (Join-Path $sandbox 'commands.log'))
	Check (@($commands | Where-Object { $_ -eq 'tgm' }).Count -eq 1 -and @($commands | Where-Object { $_ -eq 'tai' }).Count -eq 1) 'later plan applies its new toggle without repeating existing toggles'
	Check (@($commands | Where-Object { $_ -eq 'set gamehour to 12' }).Count -eq 2) 'idempotent scene setup is repeated for each plan'
	Set-Content -LiteralPath (Join-Path $sandbox 'end-session') -Value 'fixture'
	$ended = $false
	try { & (Join-Path $fixtureKit 'Invoke-ScenePlan.ps1') -Runtime OG -Run $run -Plan $plan | Out-Null } catch { $ended = $true }
	Check ($ended -and -not (Test-Path -LiteralPath $ini)) 'controller cannot recreate INI after its runner restores deployment'
	'ALL SCENE PLAN TESTS PASS'
} finally {
	$env:NMN_INGAME_SANDBOX = $previous
	$resolved = [IO.Path]::GetFullPath($sandbox)
	$allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected sandbox cleanup path' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
