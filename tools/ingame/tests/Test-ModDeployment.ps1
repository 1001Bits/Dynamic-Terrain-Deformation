# File/lock fixtures only. All writes stay in a unique TEMP sandbox; no game starts.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$sandbox = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-mod-deploy-' + [guid]::NewGuid().ToString('N'))
$previous = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $sandbox
function Get-Process { param($Name, $Id, $ErrorAction) }
function Check([bool]$Ok, [string]$Message) { if (-not $Ok) { throw "FAIL $Message" }; "PASS $Message" }
function Rejected([scriptblock]$Action) {
	try { & $Action | Out-Null; return $false } catch { return $true }
}
try {
	$data = Join-Path $sandbox 'OG\Data'
	$stage = Join-Path $sandbox '.state\mods\Fixture'
	$plugins = Join-Path $sandbox 'LocalAppData\Fallout4\Plugins.txt'
	$manifest = Join-Path $sandbox '.state\test-mods-OG.json'
	New-Item -ItemType Directory -Force -Path $data, $stage, (Split-Path $plugins), (Join-Path $data 'F4SE\Plugins') | Out-Null
	Set-Content -LiteralPath (Join-Path $stage 'Fixture.esp') -Value 'fixture plugin'
	Set-Content -LiteralPath (Join-Path $stage 'asset.ba2') -Value 'fixture asset'
	Set-Content -LiteralPath (Join-Path $data 'Other.esp') -Value 'other plugin'
	Set-Content -LiteralPath (Join-Path $data 'F4SE\Plugins\Other.dll') -Value 'other helper'
	[IO.File]::WriteAllBytes($plugins, [Text.Encoding]::UTF8.GetBytes("*Other.esp`r`n"))
	$pluginsBefore = (Get-FileHash -LiteralPath $plugins).Hash
	Check (Rejected { & "$kit\Deploy-TestMods.ps1" -Runtime OG -Mod Fixture }) 'unowned install refuses test mods'
	& { . "$kit\IngameCommon.ps1"; $null = Enter-GameLock (Get-Runtime OG) -Minutes 5 -Scope 'sandbox fixture' }
	Set-Content -LiteralPath (Join-Path $sandbox 'outside.dll') -Value 'outside'
	Check (Rejected { & "$kit\Deploy-TestMods.ps1" -Runtime OG -Disable '..\..\outside.dll' }) 'disable traversal is refused'
	Check ((Get-Content -LiteralPath (Join-Path $sandbox 'outside.dll') -Raw).Trim() -eq 'outside') 'traversal leaves outside file intact'
	& "$kit\Deploy-TestMods.ps1" -Runtime OG -Mod Fixture -Disable 'Other.esp','F4SE\Plugins\Other.dll' | Out-Null
	$m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
	Check ($m.files.Count -eq 2 -and $m.fileHashes.PSObject.Properties['Fixture.esp']) 'complete file ownership recorded'
	Check (-not (Test-Path -LiteralPath (Join-Path $data 'F4SE\Plugins\Other.dll'))) 'disabled helper is set aside'
	Check ((Get-Content -LiteralPath $plugins -Raw) -match '(?m)^Other.esp\r?$') 'disabled plugin is unticked'
	# A replacement file must survive removal and leave recovery evidence intact.
	Remove-Item -LiteralPath (Join-Path $data 'asset.ba2')
	Set-Content -LiteralPath (Join-Path $data 'asset.ba2') -Value 'foreign replacement'
	Check (Rejected { & "$kit\Deploy-TestMods.ps1" -Runtime OG -Remove }) 'changed test asset blocks destructive recovery'
	Check ((Test-Path -LiteralPath $manifest) -and (Test-Path -LiteralPath (Join-Path $data 'Fixture.esp'))) 'failed preflight preserves manifest and all deployed files'
	Copy-Item -LiteralPath (Join-Path $stage 'asset.ba2') -Destination (Join-Path $data 'asset.ba2') -Force
	Set-Content -LiteralPath (Join-Path $data 'F4SE\Plugins\Other.dll') -Value 'replacement helper'
	Check (Rejected { & "$kit\Deploy-TestMods.ps1" -Runtime OG -Remove }) 'occupied restore destination keeps recovery manifest'
	Remove-Item -LiteralPath (Join-Path $data 'F4SE\Plugins\Other.dll')
	& "$kit\Deploy-TestMods.ps1" -Runtime OG -Remove | Out-Null
	Check ((Get-FileHash -LiteralPath $plugins).Hash -eq $pluginsBefore) 'Plugins.txt restored byte for byte'
	Check ((Get-Content -LiteralPath (Join-Path $data 'F4SE\Plugins\Other.dll') -Raw).Trim() -eq 'other helper') 'disabled helper restored'
	Check (-not (Test-Path -LiteralPath $manifest) -and -not (Test-Path -LiteralPath (Join-Path $data 'Fixture.esp'))) 'successful cleanup removes manifest and assets'
	# Simulate a copy failure on the second file, before the old 500-file checkpoint.
	$failureTarget = Join-Path $data 'asset.ba2'
	function New-Item {
		[CmdletBinding()] param($Path, $ItemType, $Target, [switch]$Force)
		if ($Path -eq $failureTarget) { throw 'injected link failure' }
		Microsoft.PowerShell.Management\New-Item @PSBoundParameters
	}
	function Copy-Item {
		[CmdletBinding()] param($LiteralPath, $Destination, [switch]$Force)
		if ($Destination -eq $failureTarget) { throw 'injected copy failure' }
		Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
	}
	Check (Rejected { & "$kit\Deploy-TestMods.ps1" -Runtime OG -Mod Fixture }) 'injected partial deployment fails'
	Remove-Item Function:\New-Item, Function:\Copy-Item
	$m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
	Check ($m.files.Count -eq 2) 'partial deployment retains its complete recovery plan'
	& "$kit\Deploy-TestMods.ps1" -Runtime OG -Remove | Out-Null
	Check (-not (Test-Path -LiteralPath (Join-Path $data 'Fixture.esp'))) 'partial deployment cleanup removes files copied before failure'

	# A patch whose name sorts before its master is still enabled after it (TES4 MAST).
	$patched = Join-Path $sandbox '.state\mods\Patched'
	New-Item -ItemType Directory -Force -Path $patched | Out-Null
	function New-Plugin([string]$Path, [string[]]$Masters) {
		$body = [IO.MemoryStream]::new()
		foreach ($master in $Masters) {
			$name = [Text.Encoding]::ASCII.GetBytes($master + [char]0)
			$body.Write([Text.Encoding]::ASCII.GetBytes('MAST'), 0, 4)
			$body.Write([BitConverter]::GetBytes([uint16]$name.Length), 0, 2)
			$body.Write($name, 0, $name.Length)
		}
		$data = $body.ToArray()
		$record = [IO.MemoryStream]::new()
		$record.Write([Text.Encoding]::ASCII.GetBytes('TES4'), 0, 4)
		$record.Write([BitConverter]::GetBytes([uint32]$data.Length), 0, 4)
		$record.Write([byte[]]::new(16), 0, 16)
		$record.Write($data, 0, $data.Length)
		[IO.File]::WriteAllBytes($Path, $record.ToArray())
	}
	New-Plugin (Join-Path $patched 'A Patch.esp') @('Fallout4.esm', 'Z Base.esp')
	New-Plugin (Join-Path $patched 'Z Base.esp') @('Fallout4.esm')
	& "$kit\Deploy-TestMods.ps1" -Runtime OG -Mod Patched | Out-Null
	$order = @(Get-Content -LiteralPath $plugins | Where-Object { $_ -in '*A Patch.esp', '*Z Base.esp' })
	Check ($order.Count -eq 2 -and $order[0] -eq '*Z Base.esp' -and $order[1] -eq '*A Patch.esp') 'a patch is enabled after the master it names'
	& "$kit\Deploy-TestMods.ps1" -Runtime OG -Remove | Out-Null
	Check (-not (Test-Path -LiteralPath (Join-Path $data 'A Patch.esp'))) 'the patched pair is removed again'
	& { . "$kit\IngameCommon.ps1"; Exit-GameLock }
	'ALL MOD DEPLOYMENT TESTS PASS'
} finally {
	$env:NMN_INGAME_SANDBOX = $previous
	$resolved = [IO.Path]::GetFullPath($sandbox)
	$allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected sandbox cleanup path' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
