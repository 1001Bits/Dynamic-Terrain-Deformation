<#
.SYNOPSIS
  Puts this build of the plugin into a game install, or takes it out again.

.DESCRIPTION
  Copies build\src\NMN_DeformableTerrain.dll (+ .pdb for crash logs), the shipped INI with
  an optional test preset appended, the footprint textures and the in-game menu (MCM\Config)
  into the runtime's Data folder. Whatever it would overwrite is backed up first and a
  manifest records what it did, so -Remove puts the install back exactly as it was. Other
  plugins are never touched. Choices saved in the menu (MCM\Settings) win over the INI, so an
  earlier session's file is set aside for the session (the preset applies as written) and put
  back afterwards; the session's own choices are kept in the state folder.

  The install must be locked by this kit (Invoke-SmokeTest.ps1 does that, or pass
  -ClaimLock here for a manual session and -Remove -ReleaseLock afterwards).

.EXAMPLE
  .\Deploy-Plugin.ps1 -Runtime OG -Preset smoke -ClaimLock      # manual session
  .\Deploy-Plugin.ps1 -Runtime OG -Remove -ReleaseLock           # when done
#>
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR', 'NG')][string]$Runtime,
	[string]$Preset = 'smoke',
	[switch]$Remove,
	[switch]$ClaimLock,
	[switch]$ReleaseLock,
	[switch]$TestDriver,
	[switch]$PrivateInputGuard,
	[switch]$DrawDiagnostics,
	# Owned sessions deliberately edit this INI for live phases/manual tuning. Preserve
	# those bytes in the state folder before restoring the user's pre-session file.
	[switch]$RestoreEditedIni,
	[string]$Session = '',
	[int]$LockMinutes = 60
)

. "$PSScriptRoot\IngameCommon.ps1"
$r = Get-Runtime $Runtime
$manifestPath = Join-Path $script:StateDir "deploy-$Runtime.json"

