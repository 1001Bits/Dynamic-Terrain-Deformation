<#
.SYNOPSIS
  Puts staged mods into a runtime's Data folder for one of this kit's sessions, and takes
  them out again afterwards.

.DESCRIPTION
  The installs are shared with other projects, so a test mod is only ever present while
  this kit holds the install's lock. A staged mod is a folder under .state\mods\<Name>
  laid out like Data (plugins at its root, Textures\..., Meshes\...); -ModsRoot points at
  another folder of staged mods (D:\NMN Test Mods\staged on this machine, as C: is short of
  space). Deploying puts each file into Data - a hard link on the same drive, otherwise a
  copy while that drive keeps 2 GB free (the game does not load plugins through symbolic
  links), and a symbolic link only for non-plugin files past that, with a warning - and
  refuses to start if any of them already exists there, so nothing of anyone else's is
  ever replaced. Its .esp/.esm/.esl files are added to Plugins.txt, whose bytes are kept. Everything is recorded in a
  manifest; -Remove deletes exactly the files it placed, the folders it created (when
  empty) and puts Plugins.txt back.

  -Disable takes another mod out of the session, for a person who asked for that: each
  entry is a path under Data. A plugin at the Data root (.esm/.esp/.esl) is unticked in
  Plugins.txt; any other file (an F4SE .dll) is moved aside to .state\set-aside-<Runtime>.
  -Remove moves those files back and re-ticks the plugins.

.EXAMPLE
  .\Deploy-TestMods.ps1 -Runtime AE -Mod WinterOverhaul
  .\Deploy-TestMods.ps1 -Runtime OG -Disable 'Some Mod.esm', 'F4SE\Plugins\SomeMod.dll'
  .\Deploy-TestMods.ps1 -Runtime AE -Remove
#>
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE')][string]$Runtime,
	[string[]]$Mod = @(),
	[string]$ModsRoot = '',
	[string[]]$Disable = @(),
	[string]$Session = '',
	[switch]$Remove
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\IngameCommon.ps1"
$r = Get-Runtime $Runtime
$data = Join-Path $r.Game 'Data'
$modsDir = if ($ModsRoot) { $ModsRoot } else { Join-Path $script:StateDir 'mods' }
$manifestPath = Join-Path $script:StateDir "test-mods-$Runtime.json"
$localAppData = if ($script:Sandbox) { Join-Path $script:Sandbox 'LocalAppData' } else { $env:LOCALAPPDATA }
$pluginsTxt = Join-Path $localAppData "$($r.MyGames)\Plugins.txt"
$pluginExtensions = @('.esp', '.esm', '.esl')

# The masters a plugin names in its TES4 header (MAST subrecords), so a patch is enabled after
# the plugin it patches. An unreadable header names none.
function Get-PluginMasters([string]$Path) {
	try {
		$stream = [IO.File]::OpenRead($Path)
		try {
			$reader = [IO.BinaryReader]::new($stream)
			if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'TES4') { return @() }
			$size = $reader.ReadUInt32()
			$null = $reader.ReadBytes(16)
			$data = $reader.ReadBytes([int][Math]::Min($size, 1MB))
		} finally { $stream.Dispose() }
		$names = [Collections.Generic.List[string]]::new()
		$at = 0
		while ($at + 6 -le $data.Length) {
			$type = [Text.Encoding]::ASCII.GetString($data, $at, 4)
			$length = [BitConverter]::ToUInt16($data, $at + 4)
			if ($type -eq 'MAST' -and $at + 6 + $length -le $data.Length -and $length -gt 1) {
				$names.Add([Text.Encoding]::GetEncoding(1252).GetString($data, $at + 6, $length - 1))
			}
			$at += 6 + $length
		}
		return $names.ToArray()
	} catch {
		return @()
	}
}
$asideDir = Join-Path $script:StateDir "set-aside-$Runtime"

