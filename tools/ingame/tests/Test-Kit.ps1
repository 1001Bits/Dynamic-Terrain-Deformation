# Exercises the kit against a sandbox standing in for C:\Games (NMN_INGAME_SANDBOX): deploy and
# removal, both lock formats, and the smoke test's INI edit. Touches no real install or lock.
# Run after changing anything in tools\ingame:  pwsh -File tools\ingame\tests\Test-Kit.ps1
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$repo = (Resolve-Path (Join-Path $kit '..\..')).Path
$sandbox = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-ingame-kit-' + [guid]::NewGuid().ToString('N'))
$previousSandbox = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $sandbox
# These deployments target fake installs. A real game running elsewhere must not
# affect this sandbox test; no process inspection or termination is needed here.
function Get-Process { param($Name, $Id, $ErrorAction) }
$fails = 0
function Check([bool]$Ok, [string]$What) { if ($Ok) { "PASS $What" } else { "FAIL $What"; $script:fails++ } }

try {
	# A fake 1.10.163 install with another project's plugin and an older copy of our INI.
	$plugins = Join-Path $sandbox 'OG\Data\F4SE\Plugins'
	New-Item -ItemType Directory -Force $plugins | Out-Null
	Set-Content (Join-Path $plugins 'OtherProject.dll') 'other' -NoNewline
	Set-Content (Join-Path $plugins 'NMN_DeformableTerrain.ini') 'old user ini' -NoNewline
	$driverTarget = Join-Path $plugins 'NMN_TerrainTestDriver.dll'
	Set-Content -LiteralPath $driverTarget -Value 'prior test helper' -NoNewline
	$driverBuild = Join-Path $repo 'build\tools\ingame\driver\NMN_TerrainTestDriver.dll'
	$testDriver = Test-Path -LiteralPath $driverBuild
	$guardBuild = Join-Path $repo 'build\tools\ingame\driver\NMN_PrivateInputGuard.dll'
	$testGuard = Test-Path -LiteralPath $guardBuild
	$guardTarget = Join-Path $plugins 'NMN_PrivateInputGuard.dll'
	Set-Content -LiteralPath $guardTarget -Value 'prior guard' -NoNewline
	# A menu choice saved by an earlier session: set aside while testing, back afterwards.
	$choices = Join-Path $sandbox 'OG\Data\MCM\Settings\NMN_DeformableTerrain.ini'
	New-Item -ItemType Directory -Force (Split-Path $choices) | Out-Null
	Set-Content -LiteralPath $choices -Value 'bSnowTracks=1' -NoNewline
	$lockPath = Join-Path $sandbox 'GAME_LOCK.json'
	$priorityPath = Join-Path $sandbox 'priority-window.json'

	# 1. Deploy with a claimed lock, then remove.
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Preset visual -ClaimLock -TestDriver:$testDriver -PrivateInputGuard:$testGuard | Out-Null
	if ($testDriver) { Check ((Get-FileHash $driverTarget).Hash -eq (Get-FileHash $driverBuild).Hash) 'optional native driver deployed' }
	if ($testGuard) { Check ((Get-FileHash $guardTarget).Hash -eq (Get-FileHash $guardBuild).Hash) 'private test input guard deployed only on request' }
	$lock = Get-Content $lockPath -Raw | ConvertFrom-Json -AsHashtable
	Check ($lock['status'] -eq 'active' -and $lock['claim_token'].Length -eq 64 -and $lock['owner'] -and $lock['started']) 'lock written with both field sets'
	$priority = Get-Content $priorityPath -Raw | ConvertFrom-Json -AsHashtable
	Check ($priority['claim_token'] -eq $lock['claim_token'] -and $priority['ownership_token'].Length -eq 64) 'shared reservation uses the same ownership token'
	& { . "$kit\IngameCommon.ps1"; Update-GameLock -Minutes 90 }
	$renewed = Get-Content $priorityPath -Raw | ConvertFrom-Json -AsHashtable
	$renewedFlat = Get-Content $lockPath -Raw | ConvertFrom-Json -AsHashtable
	Check ([DateTimeOffset]$renewed['expires'] -gt [DateTimeOffset]::UtcNow.AddMinutes(80) -and
		[DateTimeOffset]$renewedFlat['expires'] -gt [DateTimeOffset]::UtcNow.AddMinutes(80)) 'both shared and install reservations renew'
	$built = Join-Path $repo 'build\src\NMN_DeformableTerrain.dll'
	Check ((Get-FileHash (Join-Path $plugins 'NMN_DeformableTerrain.dll')).Hash -eq (Get-FileHash $built).Hash) 'dll deployed'
	$ini = Get-Content (Join-Path $plugins 'NMN_DeformableTerrain.ini') -Raw
	Check ($ini -match 'EnableTessellation = 1' -and $ini -match '\[Test preset: visual\]' -and $ini -match 'DebugFieldColour = 1') 'shipped INI + preset deployed'
	Check (Test-Path (Join-Path $sandbox 'OG\Data\Textures\NMN_DeformableTerrain\FootprintHeight.dds')) 'textures deployed'
	$menu = Join-Path $sandbox 'OG\Data\MCM\Config\NMN_DeformableTerrain'
	Check ((Test-Path (Join-Path $menu 'config.json')) -and (Test-Path (Join-Path $menu 'settings.ini'))) 'in-game menu deployed'
	Check (-not (Test-Path -LiteralPath $choices)) 'an earlier menu choice is set aside for the session'
	Set-Content -LiteralPath $choices -Value 'iWinterMode=2' -NoNewline
	$second = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG | Out-Null } catch { $second = $_.Exception.Message }
	Check ($second -match 'Already deployed') 'second deploy refused'

	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -ReleaseLock | Out-Null
	Check (-not (Test-Path (Join-Path $plugins 'NMN_DeformableTerrain.dll'))) 'dll removed'
	Check ((Get-Content (Join-Path $plugins 'NMN_DeformableTerrain.ini') -Raw) -eq 'old user ini') 'user INI restored'
	Check ((Get-Content (Join-Path $plugins 'OtherProject.dll') -Raw) -eq 'other') 'other plugin untouched'
	Check ((Get-Content -LiteralPath $driverTarget -Raw) -eq 'prior test helper') 'prior native driver restored or preserved'
	Check ((Get-Content -LiteralPath $guardTarget -Raw) -eq 'prior guard') 'prior input guard restored'
	Check (-not (Test-Path (Join-Path $sandbox 'OG\Data\Textures\NMN_DeformableTerrain'))) 'created texture folder removed'
	Check (-not (Test-Path (Join-Path $sandbox 'OG\Data\MCM\Config'))) 'created menu folders removed'
	Check ((Get-Content -LiteralPath $choices -Raw) -eq 'bSnowTracks=1') 'the earlier menu choice is back'
	$kept = @(Get-ChildItem (Join-Path $sandbox '.state') -Filter 'session-mcm-OG-*.ini' -ErrorAction SilentlyContinue |
		Where-Object { (Get-Content -LiteralPath $_.FullName -Raw) -eq 'iWinterMode=2' })
	Check ($kept.Count -ge 1) "the session's own menu choices are kept"
	$kept | Remove-Item -Force
	Check (-not (Test-Path $lockPath)) 'lock released'
	Check ((Get-Content $priorityPath -Raw | ConvertFrom-Json).status -eq 'released') 'shared reservation released for other projects'

	# A later run must not adopt an old recovery record. Edited session INIs are
	# preserved explicitly before restoring the original user file.
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock -Session 'fixture-session' -DrawDiagnostics | Out-Null
	$wrongSession = $null
	try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -Session 'another-session' | Out-Null } catch { $wrongSession = $_.Exception.Message }
	Check ($wrongSession -match 'different session') 'another session cannot restore this deployment'
	$deployedIni = Join-Path $plugins 'NMN_DeformableTerrain.ini'
	Add-Content -LiteralPath $deployedIni -Value 'StampDepth=17'
	$editedHash = (Get-FileHash -LiteralPath $deployedIni).Hash
	$changed = $null
	try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -Session 'fixture-session' | Out-Null } catch { $changed = $_.Exception.Message }
	Check ($changed -match 'target changed') 'changed deployment refuses implicit overwrite'
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -Session 'fixture-session' -RestoreEditedIni -ReleaseLock | Out-Null
	$editedCopies = @(Get-ChildItem -LiteralPath (Join-Path $sandbox '.state') -Filter 'session-edited-OG-*.ini')
	Check ($editedCopies.Count -eq 1 -and (Get-FileHash -LiteralPath $editedCopies[0].FullName).Hash -eq $editedHash) 'live session INI preserved byte for byte'
	Check ((Get-Content -LiteralPath $deployedIni -Raw) -eq 'old user ini') 'edited session still restores the original user INI'

	# Force a failure after DLL/INI writes. The durable plan must recover all copies.
	$failTarget = Join-Path $sandbox 'OG\Data\Textures\NMN_DeformableTerrain\FootprintHeight.dds'
	function Copy-Item {
		[CmdletBinding()] param($LiteralPath, $Destination, [switch]$Force)
		if ($Destination -eq $failTarget) { throw 'injected deployment copy failure' }
		Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
	}
	$partial = $null
	try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $partial = $_.Exception.Message }
	Remove-Item Function:\Copy-Item
	Check ($partial -match 'injected deployment copy failure') 'partial plugin deployment failure injected'
	Check (Test-Path -LiteralPath (Join-Path $sandbox '.state\deploy-OG.json')) 'partial deployment has a recovery manifest'
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -ReleaseLock | Out-Null
	Check ((Get-Content -LiteralPath $deployedIni -Raw) -eq 'old user ini' -and -not (Test-Path -LiteralPath (Join-Path $plugins 'NMN_DeformableTerrain.dll'))) 'partial plugin deployment fully restored'

	# A private benchmark can hold the GPU between launches, without a game process.
	@{ id = 'private-benchmark'; status = 'active'; project = 'Other'; expires = [DateTimeOffset]::UtcNow.AddMinutes(20).ToString('o') } |
		ConvertTo-Json | Set-Content $priorityPath
	$foreignHash = (Get-FileHash $priorityPath).Hash
	$blocked = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $blocked = $_.Exception.Message }
	Check ($blocked -match 'shared benchmark reservation') 'foreign shared reservation blocks without a running game'
	Check ((Get-FileHash $priorityPath).Hash -eq $foreignHash -and -not (Test-Path $lockPath)) 'foreign shared reservation left unchanged'
	Set-Content $priorityPath '{invalid'
	$blocked = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $blocked = $_.Exception.Message }
	Check ($blocked -match 'unreadable lock file') 'unreadable shared reservation blocks'
	Set-Content $priorityPath '[]'
	$blocked = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $blocked = $_.Exception.Message }
	Check ($blocked -match 'unreadable lock file') 'non-object shared reservation blocks'
	Remove-Item -LiteralPath $priorityPath

	# 2. A foreign reservation blocks; 3. a legacy lock blocks; 4. an expired one does not.
	@{ id = 'other-run'; status = 'reserved'; project = 'Other'; expires = [DateTimeOffset]::UtcNow.AddMinutes(20).ToString('o') } |
		ConvertTo-Json | Set-Content $lockPath
	$blocked = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $blocked = $_.Exception.Message }
	Check ($blocked -match 'busy') 'foreign reservation blocks'
	Check ((Get-Content $lockPath -Raw) -match 'other-run') 'foreign lock left alone'

	@{ owner = 'old agent'; game = 'FO4'; started = [DateTimeOffset]::UtcNow.AddMinutes(-5).ToString('o') } |
		ConvertTo-Json | Set-Content $lockPath
	$blocked = $null; try { & "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null } catch { $blocked = $_.Exception.Message }
	Check ($blocked -match 'busy') 'legacy lock under 30 minutes blocks'

	@{ id = 'old-run'; status = 'reserved'; project = 'Other'; expires = [DateTimeOffset]::UtcNow.AddMinutes(-1).ToString('o') } |
		ConvertTo-Json | Set-Content $lockPath
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null
	Check ((Get-Content -LiteralPath $guardTarget -Raw) -eq 'prior guard') 'ordinary deployment does not install or replace input guard'
	Check ((Get-Content $lockPath -Raw) -match 'nmn-dtd') 'expired reservation taken over'
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -ReleaseLock | Out-Null
	Check (-not (Test-Path $lockPath)) 'own lock released'

	& "$kit\Deploy-Plugin.ps1" -Runtime OG -ClaimLock | Out-Null
	@{ id = 'replacement'; status = 'active'; project = 'Other'; claim_token = 'foreign'; expires = [DateTimeOffset]::UtcNow.AddMinutes(20).ToString('o') } |
		ConvertTo-Json | Set-Content $priorityPath
	$lost = $null; try { & { . "$kit\IngameCommon.ps1"; Update-GameLock } } catch { $lost = $_.Exception.Message }
	Check ($lost -match 'no longer ours') 'lost shared reservation ends renewal'
	& "$kit\Deploy-Plugin.ps1" -Runtime OG -Remove -ReleaseLock | Out-Null
	Check ((Get-Content $priorityPath -Raw | ConvertFrom-Json).id -eq 'replacement') 'cleanup preserves a replacement shared reservation'

	# 5. The INI edit the smoke test makes, on a copy of the real Fallout4Custom.ini.
	$smoke = Get-Content "$kit\Invoke-SmokeTest.ps1" -Raw
	$fn = [regex]::Match($smoke, '(?s)function Set-IniValue.*?\n}\r?\n').Value
	Invoke-Expression $fn
	$real = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Fallout4\Fallout4Custom.ini'
	$copy = Join-Path $sandbox 'Fallout4Custom.ini'
	if (Test-Path $real) { Copy-Item $real $copy } else { Set-Content $copy "[General]`nsLanguage=en" }
	$before = @(Get-Content $copy)
	$preserved = @($before | Where-Object { $_ -notmatch '^\s*sStartingConsoleCommand\s*=' })
	Set-IniValue $copy General sStartingConsoleCommand 'coc SanctuaryExt'
	$after = @(Get-Content $copy)
	$general = [Array]::FindIndex([string[]]$after, [Predicate[string]] { param($l) $l -match '^\s*\[General\]' })
	Check ($after[$general + 1] -eq 'sStartingConsoleCommand=coc SanctuaryExt') 'command inserted under [General]'
	Check (($after.Count -eq $preserved.Count + 1) -and
		(@(Compare-Object $preserved @($after | Where-Object { $_ -notmatch '^\s*sStartingConsoleCommand\s*=' })).Count -eq 0)) 'other lines unchanged'
	Set-IniValue $copy General sStartingConsoleCommand 'coc RedRocketExt'
	Check (@(Get-Content $copy | Where-Object { $_ -match 'sStartingConsoleCommand' }).Count -eq 1) 'no duplicate on a second edit'

	$noGeneral = Join-Path $sandbox 'empty.ini'
	Set-Content $noGeneral "[Display]`nfoo=1"
	Set-IniValue $noGeneral General sStartingConsoleCommand 'coc X'
	Check ((Get-Content $noGeneral)[0] -eq '[General]') 'adds [General] when missing'
} finally {
	$env:NMN_INGAME_SANDBOX = $previousSandbox
	$resolved = [IO.Path]::GetFullPath($sandbox)
	$allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected sandbox cleanup path' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

"`n$(if ($fails) { "$fails FAILED" } else { 'ALL KIT TESTS PASS' })"
exit $(if ($fails) { 1 } else { 0 })