if ($Remove) {
	if (-not (Test-Path $manifestPath)) {
		Write-Output "Nothing deployed by this kit to $($r.Name)."
	} else {
		# Only this install's game holds the files; another runtime's game does not.
		$own = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($r.Exe)) -ErrorAction SilentlyContinue |
			Where-Object { $_.Path -eq (Join-Path $r.Game $r.Exe) })
		if ($own) { throw 'The game is still running; close it before removing the plugin.' }
		$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json -AsHashtable
		if ($manifest['runtime'] -ne $Runtime) { throw 'Deployment manifest targets a different runtime.' }
		if ($Session -and $manifest['session'] -ne $Session) { throw 'Deployment belongs to a different session; refusing restoration.' }
		# Validate all paths/content before making restoration changes. A failed copy can
		# leave the original in place; both that hash and the staged hash are ours.
		$dataRoot = Join-Path $r.Game 'Data'
		$edited = @()
		foreach ($entry in $manifest['files']) {
			$relative = [IO.Path]::GetRelativePath($dataRoot, $entry['target'])
			$null = Get-ContainedPath $dataRoot $relative
			if ($entry['backup']) {
				$null = Get-ContainedPath $script:StateDir ([IO.Path]::GetRelativePath($script:StateDir, $entry['backup']))
				if (-not (Test-Path -LiteralPath $entry['backup'])) { throw "Missing deployment backup $($entry['backup']); manifest retained." }
			}
			if (Test-Path -LiteralPath $entry['target']) {
				$current = (Get-FileHash -LiteralPath $entry['target']).Hash
				$original = if ($entry['backup']) { (Get-FileHash -LiteralPath $entry['backup']).Hash } else { $null }
				if (-not $entry['sha256'] -or ($current -ne $entry['sha256'] -and $current -ne $original)) {
					if ($RestoreEditedIni -and $entry['sha256'] -and $entry['target'] -eq (Join-Path $r.Plugins "$script:PluginName.ini")) {
						$edited += $entry
					} else {
						throw "Deployment target changed or lacks an ownership hash: $($entry['target']). Manifest retained."
					}
				}
			}
		}
		foreach ($dir in $manifest['createdDirs']) {
			$null = Get-ContainedPath $dataRoot ([IO.Path]::GetRelativePath($dataRoot, $dir))
		}
		foreach ($entry in $edited) {
			$saved = Join-Path $script:StateDir ("session-edited-$Runtime-" + [guid]::NewGuid().ToString('N') + '.ini')
			Copy-Item -LiteralPath $entry['target'] -Destination $saved
			$entry['sessionEditedBackup'] = $saved
			$entry['sha256'] = (Get-FileHash -LiteralPath $saved).Hash
			Write-Output "Preserved edited session INI at $saved"
		}
		if ($edited.Count) { Write-AtomicJson $manifestPath $manifest }
		foreach ($entry in $manifest['files']) {
			if (Test-Path -LiteralPath $entry['target']) { Remove-Item -LiteralPath $entry['target'] -Force }
			if ($entry['backup']) { Copy-Item -LiteralPath $entry['backup'] -Destination $entry['target'] -Force }
		}
		$choices = $manifest['mcmChoices']
		if ($choices) {
			$null = Get-ContainedPath $dataRoot ([IO.Path]::GetRelativePath($dataRoot, $choices['target']))
			if (Test-Path -LiteralPath $choices['target']) {
				$saved = Join-Path $script:StateDir ("session-mcm-$Runtime-" + [guid]::NewGuid().ToString('N') + '.ini')
				Copy-Item -LiteralPath $choices['target'] -Destination $saved
				Remove-Item -LiteralPath $choices['target'] -Force
				Write-Output "Preserved the session's menu choices at $saved"
			}
			if ($choices['backup']) {
				$null = Get-ContainedPath $script:StateDir ([IO.Path]::GetRelativePath($script:StateDir, $choices['backup']))
				Copy-Item -LiteralPath $choices['backup'] -Destination $choices['target'] -Force
			}
		}
		foreach ($dir in $manifest['createdDirs']) {
			if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force)) { Remove-Item -LiteralPath $dir }
		}
		Remove-Item $manifestPath -Force
		Write-Output "Removed the plugin from $($r.Name); the previous files are back."
	}
	if ($ReleaseLock) { Exit-GameLock; Write-Output 'Lock released.' }
	return
}

if ($ClaimLock) {
	$lease = Enter-GameLock $r -Minutes $LockMinutes -Scope "Manual NMN Deformable Terrain test on $($r.Name)"
	Write-Output "Claimed the $($r.Name) lock until $($lease.expires)."
}
Assert-LockHeld $r
if (Get-GameProcesses) { throw 'The game is running; deploy before launching.' }
if (Test-Path $manifestPath) { throw "Already deployed to $($r.Name) ($manifestPath); remove first." }

$art = Get-BuildArtifacts
if ($PrivateInputGuard) {
	$guard = Join-Path $script:RepoRoot 'build\tools\ingame\driver\NMN_PrivateInputGuard.dll'
	if (-not (Test-Path -LiteralPath $guard)) { throw 'Build NMN_PrivateInputGuard before launching a private game.' }
}
if ($TestDriver) {
	$driver = Join-Path $script:RepoRoot 'build\tools\ingame\driver\NMN_TerrainTestDriver.dll'
	if (-not (Test-Path -LiteralPath $driver)) { throw 'Build with -DBUILD_INGAME_DRIVER=ON before requesting -TestDriver.' }
}
if ($art.Dirty) { Write-Warning 'src/ or data/ has uncommitted changes; the build may not match a commit.' }

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
$backupDir = Join-Path $script:StateDir "backup-$Runtime-$stamp"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null

$staged = Join-Path $backupDir 'staged.ini'
New-PresetIni $Preset $staged
if ($DrawDiagnostics) { Add-Content -LiteralPath $staged -Value "`nLogDraws=1" -Encoding utf8NoBOM }