if ($Remove) {
	if (-not (Test-Path -LiteralPath $manifestPath)) { "No test mods are deployed to $($r.Name)."; return }
	$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
	if ($manifest.runtime -ne $Runtime -or $manifest.game -ne $r.Game) { throw 'Test-mod manifest targets a different runtime/install.' }
	if ($Session -and (-not $manifest.PSObject.Properties['session'] -or $manifest.session -ne $Session)) {
		throw 'Test mods belong to a different session; refusing restoration.'
	}
	if (Get-GameProcesses) { throw 'A game is running; close it before restoring test mods.' }
	# Preflight recovery before deleting anything. Changed files and occupied restore
	# paths belong to their current writer; retain the manifest/backups for recovery.
	foreach ($relative in @($manifest.files)) {
		$target = Get-ContainedPath $data $relative
		if (Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue) {
			$hashes = $manifest.PSObject.Properties['fileHashes']
			$expected = if ($hashes) { $hashes.Value.PSObject.Properties[$relative] } else { $null }
			if (-not $expected -or (Get-FileHash -LiteralPath $target).Hash -ne $expected.Value) {
				throw "Test-mod file changed or lacks an ownership hash: $target. Recovery manifest retained."
			}
		}
	}
	foreach ($relative in @($manifest.PSObject.Properties['setAside'] ? $manifest.setAside : @())) {
		$held = Get-ContainedPath $asideDir $relative
		$target = Get-ContainedPath $data $relative
		if ((Test-Path -LiteralPath $held) -and (Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue)) {
			throw "Cannot restore $relative because its target is occupied. Recovery manifest retained."
		}
	}
	foreach ($relative in @($manifest.createdDirectories)) { $null = Get-ContainedPath $data $relative }
	$removed = 0
	foreach ($relative in @($manifest.files)) {
		$target = Get-ContainedPath $data $relative
		# Get-Item also finds a symbolic link whose target has gone, which Test-Path does not.
		if (Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue) { Remove-Item -LiteralPath $target -Force; $removed++ }
	}
	# Deepest first, and only folders this deploy created that are now empty.
	foreach ($relative in @($manifest.createdDirectories | Sort-Object { $_.Length } -Descending)) {
		$folder = Get-ContainedPath $data $relative
		if ((Test-Path -LiteralPath $folder) -and -not (Get-ChildItem -LiteralPath $folder -Force | Select-Object -First 1)) {
			Remove-Item -LiteralPath $folder -Force
		}
	}
	$restored = 0
	foreach ($relative in @($manifest.PSObject.Properties['setAside'] ? $manifest.setAside : @())) {
		$held = Get-ContainedPath $asideDir $relative
		$target = Get-ContainedPath $data $relative
		if (-not (Test-Path -LiteralPath $held)) { continue }  # recorded, but the move never happened
		if (Test-Path -LiteralPath $target) {
			"WARNING: $relative is back in Data already; the set-aside copy stays in $held"
		} else {
			Move-Item -LiteralPath $held -Destination $target; $restored++
		}
	}
	if (Test-Path -LiteralPath $asideDir) {
		Get-ChildItem -LiteralPath $asideDir -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
			Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1) } |
			ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
		if (-not (Get-ChildItem -LiteralPath $asideDir -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $asideDir -Force }
	}
	if ($restored) { "Moved $restored set-aside file(s) back into $($r.Name) Data." }
	if ($manifest.pluginsTxtBackup) {
		$current = if (Test-Path -LiteralPath $pluginsTxt) { [IO.File]::ReadAllText($pluginsTxt) } else { $null }
		if ($current -eq $manifest.pluginsTxtWritten) {
			if ($manifest.pluginsTxtExisted) {
				Copy-Item -LiteralPath $manifest.pluginsTxtBackup -Destination $pluginsTxt -Force
			} else {
				Remove-Item -LiteralPath $pluginsTxt -Force
			}
			'Plugins.txt restored'
		} else {
			# Someone changed it since: take out only the lines this deploy added, and tick
			# again what it unticked.
			$added = @($manifest.pluginsAdded)
			$unticked = @($manifest.PSObject.Properties['pluginsDeactivated'] ? $manifest.pluginsDeactivated : @())
			$linesNow = if (Test-Path -LiteralPath $pluginsTxt) { @(Get-Content -LiteralPath $pluginsTxt) } else { @() }
			$kept = @($linesNow | Where-Object { $added -notcontains $_ } |
				ForEach-Object { if ($unticked -contains $_) { "*$_" } else { $_ } })
			Set-Content -LiteralPath $pluginsTxt -Value $kept -Encoding utf8NoBOM
			"WARNING: Plugins.txt changed during the session; undid only this deploy's lines ($((@($added) + @($unticked)) -join ', '))"
		}
	}
	Remove-Item -LiteralPath $manifestPath -Force
	"Removed $removed test-mod file(s) ($(@($manifest.mods) -join ', ')) from $($r.Name)."
	return
}

