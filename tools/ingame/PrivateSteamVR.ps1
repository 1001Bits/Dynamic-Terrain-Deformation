# Test-only null-headset session. Ownership and settings recovery are durable before
# mutation; an abandoned controller can be recovered without adopting foreign PIDs.
function Get-PrivateSteamVRPaths {
    if ($script:Sandbox) {
        return @{ root = Join-Path $script:Sandbox 'SteamVR'; settings = Join-Path $script:Sandbox 'Steam\config\steamvr.vrsettings' }
    }
    return @{ root = 'C:\Games\Steam\steamapps\common\SteamVR'; settings = 'C:\Games\Steam\config\steamvr.vrsettings' }
}

function Get-SteamVRProcessNames { @('vrstartup','vrserver','vrcompositor','vrmonitor','vrdashboard','vrwebhelper','vrprismhost') }

function Copy-PrivateSteamVRSettings([string]$Source, [string]$Destination) {
    # Stage beside the destination and replace atomically. Controller death cannot
    # leave half of our JSON in the shared settings file.
    $temporary = "$Destination.nmn-$([guid]::NewGuid().ToString('N')).tmp"
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary
        [IO.File]::Replace($temporary, $Destination, [NullString]::Value)
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Save-PrivateSteamVRSession($Session) {
    $records = @($Session.owned.Values | ForEach-Object {
        @{ pid = $_.process.Id; path = $_.path; startUtcTicks = $_.startUtcTicks; parentPid = $_.parentPid }
    })
    Write-AtomicJson $Session.journal @{
        version = 1; run = $Session.run; root = $Session.root; settings = $Session.settings
        backup = $Session.backup; originalHash = $Session.originalHash; writtenHash = $Session.writtenHash
        ownerPid = $Session.ownerPid; ownerStartUtcTicks = $Session.ownerStartUtcTicks
        startedUtcTicks = $Session.started.ToUniversalTime().Ticks; processes = $records
        settingsRestored = $Session.settingsRestored; completed = $Session.completed
    }
}

function Test-SteamVRIdentity($Process, $Owned) {
    try {
        return $Process -and $Process.Path -eq $Owned.path -and
            $Process.StartTime.ToUniversalTime().Ticks -eq [long]$Owned.startUtcTicks
    } catch { return $false }
}

function Add-PrivateSteamVRProcess($Session, $Process, [int]$ParentPid = 0) {
    if (-not $Process.Path -or -not $Process.Path.StartsWith($Session.root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'SteamVR process is outside the expected installation.'
    }
    $null = $Process.Handle # Retain the original process handle against PID reuse.
    $Session.owned[$Process.Id] = [pscustomobject]@{
        process = $Process; path = $Process.Path
        startUtcTicks = $Process.StartTime.ToUniversalTime().Ticks; parentPid = $ParentPid
    }
    Save-PrivateSteamVRSession $Session
}

function Start-PrivateSteamVR($Desktop, [string]$Run) {
    Assert-LockHeld (Get-Runtime VR)
    $names = @(Get-SteamVRProcessNames)
    if (@(Get-Process -Name $names -ErrorAction SilentlyContinue).Count) {
        throw 'Existing SteamVR session; refusing to change its settings or processes.'
    }
    $paths = Get-PrivateSteamVRPaths
    $Run = [IO.Path]::GetFullPath($Run)
    $journal = Join-Path $Run 'steamvr-session.json'
    if (Test-Path -LiteralPath $journal) { throw 'This run already has a SteamVR recovery record.' }
    $session = [pscustomobject]@{
        root = $paths.root; settings = $paths.settings; run = $Run; journal = $journal
        backup = Join-Path $Run 'steamvr.vrsettings.original'; started = Get-Date
        launcher = $null; owned = @{}; names = $names; originalHash = ''; writtenHash = ''
        ownerPid = $PID; ownerStartUtcTicks = [Diagnostics.Process]::GetCurrentProcess().StartTime.ToUniversalTime().Ticks
        settingsRestored = $false; completed = $false
    }
    Copy-Item -LiteralPath $session.settings -Destination $session.backup
    $session.originalHash = (Get-FileHash -LiteralPath $session.backup).Hash
    $settings = Get-Content -LiteralPath $session.backup -Raw | ConvertFrom-Json -AsHashtable
    foreach ($section in @('driver_null','steamvr')) {
        if (-not $settings.ContainsKey($section)) { $settings[$section] = @{} }
    }
    $settings.driver_null.enable = $true
    $settings.driver_null.renderWidth = 1512
    $settings.driver_null.renderHeight = 1680
    $settings.steamvr.forcedDriver = 'null'
    $settings.steamvr.activateMultipleDrivers = $false
    $settings.steamvr.enableHomeApp = $false
    $settings.steamvr.startDashboardFromAppLaunch = $false
    $settings.steamvr.showMirrorView = $false
    $settings.steamvr.showPerfGraph = $false
    $testSettings = Join-Path $Run 'steamvr.vrsettings.test'
    $settings | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $testSettings -Encoding utf8NoBOM
    $session.writtenHash = (Get-FileHash -LiteralPath $testSettings).Hash
    Save-PrivateSteamVRSession $session # Durable BEFORE touching shared settings/launching.
    try {
        Copy-PrivateSteamVRSettings $testSettings $session.settings
        $directory = Join-Path $session.root 'bin\win64'
        $session.launcher = $Desktop.Launch((Join-Path $directory 'vrstartup.exe'), $directory, [string[]]@())
        Add-PrivateSteamVRProcess $session $session.launcher
        $deadline = (Get-Date).AddSeconds(45)
        do {
            Update-PrivateSteamVR $session
            $liveNames = @($session.owned.Values | ForEach-Object process | Where-Object { -not $_.HasExited } | ForEach-Object ProcessName)
            if ('vrserver' -in $liveNames -and 'vrcompositor' -in $liveNames) { return $session }
            Start-Sleep -Seconds 1
        } while ((Get-Date) -lt $deadline)
        throw 'Private SteamVR did not produce an identifiable owned server/compositor.'
    } catch {
        $startFailure = $_
        try { Stop-PrivateSteamVR $session $Run } catch { Write-Warning $_.Exception.Message }
        throw $startFailure
    }
}

function Update-PrivateSteamVR($Session) {
    # Path and age alone do not prove ownership: require a child of a retained,
    # verified process, born during that parent's lifetime. Never adopt arbitrary
    # SteamVR processes merely because they appeared while our lock was held.
    $candidates = @(Get-Process -Name $Session.names -ErrorAction SilentlyContinue)
    for ($pass = 0; $pass -lt $candidates.Count; $pass++) {
        $added = $false
        foreach ($process in $candidates) {
            if ($Session.owned.ContainsKey($process.Id)) { continue }
            try {
                if (-not $process.Path -or -not $process.Path.StartsWith($Session.root + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
                $info = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$($process.Id)" -ErrorAction Stop
                if (-not $info -or -not $Session.owned.ContainsKey([int]$info.ParentProcessId)) { continue }
                $parent = $Session.owned[[int]$info.ParentProcessId]
                $currentParent = Get-Process -Id $info.ParentProcessId -ErrorAction SilentlyContinue
                if ($currentParent -and -not (Test-SteamVRIdentity $currentParent $parent)) { continue }
                $born = $process.StartTime.ToUniversalTime().Ticks
                if ($born -lt $parent.startUtcTicks) { continue }
                if ($parent.process.HasExited -and $born -gt $parent.process.ExitTime.ToUniversalTime().Ticks) { continue }
            } catch { continue } # An inaccessible/vanished candidate is never adopted.
            # A failed journal write is a session failure, not a skippable process
            # query error. Do not silently continue with ownership only in memory.
            Add-PrivateSteamVRProcess $Session $process ([int]$info.ParentProcessId)
            $added = $true
        }
        if (-not $added) { break }
    }
}

function Stop-PrivateSteamVR($Session, [string]$Run) {
    if ([IO.Path]::GetFullPath($Run) -ne $Session.run) { throw 'SteamVR cleanup run does not match its ownership record.' }
    if ($Session.completed) { return }
    $failures = @()
    $records = @()
    # No new adoption during cleanup or recovery. Only persisted identities qualify.
    foreach ($owned in @($Session.owned.Values)) {
        $record = [ordered]@{ pid = $owned.process.Id; path = $owned.path; startUtcTicks = $owned.startUtcTicks; exited = $false }
        try {
            $current = Get-Process -Id $owned.process.Id -ErrorAction SilentlyContinue
            if (-not $current) {
                $record.exited = $true
            } elseif (-not (Test-SteamVRIdentity $current $owned)) {
                $record.identityMismatch = $true
                throw 'PID now belongs to a different process; left untouched'
            } else {
                $null = $current.Handle
                if (-not $current.HasExited) {
                    $null = $current.CloseMainWindow()
                    if (-not $current.WaitForExit(1500)) { $current.Kill(); $null = $current.WaitForExit(3000) }
                }
                $record.exited = $current.HasExited
                if (-not $record.exited) { throw 'Owned process did not exit' }
            }
        } catch {
            $record.error = $_.Exception.Message
            $failures += "pid $($owned.process.Id): $($_.Exception.Message)"
        }
        $records += $record
    }
    # A different writer's settings, a missing/corrupt backup, or a still-running
    # SteamVR session must survive recovery. Keep all evidence for a later retry.
    try {
        if (@(Get-Process -Name $Session.names -ErrorAction SilentlyContinue).Count) {
            throw 'SteamVR processes remain; settings left untouched'
        }
        if (-not (Test-Path -LiteralPath $Session.backup) -or
            (Get-FileHash -LiteralPath $Session.backup).Hash -ne $Session.originalHash) {
            throw 'Original SteamVR settings backup is missing or changed'
        }
        if (-not (Test-Path -LiteralPath $Session.settings)) { throw 'SteamVR settings were removed by another writer' }
        $currentHash = (Get-FileHash -LiteralPath $Session.settings).Hash
        if ($currentHash -ne $Session.originalHash -and $currentHash -ne $Session.writtenHash) {
            throw 'SteamVR settings changed after staging; left untouched'
        }
        if ($currentHash -ne $Session.originalHash) { Copy-PrivateSteamVRSettings $Session.backup $Session.settings }
        $Session.settingsRestored = (Get-FileHash -LiteralPath $Session.settings).Hash -eq $Session.originalHash
        if (-not $Session.settingsRestored) { throw 'SteamVR settings restoration did not match the original bytes' }
    } catch { $failures += $_.Exception.Message }
    $Session.completed = $Session.settingsRestored -and $failures.Count -eq 0
    Save-PrivateSteamVRSession $Session
    Write-AtomicJson (Join-Path $Run 'private-steamvr.json') @{
        desktopOnly = $true; processes = $records; failures = $failures
        settingsRestored = $Session.settingsRestored; completed = $Session.completed
    }
    if ($Session.launcher) { $Session.launcher.Dispose() }
    if ($failures.Count) { throw "Private SteamVR cleanup: $($failures -join '; ')" }
}

function Restore-PrivateSteamVR([string]$Run) {
    $Run = [IO.Path]::GetFullPath($Run)
    $journal = Join-Path $Run 'steamvr-session.json'
    if (-not (Test-Path -LiteralPath $journal)) {
        if (Test-Path -LiteralPath (Join-Path $Run 'steamvr.vrsettings.original')) {
            throw 'Legacy SteamVR backup has no durable process ownership record; automatic recovery refused.'
        }
        return
    }
    $state = Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json -AsHashtable
    $paths = Get-PrivateSteamVRPaths
    if ($state['version'] -ne 1 -or $state['run'] -ne $Run -or $state['root'] -ne $paths.root -or
        $state['settings'] -ne $paths.settings -or $state['backup'] -ne (Join-Path $Run 'steamvr.vrsettings.original')) {
        throw 'SteamVR recovery record paths/version do not match this run/install.'
    }
    if ($state['completed']) { return }
    $owner = Get-Process -Id $state['ownerPid'] -ErrorAction SilentlyContinue
    if ($owner -and $owner.StartTime.ToUniversalTime().Ticks -eq [long]$state['ownerStartUtcTicks']) {
        throw 'SteamVR session still has a live controller; refusing concurrent recovery.'
    }
    $session = [pscustomobject]@{
        run = $Run; journal = $journal; root = $paths.root; settings = $paths.settings; backup = $state['backup']
        originalHash = $state['originalHash']; writtenHash = $state['writtenHash']
        ownerPid = $state['ownerPid']; ownerStartUtcTicks = $state['ownerStartUtcTicks']
        started = [datetime]::new([long]$state['startedUtcTicks'], [DateTimeKind]::Utc)
        names = @(Get-SteamVRProcessNames); owned = @{}; launcher = $null
        settingsRestored = [bool]$state['settingsRestored']; completed = $false
    }
    foreach ($entry in @($state['processes'])) {
        if (-not $entry['path'].StartsWith($paths.root + '\', [StringComparison]::OrdinalIgnoreCase) -or [long]$entry['startUtcTicks'] -le 0) {
            throw 'SteamVR recovery contains an invalid process identity.'
        }
        # Only the ID is needed to look up and verify a current process at cleanup;
        # do not retain/adopt the current PID until its path and birth time match.
        $session.owned[[int]$entry['pid']] = [pscustomobject]@{
            process = [pscustomobject]@{ Id = [int]$entry['pid'] }; path = $entry['path']
            startUtcTicks = [long]$entry['startUtcTicks']; parentPid = [int]$entry['parentPid']
        }
    }
    Stop-PrivateSteamVR $session $Run
}
