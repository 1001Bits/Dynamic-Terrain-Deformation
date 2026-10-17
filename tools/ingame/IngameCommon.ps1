# Shared helpers for the in-game test kit (PowerShell 7). Dot-source it:  . "$PSScriptRoot\IngameCommon.ps1"
#
# The game installs on this machine are shared with other agents that run unattended tests.
# Nothing here touches an install without holding its lock:
#   - flat Fallout 4: C:\Games\GAME_LOCK.json, a reservation that blocks while its status is
#     "reserved"/"active" and its "expires" (ISO 8601 with offset) is in the future. Older
#     agents write {"owner","game","started"} instead: such a file blocks for 30 minutes from
#     "started". An unreadable file always blocks.
#   - Fallout 4 VR / SteamVR: %TEMP%\steamvr-null-in-use.txt, one line "<session> <pid> <start>";
#     stale only once that PID is gone. No vrserver/vrcompositor/Fallout4VR may be running either.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:StateDir   = Join-Path $PSScriptRoot '.state'
$script:LeaseFile  = Join-Path $script:StateDir 'lease.json'
$script:GameLock   = 'C:\Games\GAME_LOCK.json'
$script:PriorityLock = 'C:\Development\Realistic Reflections FO4 - Glass\.tmp\launch-coordination\priority-window.json'
$script:VRLock     = Join-Path $env:TEMP 'steamvr-null-in-use.txt'
$script:PluginName = 'NMN_DeformableTerrain'
$script:Documents  = [Environment]::GetFolderPath('MyDocuments')

# For testing the kit itself: a folder standing in for C:\Games, the locks, the state folder
# and Documents. Runtime installs are <sandbox>\<OG|AE|VR> and need no real executable.
$script:Sandbox = $env:NMN_INGAME_SANDBOX
if ($script:Sandbox) {
	$script:GameLock  = Join-Path $script:Sandbox 'GAME_LOCK.json'
	$script:PriorityLock = Join-Path $script:Sandbox 'priority-window.json'
	$script:VRLock    = Join-Path $script:Sandbox 'steamvr-null-in-use.txt'
	$script:StateDir  = Join-Path $script:Sandbox '.state'
	$script:LeaseFile = Join-Path $script:StateDir 'lease.json'
	$script:Documents = Join-Path $script:Sandbox 'Documents'
}

$script:Runtimes = @{
	OG = @{
		Name = 'Fallout 4 1.10.163'; Game = 'C:\Games\Fallout.4 1.10.163'; Exe = 'Fallout4.exe'
		Loader = 'f4se_loader.exe'; Version = '1.10.163.0'; MyGames = 'Fallout4'; VR = $false
	}
	AE = @{
		Name = 'Fallout 4 1.11.240'; Game = 'C:\Games\Steam\steamapps\common\Fallout 4'; Exe = 'Fallout4.exe'
		Loader = 'f4se_loader.exe'; Version = '1.11.240.0'; MyGames = 'Fallout4'; VR = $false
	}
	VR = @{
		Name = 'Fallout 4 VR 1.2.72'; Game = 'C:\Games\Steam\steamapps\common\Fallout 4 VR'; Exe = 'Fallout4VR.exe'
		Loader = 'f4sevr_loader.exe'; Version = '1.2.72.0'; MyGames = 'Fallout4VR'; VR = $true
	}
}

function Get-Runtime([string]$Key) {
	if ($Key -eq 'NG') {
		throw 'Fallout 4 1.10.984 (NG) is not installed on this machine; it cannot be tested here.'
	}
	if (-not $script:Runtimes.ContainsKey($Key)) { throw "Unknown runtime '$Key' (OG, AE or VR)." }
	$r = $script:Runtimes[$Key].Clone()
	$r.Key        = $Key
	if ($script:Sandbox) { $r.Game = Join-Path $script:Sandbox $Key }
	$r.Plugins    = Join-Path $r.Game 'Data\F4SE\Plugins'
	$r.Textures   = Join-Path $r.Game "Data\Textures\$script:PluginName"
	$r.LogDir     = Join-Path $script:Documents "My Games\$($r.MyGames)\F4SE"
	$r.PluginLog  = Join-Path $r.LogDir "$script:PluginName.log"
	$r.CustomIni  = Join-Path $script:Documents "My Games\$($r.MyGames)\Fallout4Custom.ini"
	if ($script:Sandbox) { return $r }
	$exe = Join-Path $r.Game $r.Exe
	if (-not (Test-Path $exe)) { throw "$($r.Name): $exe is missing." }
	$found = (Get-Item $exe).VersionInfo.FileVersion
	if ($found -ne $r.Version) { throw "$($r.Name): $exe is version $found, expected $($r.Version)." }
	return $r
}