if (-not $Mod.Count -and -not $Disable.Count) { throw 'Give -Mod <name> (a folder under .state\mods), -Disable <path under Data> or -Remove.' }
Assert-LockHeld $r
if (Get-GameProcesses) { throw 'A game is running; deploy test mods before launching.' }
if (Test-Path -LiteralPath $manifestPath) {
	throw "Test mods are already deployed to $($r.Name) ($manifestPath); remove them first."
}

# Plan everything before touching the install: every file, and every clash.
$plan = [System.Collections.Generic.List[object]]::new()
foreach ($name in $Mod) {
	$source = Get-ContainedPath $modsDir $name
	if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "No staged mod at $source." }
	foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File -Force) {
		$relative = [IO.Path]::GetRelativePath($source, $file.FullName)
		$null = Get-ContainedPath $data $relative
		$plugin = $relative -notmatch '[\\/]' -and $pluginExtensions -contains [IO.Path]::GetExtension($relative).ToLowerInvariant()
		$plan.Add([pscustomobject]@{ Mod = $name; Source = $file.FullName; Relative = $relative; Plugin = $plugin })
	}
}
$clashes = @($plan | Where-Object { Get-Item -LiteralPath (Get-ContainedPath $data $_.Relative) -Force -ErrorAction SilentlyContinue })
if ($clashes.Count) {
	throw ("{0} file(s) already exist in {1}, e.g. {2}; not replacing anyone's files." -f
		$clashes.Count, $data, (($clashes | Select-Object -First 3).Relative -join ', '))
}
$duplicates = @($plan | Group-Object { $_.Relative.ToLowerInvariant() } | Where-Object Count -gt 1)
if ($duplicates.Count) { throw "Two staged mods both hold $($duplicates[0].Name)." }

# What -Disable takes out: root plugins are unticked, other files moved aside.
$untick = [System.Collections.Generic.List[string]]::new()
$setAside = [System.Collections.Generic.List[string]]::new()
foreach ($entry in $Disable) {
	$relative = $entry.Trim()
	$target = Get-ContainedPath $data $relative
	$held = Get-ContainedPath $asideDir $relative
	if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Nothing to disable at $target." }
	if ($relative -notmatch '[\\/]' -and $pluginExtensions -contains [IO.Path]::GetExtension($relative).ToLowerInvariant()) {
		$untick.Add($relative)
	} else {
		if (Test-Path -LiteralPath (Join-Path $asideDir $relative)) { throw "$(Join-Path $asideDir $relative) already exists; restore it first." }
		$setAside.Add($relative)
	}
}