$copies = @(
	@{ source = $art.Dll; target = Join-Path $r.Plugins "$script:PluginName.dll" }
	@{ source = $staged;  target = Join-Path $r.Plugins "$script:PluginName.ini" }
)
if ($TestDriver) { $copies += @{ source = $driver; target = Join-Path $r.Plugins 'NMN_TerrainTestDriver.dll' } }
if ($PrivateInputGuard) { $copies += @{ source = $guard; target = Join-Path $r.Plugins 'NMN_PrivateInputGuard.dll' } }
if (Test-Path $art.Pdb) { $copies += @{ source = $art.Pdb; target = Join-Path $r.Plugins "$script:PluginName.pdb" } }
foreach ($texture in Get-ChildItem -LiteralPath $art.Textures -File) {
	$copies += @{ source = $texture.FullName; target = Join-Path $r.Textures $texture.Name }
}
# The in-game menu (Mod Configuration Menu): its layout and defaults.
$menuSource = Join-Path $script:RepoRoot "data\Data\MCM\Config\$script:PluginName"
$menuTarget = Join-Path $r.Game "Data\MCM\Config\$script:PluginName"
$menuDirs = @()
if (Test-Path -LiteralPath $menuSource) {
	foreach ($file in Get-ChildItem -LiteralPath $menuSource -File) {
		$copies += @{ source = $file.FullName; target = Join-Path $menuTarget $file.Name }
	}
	$menuDirs = @($menuTarget, (Split-Path $menuTarget), (Split-Path (Split-Path $menuTarget)))
}
$choicesTarget = Join-Path $r.Game "Data\MCM\Settings\$script:PluginName.ini"
$null = Get-ContainedPath (Join-Path $r.Game 'Data') ([IO.Path]::GetRelativePath((Join-Path $r.Game 'Data'), $choicesTarget))

# Deepest first, so -Remove takes away an emptied child before its parent.
$createdDirs = @()
foreach ($dir in @($r.Plugins, $r.Textures) + $menuDirs) {
	$null = Get-ContainedPath (Join-Path $r.Game 'Data') ([IO.Path]::GetRelativePath((Join-Path $r.Game 'Data'), $dir))
	if (-not (Test-Path -LiteralPath $dir)) { $createdDirs += $dir }
}

$files = @()
foreach ($copy in $copies) {
	$backup = $null
	$targetItem = Get-Item -LiteralPath $copy.target -Force -ErrorAction SilentlyContinue
	if ($targetItem -and ($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
		throw "Deployment target is a link: $($copy.target)"
	}
	if (Test-Path -LiteralPath $copy.target) {
		$backup = Join-Path $backupDir ([IO.Path]::GetFileName($copy.target))
		Copy-Item -LiteralPath $copy.target -Destination $backup -Force
	}
	$files += @{ target = $copy.target; backup = $backup; sha256 = (Get-FileHash -LiteralPath $copy.source).Hash }
}

$choicesBackup = $null
if (Test-Path -LiteralPath $choicesTarget) {
	$choicesBackup = Join-Path $backupDir 'mcm-choices.ini'
	Copy-Item -LiteralPath $choicesTarget -Destination $choicesBackup -Force
}

$manifest = [ordered]@{
	runtime     = $Runtime
	session     = $Session
	preset      = $Preset
	deployed    = (Get-Date).ToString('o')
	commit      = $art.Commit
	dllSha256   = $art.Sha256
	files       = $files
	createdDirs = $createdDirs
	mcmChoices  = @{ target = $choicesTarget; backup = $choicesBackup }
}
Write-AtomicJson $manifestPath $manifest
# The complete recovery record and backups precede the first install mutation.
foreach ($dir in $createdDirs) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
foreach ($copy in $copies) { Copy-Item -LiteralPath $copy.source -Destination $copy.target -Force }
if ($choicesBackup) { Remove-Item -LiteralPath $choicesTarget -Force }
Write-Output "Deployed $($art.Commit) (dll $($art.Sha256.Substring(0, 12))) to $($r.Name) with preset '$Preset'."