# Every deployment/recovery manifest path must stay under its intended root. Reject
# directory junctions as well as '..' paths: a lexical prefix alone is not sufficient.
function Get-ContainedPath([string]$Root, [string]$Relative) {
	if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative)) {
		throw "Expected a relative path under $Root, got '$Relative'."
	}
	$base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
	$target = [IO.Path]::GetFullPath((Join-Path $base $Relative))
	if (-not $target.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
		throw "Path escapes $Root`: $Relative"
	}
	for ($walk = Split-Path $target; $walk.Length -ge $base.Length; $walk = Split-Path $walk) {
		$item = Get-Item -LiteralPath $walk -Force -ErrorAction SilentlyContinue
		if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
			throw "Directory link is not safe for deployment/recovery: $walk"
		}
		if ($walk -eq $base) { break }
	}
	return $target
}

function Get-BuildArtifacts {
	$build = Join-Path $script:RepoRoot 'build\src'
	$dll = Join-Path $build "$script:PluginName.dll"
	if (-not (Test-Path $dll)) { throw "No built plugin at $dll - run build.cmd first." }
	[pscustomobject]@{
		Dll      = $dll
		Pdb      = Join-Path $build "$script:PluginName.pdb"
		Ini      = Join-Path $script:RepoRoot "data\Data\F4SE\Plugins\$script:PluginName.ini"
		Textures = Join-Path $script:RepoRoot "data\Data\Textures\$script:PluginName"
		Sha256   = (Get-FileHash $dll -Algorithm SHA256).Hash
		Commit   = (git -C $script:RepoRoot rev-parse --short HEAD 2>$null)
		Dirty    = [bool](git -C $script:RepoRoot status --porcelain -- src data 2>$null)
	}
}

# ── locks ─────────────────────────────────────────────────────────────────────────────