# How each file gets into Data. A hard link (same drive only) and a copy are real files to
# the game. A symbolic link is not: the game reads the link's own directory entry and does
# not load a plugin through one (mod managers do not deploy Bethesda games that way), so
# from another drive everything is copied while that drive keeps 2 GB free, and only past
# that are files other than plugins linked (Developer Mode or elevation), with a warning.
$sameDrive = -not $plan.Count -or [IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $modsDir).Path) -eq [IO.Path]::GetPathRoot($data)
$linkRest = $false
if (-not $sameDrive) {
	$sizeOf = { param($items) [double](($items | ForEach-Object { (Get-Item -LiteralPath $_.Source).Length } | Measure-Object -Sum).Sum) }
	$bytes = & $sizeOf $plan
	$free = (Get-PSDrive -Name $data.Substring(0, 1)).Free
	if ($free - $bytes -lt 2GB) {
		$pluginBytes = & $sizeOf @($plan | Where-Object Plugin)
		New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
		$probe = Join-Path $script:StateDir "symlink-probe-$PID"
		try { New-Item -ItemType SymbolicLink -Path $probe -Target $plan[0].Source -ErrorAction Stop | Out-Null; $linkRest = $true }
		catch { $linkRest = $false }
		finally { Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue }
		if (-not $linkRest -or $free - $pluginBytes -lt 2GB) {
			throw ("The staged mods are on another drive and copying {0:N1} GB would leave {1:N1} GB free on {2}." -f
				($bytes / 1GB), (($free - $bytes) / 1GB), [IO.Path]::GetPathRoot($data))
		}
		"WARNING: copying {0:N1} GB would leave under 2 GB free on {1}; plugins are copied, the rest symbolically linked, which the game may not read." -f ($bytes / 1GB), [IO.Path]::GetPathRoot($data)
	}
}

$manifest = [ordered]@{
	runtime = $Runtime; game = $r.Game; mods = $Mod; session = $Session; deployed = (Get-Date).ToString('o')
	files = [System.Collections.Generic.List[string]]::new()
	fileHashes = @{}
	createdDirectories = [System.Collections.Generic.List[string]]::new()
	pluginsAdded = @(); pluginsTxtExisted = $false; pluginsTxtBackup = $null; pluginsTxtWritten = $null
	setAside = [System.Collections.Generic.List[string]]::new(); pluginsDeactivated = @()
}
New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
$save = { Write-AtomicJson $manifestPath $manifest }
# Record the complete intended file set durably before the first copy. An
# interruption at any file count is recoverable, including the first 499 files.
foreach ($item in $plan) {
	$manifest.files.Add($item.Relative)
	$manifest.fileHashes[$item.Relative] = (Get-FileHash -LiteralPath $item.Source).Hash
}
& $save

$linked = 0
$copied = 0
$symlinked = 0
foreach ($item in $plan) {
	$target = Get-ContainedPath $data $item.Relative
	$folder = Split-Path $target
	$missing = @()
	for ($walk = $folder; $walk -and -not (Test-Path -LiteralPath $walk); $walk = Split-Path $walk) { $missing += $walk }
	foreach ($created in ($missing | Sort-Object { $_.Length })) {
		$manifest.createdDirectories.Add([IO.Path]::GetRelativePath($data, $created))
		& $save
		New-Item -ItemType Directory -Path $created | Out-Null
	}
	if (Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue) { throw "Deployment target appeared during staging: $target" }
	if ($sameDrive) {
		try { New-Item -ItemType HardLink -Path $target -Target $item.Source | Out-Null; $linked++ }
		catch { Copy-Item -LiteralPath $item.Source -Destination $target; $copied++ }
	} elseif ($linkRest -and -not $item.Plugin) {
		New-Item -ItemType SymbolicLink -Path $target -Target $item.Source | Out-Null; $symlinked++
	} else {
		Copy-Item -LiteralPath $item.Source -Destination $target; $copied++
	}
}
foreach ($relative in $setAside) {
	$held = Join-Path $asideDir $relative
	New-Item -ItemType Directory -Force -Path (Split-Path $held) | Out-Null
	# Recorded before the move, so an interrupted deploy still puts it back.
	$manifest.setAside.Add($relative); & $save
	Move-Item -LiteralPath (Join-Path $data $relative) -Destination $held
}
& $save

$plugins = @($plan | Where-Object { $_.Relative -notmatch '[\\/]' -and $pluginExtensions -contains [IO.Path]::GetExtension($_.Relative).ToLowerInvariant() } |
	ForEach-Object { $_.Relative })
if ($plugins.Count -or $untick.Count) {
	$manifest.pluginsTxtExisted = Test-Path -LiteralPath $pluginsTxt
	if ($manifest.pluginsTxtExisted) {
		$backup = Join-Path $script:StateDir "Plugins.txt.$Runtime.before-test-mods"
		Copy-Item -LiteralPath $pluginsTxt -Destination $backup -Force
		$manifest.pluginsTxtBackup = $backup
	} else {
		New-Item -ItemType Directory -Force -Path (Split-Path $pluginsTxt) | Out-Null
		$manifest.pluginsTxtBackup = '(none)'
	}
	$lines = if ($manifest.pluginsTxtExisted) { @(Get-Content -LiteralPath $pluginsTxt) } else { @() }
	# .esm, then .esl, then .esp (a mod's .esl patch after its .esm); within each, a plugin
	# after the masters it names among these (a DLC patch after its mod), then by name.
	# '*' enables it.
	$rank = @{ '.esm' = 0; '.esl' = 1; '.esp' = 2 }
	$sources = @{}
	foreach ($item in $plan) { if ($plugins -contains $item.Relative) { $sources[$item.Relative] = $item.Source } }
	$masters = @{}
	foreach ($plugin in $plugins) { $masters[$plugin] = @(Get-PluginMasters $sources[$plugin] | Where-Object { $plugins -contains $_ }) }
	$depth = @{}
	$depthOf = {
		param([string]$Plugin, [int]$Guard)
		if ($depth.ContainsKey($Plugin)) { return $depth[$Plugin] }
		$deepest = 0
		if ($Guard -lt 32) { foreach ($master in $masters[$Plugin]) { $deepest = [Math]::Max($deepest, 1 + (& $depthOf $master ($Guard + 1))) } }
		$depth[$Plugin] = $deepest
		return $deepest
	}
	$ordered = @($plugins | Sort-Object { $rank[[IO.Path]::GetExtension($_).ToLowerInvariant()] }, { & $depthOf $_ 0 }, { $_ })
	$added = @($ordered | ForEach-Object { "*$_" } | Where-Object { $lines -notcontains $_ })
	$unticked = @($lines | Where-Object { $_.StartsWith('*') -and $untick -contains $_.Substring(1) } | ForEach-Object { $_.Substring(1) })
	$lines = @($lines | ForEach-Object { if ($_.StartsWith('*') -and $unticked -contains $_.Substring(1)) { $_.Substring(1) } else { $_ } })
	$manifest.pluginsAdded = $added
	$manifest.pluginsDeactivated = $unticked
	$manifest.pluginsTxtWritten = ((@($lines) + $added | ForEach-Object { "$_$([Environment]::NewLine)" }) -join '')
	& $save
	[IO.File]::WriteAllText($pluginsTxt, $manifest.pluginsTxtWritten, [Text.UTF8Encoding]::new($false))
}
if ($plan.Count) {
	"Deployed $($plan.Count) file(s) of $($Mod -join ', ') to $($r.Name) ($linked hard-linked, $copied copied, $symlinked symbolic links); plugins enabled: $(if ($plugins.Count) { $plugins -join ', ' } else { 'none' })."
}
if ($Disable.Count) {
	"Disabled for the session: $((@($untick | ForEach-Object { "$_ (unticked)" }) + @($setAside | ForEach-Object { "$_ (moved aside)" })) -join ', ')."
}