function Test-PidAlive([int]$ProcessId) {
	if ($ProcessId -le 0) { return $false }
	return [bool](Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

# A timestamp from a lock file: ConvertFrom-Json already turns ISO 8601 strings into DateTime
# (local); anything else is parsed culture-independently.
function ConvertTo-Instant($Value) {
	if ($Value -is [DateTimeOffset]) { return $Value }
	if ($Value -is [DateTime]) { return [DateTimeOffset]$Value }
	return [DateTimeOffset]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture,
		[Globalization.DateTimeStyles]::AssumeUniversal)
}

function Get-FlatLockState([string]$Path = $script:GameLock) {
	$now = [DateTimeOffset]::UtcNow
	# A runner renews its lock by replacing the file; for a moment it can be missing. Only a
	# file that stays missing for a short while counts as no lock (a run once ended itself
	# on such a moment: 'GAME_LOCK.json is not ours any more').
	for ($look = 0; $look -lt 8 -and -not (Test-Path -LiteralPath $Path); $look++) { Start-Sleep -Milliseconds 25 }
	if (-not (Test-Path -LiteralPath $Path)) {
		return [pscustomobject]@{ Free = $true; Reason = 'no lock file'; Lease = $null; Ours = $false }
	}
	# A runner rewrites its lock every few seconds (atomic replace); a read can meet the
	# moment of the swap, so retry briefly before calling the file unreadable.
	$lease = $null
	for ($attempt = 1; ; $attempt++) {
		try {
			$lease = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
			break
		} catch {
			if ($attempt -ge 6 -or -not (Test-Path -LiteralPath $Path)) {
				if (-not (Test-Path -LiteralPath $Path)) {
					return [pscustomobject]@{ Free = $true; Reason = 'no lock file'; Lease = $null; Ours = $false }
				}
				return [pscustomobject]@{ Free = $false; Reason = "unreadable lock file: $($_.Exception.Message)"; Lease = $null; Ours = $false }
			}
			Start-Sleep -Milliseconds 50
		}
	}
	if ($lease -isnot [System.Collections.IDictionary]) {
		return [pscustomobject]@{ Free = $false; Reason = 'unreadable lock file: expected an object'; Lease = $null; Ours = $false }
	}
	$ours = $false
	if (Test-Path $script:LeaseFile) {
		$mine = Get-Content $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
		$ours = $lease.ContainsKey('claim_token') -and $lease['claim_token'] -eq $mine['claim_token']
	}
	if ($lease.ContainsKey('status')) {
		if ($lease['status'] -eq 'released') {
			return [pscustomobject]@{ Free = $true; Reason = 'released'; Lease = $lease; Ours = $ours }
		}
		try { $expires = ConvertTo-Instant $lease['expires'] } catch {
			return [pscustomobject]@{ Free = $false; Reason = 'reservation without a readable expiry'; Lease = $lease; Ours = $ours }
		}
		if ($now -ge $expires) {
			return [pscustomobject]@{ Free = $true; Reason = "expired $($expires.ToString('u'))"; Lease = $lease; Ours = $ours }
		}
		$who = if ($lease.ContainsKey('project')) { $lease['project'] } elseif ($lease.ContainsKey('id')) { $lease['id'] } else { 'unknown owner' }
		return [pscustomobject]@{ Free = $false; Reason = "$($lease['status']) by $who until $($expires.ToString('u'))"; Lease = $lease; Ours = $ours }
	}
	# The older {"owner","game","started"} form: blocks for 30 minutes.
	try { $started = ConvertTo-Instant $lease['started'] } catch {
		return [pscustomobject]@{ Free = $false; Reason = 'lock without status or a readable start time'; Lease = $lease; Ours = $false }
	}
	if ($now - $started -lt [TimeSpan]::FromMinutes(30)) {
		return [pscustomobject]@{ Free = $false; Reason = "held by $($lease['owner']) since $($started.ToString('u'))"; Lease = $lease; Ours = $false }
	}
	return [pscustomobject]@{ Free = $true; Reason = 'legacy lock older than 30 minutes'; Lease = $lease; Ours = $false }
}

function Get-VRLockState {
	$busy = @(Get-Process -Name vrserver, vrcompositor, Fallout4VR -ErrorAction SilentlyContinue)
	if ($busy.Count -gt 0) {
		return [pscustomobject]@{ Free = $false; Reason = 'running: ' + (($busy | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', ') }
	}
	if (Test-Path -LiteralPath $script:VRLock) {
		$line = (Get-Content -LiteralPath $script:VRLock -TotalCount 1)
		$parts = "$line" -split '\s+'
		$lockPid = 0
		if ($parts.Count -ge 2) { [void][int]::TryParse($parts[1], [ref]$lockPid) }
		if (Test-PidAlive $lockPid) {
			return [pscustomobject]@{ Free = $false; Reason = "steamvr lock held by '$line'" }
		}
		return [pscustomobject]@{ Free = $true; Reason = "stale steamvr lock '$line' (PID gone)" }
	}
	return [pscustomobject]@{ Free = $true; Reason = 'no steamvr lock' }
}

function Get-GameProcesses {
	@(Get-Process -Name Fallout4, Fallout4VR, f4se_loader, f4sevr_loader -ErrorAction SilentlyContinue)
}

# Query the documented native API: CIM/WMI may be denied in a restricted session
# even when read-only memory and process inspection are available.
function Get-FreePhysicalMemoryGB {
	if (-not ('NmnIngame.Memory' -as [type])) {
		Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace NmnIngame {
    public static class Memory {
        [StructLayout(LayoutKind.Sequential)]
        private struct Status {
            public uint Length, Load;
            public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile;
            public ulong TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GlobalMemoryStatusEx(ref Status status);
        public static double FreeGB() {
            var status = new Status { Length = (uint)Marshal.SizeOf<Status>() };
            if (!GlobalMemoryStatusEx(ref status))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return status.AvailablePhysical / 1073741824.0;
        }
        // What can still be committed system-wide (RAM plus page file): when it runs out,
        // allocations fail and a game plugin's std::bad_alloc ends the game.
        public static double CommitFreeGB() {
            var status = new Status { Length = (uint)Marshal.SizeOf<Status>() };
            if (!GlobalMemoryStatusEx(ref status))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return status.AvailablePageFile / 1073741824.0;
        }
    }
}
'@
	}
	return [NmnIngame.Memory]::FreeGB()
}

function Get-CommitFreeGB {
	[void](Get-FreePhysicalMemoryGB)  # compiles the helper type
	return [NmnIngame.Memory]::CommitFreeGB()
}

function Test-RuntimeFree($Runtime) {
	$running = @(Get-GameProcesses)
	if ($running.Count -gt 0) {
		return [pscustomobject]@{ Free = $false; Reason = 'game running: ' + (($running | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', ') }
	}
	# The kit has one lease file for all its runners. While another runner of this kit is
	# alive (it may still be undeploying or restoring after its game closed), its
	# reservations would read as ours: a second run must wait, not take them over.
	if (Test-Path $script:LeaseFile) {
		$mine = Get-Content $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
		$holder = [int]($mine['owner_pid'] ?? 0)
		if ($holder -and $holder -ne $PID -and (Test-PidAlive $holder)) {
			return [pscustomobject]@{ Free = $false; Reason = "this kit's $($mine['runtime']) run (PID $holder) is still finishing" }
		}
	}
	$priority = Get-FlatLockState $script:PriorityLock
	if (-not $priority.Free -and -not $priority.Ours) {
		return [pscustomobject]@{ Free = $false; Reason = "shared benchmark reservation: $($priority.Reason)" }
	}
	if ($Runtime.VR) { return Get-VRLockState }
	$state = Get-FlatLockState
	if ($state.Ours) { return [pscustomobject]@{ Free = $true; Reason = 'held by this kit' } }
	return $state
}

function New-ClaimToken {
	$bytes = [byte[]]::new(32)
	[Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
	-join ($bytes | ForEach-Object { $_.ToString('x2') })
}

function Write-AtomicJson([string]$Path, $Object) {
	$tmp = "$Path.$PID.tmp"
	$Object | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $tmp -Encoding utf8NoBOM
	# Another project's runner (or a scanner) can hold the target open for a moment; the
	# replace is retried briefly before that counts as a failure.
	for ($attempt = 1; ; $attempt++) {
		try { Move-Item -LiteralPath $tmp -Destination $Path -Force -ErrorAction Stop; return }
		catch {
			if ($attempt -ge 10) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue; throw }
			Start-Sleep -Milliseconds 200
		}
	}
}

# Claims the runtime's lock for $Minutes. Flat: a reservation in GAME_LOCK.json carrying
# both the current fields and the older owner/game/started ones. VR: the steamvr line.
function Enter-GameLock($Runtime, [int]$Minutes = 45, [string]$Scope = 'NMN Deformable Terrain in-game test') {
	New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
	$free = Test-RuntimeFree $Runtime
	if (-not $free.Free) { throw "$($Runtime.Name) is busy: $($free.Reason)" }

	$now = [DateTimeOffset]::UtcNow
	$token = New-ClaimToken
	$lease = [ordered]@{
		id                 = "nmn-dtd-$($Runtime.Key.ToLower())-$($now.ToString('yyyyMMddTHHmmssZ'))"
		status             = 'active'
		project            = 'Dynamic Terrain Deformation FO4'
		owner              = 'nmn-dtd in-game test kit'
		game               = $Runtime.Name
		owner_thread       = "nmn-dtd-ingame-kit"
		owner_pid          = $PID
		claim_token        = $token
		ownership_token    = $token
		created            = $now.ToString('o')
		started            = $now.ToString('o')
		expires            = $now.AddMinutes($Minutes).ToString('o')
		scope              = $Scope
		automatic_relaunch = $false
	}

	$lease.runtime = $Runtime.Key
	# The cross-project reservation prevents another private benchmark from launching
	# between our process checks. Claim it before the install-specific lock.
	# The cross-project reservation lives in another project's coordination folder: only
	# while that folder exists (a removed one is not made again).
	$claims = @(if (Test-Path -LiteralPath (Split-Path $script:PriorityLock)) { $script:PriorityLock }) +
		@(if (-not $Runtime.VR) { $script:GameLock })
	try {
		foreach ($path in $claims) {
			$again = Get-FlatLockState $path
			if (-not $again.Free -and -not $again.Ours) { throw "Lost the race for $path`: $($again.Reason)" }
			Write-AtomicJson $path $lease
			Start-Sleep -Milliseconds 300
			$check = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
			if ($check['claim_token'] -ne $token) { throw "Another session wrote $path at the same time; not proceeding." }
		}
		if ($Runtime.VR) {
			Set-Content -LiteralPath $script:VRLock -Value "nmn-dtd $PID $($now.ToString('o'))" -Encoding ascii
		}
		Write-AtomicJson $script:LeaseFile $lease
		return $lease
	} catch {
		foreach ($path in $claims) {
			$check = (Get-FlatLockState $path).Lease
			if ($check -and $check.ContainsKey('claim_token')) {
				if ($check['claim_token'] -eq $token) {
					$lease.status = 'released'
					$lease.expires = [DateTimeOffset]::UtcNow.ToString('o')
					Write-AtomicJson $path $lease
				}
			}
		}
		throw
	}
}

function Update-GameLock([int]$Minutes = 30) {
	if (-not (Test-Path $script:LeaseFile)) { return }
	$mine = Get-Content $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
	$paths = @($script:PriorityLock) + @(if ($mine['runtime'] -ne 'VR') { $script:GameLock })
	foreach ($path in $paths) {
		$state = Get-FlatLockState $path
		if (-not $state.Lease -and $state.Reason -eq 'no lock file') {
			# The file was deleted under a held reservation (a coordination folder cleaned
			# up): written again, or the next project would read the install as free.
			$lease = [ordered]@{}
			foreach ($key in $mine.Keys) { $lease[$key] = $mine[$key] }
			$lease['status'] = 'active'
			$lease['expires'] = [DateTimeOffset]::UtcNow.AddMinutes($Minutes).ToString('o')
			# Only where the coordination folder is still in use; a removed folder is not made again.
			if (Test-Path -LiteralPath (Split-Path $path)) { Write-AtomicJson $path $lease }
			continue
		}
		if (-not $state.Ours) { throw "The reservation $path is no longer ours; ending the run." }
		$lease = $state.Lease
		$lease['expires'] = [DateTimeOffset]::UtcNow.AddMinutes($Minutes).ToString('o')
		Write-AtomicJson $path $lease
	}
}

function Exit-GameLock {
	if (-not (Test-Path $script:LeaseFile)) { return }
	$mine = Get-Content $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
	if ($mine['runtime'] -eq 'VR') {
		if (Test-Path -LiteralPath $script:VRLock) {
			$line = Get-Content -LiteralPath $script:VRLock -TotalCount 1
			if ("$line" -like "nmn-dtd $($mine['owner_pid']) *") { Remove-Item -LiteralPath $script:VRLock -Force }
		}
	} else {
		$state = Get-FlatLockState
		if ($state.Ours) { Remove-Item -LiteralPath $script:GameLock -Force }
		else { Write-Warning 'The game lock is not ours any more; left it alone.' }
	}
	$priority = Get-FlatLockState $script:PriorityLock
	if ($priority.Ours) {
		$priority.Lease['status'] = 'released'
		$priority.Lease['expires'] = [DateTimeOffset]::UtcNow.ToString('o')
		Write-AtomicJson $script:PriorityLock $priority.Lease
	}
	Remove-Item -LiteralPath $script:LeaseFile -Force
}

function Assert-LockHeld($Runtime) {
	if (-not (Test-Path $script:LeaseFile)) { throw 'This kit holds no lock; run with -ClaimLock or Enter-GameLock first.' }
	$mine = Get-Content $script:LeaseFile -Raw | ConvertFrom-Json -AsHashtable
	if ($mine['runtime'] -ne $Runtime.Key) { throw "The kit's lock is for $($mine['runtime']), not $($Runtime.Key)." }
	if (-not $Runtime.VR -and -not (Get-FlatLockState).Ours) { throw 'GAME_LOCK.json is not ours any more.' }
	if ((Test-Path -LiteralPath (Split-Path $script:PriorityLock)) -and -not (Get-FlatLockState $script:PriorityLock).Ours) {
		throw 'The shared benchmark reservation is not ours any more.'
	}
}

# ── settings presets ──────────────────────────────────────────────────────────────────

# The shipped INI with the preset's lines appended: the plugin reads lines in order, so a
# later key wins, and section headers are only comments to it.
function New-PresetIni([string]$Preset, [string]$OutFile) {
	$art = Get-BuildArtifacts
	$text = Get-Content -LiteralPath $art.Ini -Raw
	if ($Preset -and $Preset -ne 'shipped') {
		$presetFile = Join-Path $PSScriptRoot "presets\$Preset.ini"
		if (-not (Test-Path $presetFile)) { throw "No preset '$Preset' in $PSScriptRoot\presets." }
		$text = $text.TrimEnd() + "`r`n`r`n[Test preset: $Preset]`r`n" + (Get-Content -LiteralPath $presetFile -Raw)
	}
	Set-Content -LiteralPath $OutFile -Value $text -Encoding utf8NoBOM -NoNewline
}

# Sets one key in an INI section (the game's custom INI), replacing any line for that key.
function Set-IniValue([string]$IniPath, [string]$Section, [string]$Key, [string]$Value) {
	$lines = if (Test-Path -LiteralPath $IniPath) { @(Get-Content -LiteralPath $IniPath) } else { @() }
	$lines = @($lines | Where-Object { $_ -notmatch ('(?i)^\s*' + [regex]::Escape($Key) + '\s*=') })
	$general = [Array]::FindIndex([string[]]$lines, [Predicate[string]] { param($l) $l -match ('(?i)^\s*\[' + [regex]::Escape($Section) + '\]\s*$') })
	$entry = "$Key=$Value"
	if ($general -ge 0) {
		$lines = @($lines[0..$general]) + $entry + @(if ($general + 1 -lt $lines.Count) { $lines[($general + 1)..($lines.Count - 1)] })
	} else {
		$lines = @("[$Section]", $entry) + $lines
	}
	Set-Content -LiteralPath $IniPath -Value $lines -Encoding utf8NoBOM
}
