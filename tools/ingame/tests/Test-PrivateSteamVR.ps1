# Pure process mocks and TEMP files: no process, game, SteamVR, desktop or GPU starts.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-steamvr-test-' + [guid]::NewGuid().ToString('N'))
$previous = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $fixtureRoot
$global:NmnSteamVRFixtureProcesses = @{}
$checks = 0
function Check([bool]$Ok, [string]$Label) {
    if (-not $Ok) { throw "FAIL $Label" }
    $script:checks++; "PASS $Label"
}
function Rejected([scriptblock]$Action) {
    try { & $Action | Out-Null; return $null } catch { return $_.Exception.Message }
}
function Get-Process {
    param($Id, $Name, $ErrorAction)
    if ($PSBoundParameters.ContainsKey('Id')) {
        if ($global:NmnSteamVRFixtureProcesses.ContainsKey([int]$Id) -and -not $global:NmnSteamVRFixtureProcesses[[int]$Id].HasExited) { return $global:NmnSteamVRFixtureProcesses[[int]$Id] }
    } else {
        $global:NmnSteamVRFixtureProcesses.Values | Where-Object { -not $_.HasExited -and $_.ProcessName -in $Name }
    }
}
function Get-CimInstance {
    param($ClassName, $Filter, $ErrorAction)
    $id = [int]($Filter -replace '^ProcessId=', '')
    if ($global:NmnSteamVRFixtureProcesses.ContainsKey($id)) { [pscustomobject]@{ ParentProcessId = $global:NmnSteamVRFixtureProcesses[$id].ParentId } }
}
function New-FakeProcess([int]$Id, [string]$Name, [int]$Parent = 0, [datetime]$Born = [datetime]::UtcNow) {
    $fake = [pscustomobject]@{
        Id = $Id; ProcessName = $Name; Path = Join-Path $script:steamRoot "bin\win64\$Name.exe"
        StartTime = $Born; ExitTime = $Born.AddSeconds(10); HasExited = $false; ParentId = $Parent
        Handle = [IntPtr]1; KillCalls = 0; CloseCalls = 0; Stuck = $false; Disposed = $false
    }
    $fake | Add-Member ScriptMethod CloseMainWindow { $this.CloseCalls++; if ($this.Stuck) { throw 'fixture access denied' }; return $false }
    $fake | Add-Member ScriptMethod WaitForExit { param($Timeout) return $this.HasExited }
    $fake | Add-Member ScriptMethod Kill { $this.KillCalls++; $this.HasExited = $true }
    $fake | Add-Member ScriptMethod Dispose { $this.Disposed = $true }
    return $fake
}
try {
    . "$kit\IngameCommon.ps1"
    . "$kit\PrivateSteamVR.ps1"
    function Assert-LockHeld { param($Runtime) } # No real or sandbox lease is needed by a fixture.
    $paths = Get-PrivateSteamVRPaths
    $script:steamRoot = $paths.root
    $settings = $paths.settings
    New-Item -ItemType Directory -Force -Path (Split-Path $settings) | Out-Null
    $original = [Text.Encoding]::UTF8.GetPreamble() + [Text.Encoding]::UTF8.GetBytes("{`r`n  `"steamvr`": { `"otherSetting`": 123 }`r`n}`r`n")
    $script:scenario = 0
    function Start-Fixture {
        $script:scenario++
        $script:run = Join-Path $fixtureRoot "run-$script:scenario"
        New-Item -ItemType Directory -Path $script:run | Out-Null
        [IO.File]::WriteAllBytes($settings, $original)
        $global:NmnSteamVRFixtureProcesses = @{}
        $desktop = [pscustomobject]@{}
        $desktop | Add-Member ScriptMethod Launch {
            param($Executable, $Directory, $Arguments)
            $journal = Get-Content -LiteralPath (Join-Path $script:run 'steamvr-session.json') -Raw | ConvertFrom-Json
            if (-not $journal.originalHash -or -not $journal.writtenHash -or $journal.processes.Count -ne 0) {
                throw 'Recovery metadata must exist before launch'
            }
            if ((Get-FileHash -LiteralPath $journal.settings).Hash -ne $journal.writtenHash) { throw 'Staged settings must match the recorded hash' }
            $born = [datetime]::UtcNow
            $global:NmnSteamVRFixtureProcesses[1001] = New-FakeProcess 1001 vrstartup 0 $born
            $global:NmnSteamVRFixtureProcesses[1002] = New-FakeProcess 1002 vrserver 1001 ($born.AddMilliseconds(1))
            $global:NmnSteamVRFixtureProcesses[1003] = New-FakeProcess 1003 vrcompositor 1002 ($born.AddMilliseconds(2))
            return $global:NmnSteamVRFixtureProcesses[1001]
        }
        return Start-PrivateSteamVR $desktop $script:run
    }

    $session = Start-Fixture
    $journal = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json
    Check ($journal.processes.Count -eq 3 -and $journal.originalHash -and $journal.writtenHash) 'ownership identities and both settings hashes are persisted'
    Check (@($journal.processes | Where-Object { $_.pid -eq 1003 -and $_.parentPid -eq 1002 }).Count -eq 1) 'only verified launcher descendants are adopted'
    Restore-PrivateSteamVR $script:run
    Check (@($global:NmnSteamVRFixtureProcesses.Values | Where-Object { $_.KillCalls -eq 1 }).Count -eq 3) 'abandoned recovery stops all recorded owned processes'
    Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($settings)) -eq [Convert]::ToBase64String($original)) 'recovery restores exact BOM, whitespace and line endings'
    $journal = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json
    Check ($journal.completed -and $journal.settingsRestored) 'successful recovery completion is durable'
    Set-Content -LiteralPath $settings -Value 'later foreign configuration'
    Restore-PrivateSteamVR $script:run
    Check ((Get-Content -LiteralPath $settings -Raw).Trim() -eq 'later foreign configuration') 'completed recovery is idempotent and preserves later settings'

    $session = Start-Fixture
    Set-Content -LiteralPath $settings -Value 'foreign settings update'
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'settings changed after staging') 'a foreign settings edit blocks restoration'
    Check ((Get-Content -LiteralPath $settings -Raw).Trim() -eq 'foreign settings update') 'foreign settings bytes are preserved'
    Check (-not (Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json).completed) 'conflicted recovery retains an incomplete durable record'
    Copy-Item -LiteralPath (Join-Path $script:run 'steamvr.vrsettings.test') -Destination $settings -Force
    Restore-PrivateSteamVR $script:run
    Check ((Get-FileHash -LiteralPath $settings).Hash -eq $session.originalHash) 'a resolved settings conflict can be retried safely'

    $session = Start-Fixture
    $reused = New-FakeProcess 1002 vrserver 999 ($global:NmnSteamVRFixtureProcesses[1002].StartTime.AddMinutes(1))
    $global:NmnSteamVRFixtureProcesses[1002] = $reused
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'different process' -and $reused.KillCalls -eq 0 -and $reused.CloseCalls -eq 0) 'PID reuse with a different birth time is never stopped'
    Check ((Get-FileHash -LiteralPath $settings).Hash -eq $session.writtenHash) 'settings stay untouched while a foreign SteamVR process exists'
    Check ($global:NmnSteamVRFixtureProcesses[1001].HasExited -and $global:NmnSteamVRFixtureProcesses[1003].HasExited) 'identity mismatch does not prevent cleanup of other owned processes'

    $session = Start-Fixture
    $global:NmnSteamVRFixtureProcesses[1002].Path = Join-Path $fixtureRoot 'foreign\vrserver.exe'
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'different process' -and $global:NmnSteamVRFixtureProcesses[1002].KillCalls -eq 0) 'same PID and birth time with a different executable path is preserved'

    $session = Start-Fixture
    $foreign = New-FakeProcess 2001 vrmonitor 999
    $global:NmnSteamVRFixtureProcesses[2001] = $foreign
    Update-PrivateSteamVR $session
    Check (-not $session.owned.ContainsKey(2001)) 'an unrelated new SteamVR process is not adopted by name/path/age'
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'processes remain' -and $foreign.KillCalls -eq 0) 'unknown SteamVR processes survive recovery'

    $session = Start-Fixture
    $global:NmnSteamVRFixtureProcesses[1001].Stuck = $true
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match '1001' -and $global:NmnSteamVRFixtureProcesses[1002].HasExited -and $global:NmnSteamVRFixtureProcesses[1003].HasExited) 'a stop failure is reported while other owned processes still stop'
    Check ((Get-FileHash -LiteralPath $settings).Hash -eq $session.writtenHash) 'settings are not replaced underneath a surviving SteamVR process'

    $session = Start-Fixture
    $state = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json -AsHashtable
    $owner = New-FakeProcess 3001 pwsh
    $global:NmnSteamVRFixtureProcesses[3001] = $owner
    $state.ownerPid = $owner.Id; $state.ownerStartUtcTicks = $owner.StartTime.ToUniversalTime().Ticks
    Write-AtomicJson $session.journal $state
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'live controller' -and @($global:NmnSteamVRFixtureProcesses.Values | Where-Object KillCalls -gt 0).Count -eq 0) 'live controller blocks concurrent recovery before touching any process'

    $session = Start-Fixture
    Set-Content -LiteralPath $session.backup -Value 'corrupt backup'
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'backup is missing or changed' -and (Get-FileHash -LiteralPath $settings).Hash -eq $session.writtenHash) 'a corrupted backup is never restored'

    $session = Start-Fixture
    $state = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json -AsHashtable
    $state.settings = Join-Path $fixtureRoot 'unrelated.settings'
    Write-AtomicJson $session.journal $state
    $message = Rejected { Restore-PrivateSteamVR $script:run }
    Check ($message -match 'record paths/version' -and @($global:NmnSteamVRFixtureProcesses.Values | Where-Object KillCalls -gt 0).Count -eq 0) 'invalid recovery paths are rejected before process or file mutations'

    $session = Start-Fixture
    # A hard kill after staging settings, before launching: no process identities yet.
    $state = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json -AsHashtable
    $state.processes = @(); $global:NmnSteamVRFixtureProcesses = @{}
    Write-AtomicJson $session.journal $state
    Restore-PrivateSteamVR $script:run
    Check ((Get-FileHash -LiteralPath $settings).Hash -eq $session.originalHash) 'prelaunch controller death restores settings without a game/process record'

    # Full abandoned-run integration before game creation, against copied production
    # scripts and an inert desktop shim. The fake DLL is never executed or loaded.
    $fixtureKit = Join-Path $fixtureRoot 'tools\ingame'
    $integration = Join-Path $fixtureRoot '.state\runs\prelaunch-recovery'
    $plugin = Join-Path $fixtureRoot 'VR\Data\F4SE\Plugins\NMN_DeformableTerrain.dll'
    $custom = Join-Path $fixtureRoot 'Documents\My Games\Fallout4VR\Fallout4Custom.ini'
    $built = Join-Path $fixtureRoot 'build\src\NMN_DeformableTerrain.dll'
    New-Item -ItemType Directory -Force -Path $fixtureKit, $integration, (Split-Path $plugin), (Split-Path $custom), (Split-Path $built) | Out-Null
    foreach ($file in @('Repair-AbandonedRun.ps1','IngameCommon.ps1','PrivateSteamVR.ps1','Deploy-Plugin.ps1')) {
        Copy-Item -LiteralPath (Join-Path $kit $file) -Destination $fixtureKit
    }
    Set-Content -LiteralPath (Join-Path $fixtureKit 'PrivateDesktop.ps1') -Value '# No desktop APIs in a fixture.'
    Set-Content -LiteralPath $built -Value 'inert fixture'
    Copy-Item -LiteralPath $built -Destination $plugin
    Set-Content -LiteralPath (Join-Path $integration 'Fallout4Custom.ini.original') -Value 'original custom INI' -NoNewline
    Set-Content -LiteralPath (Join-Path $integration 'Fallout4Custom.ini.test') -Value 'test custom INI' -NoNewline
    Copy-Item -LiteralPath (Join-Path $integration 'Fallout4Custom.ini.test') -Destination $custom
    $state = Get-Content -LiteralPath $session.journal -Raw | ConvertFrom-Json -AsHashtable
    $state.run = $integration; $state.backup = Join-Path $integration 'steamvr.vrsettings.original'
    $state.completed = $false; $state.settingsRestored = $false; $state.processes = @()
    [IO.File]::WriteAllBytes($state.backup, $original)
    Copy-Item -LiteralPath (Join-Path $script:run 'steamvr.vrsettings.test') -Destination $settings -Force
    Write-AtomicJson (Join-Path $integration 'steamvr-session.json') $state
    Write-AtomicJson (Join-Path $fixtureRoot '.state\deploy-VR.json') @{
        runtime = 'VR'; session = $integration; createdDirs = @()
        files = @(@{ target = $plugin; backup = $null; sha256 = (Get-FileHash -LiteralPath $plugin).Hash })
    }
    & (Join-Path $fixtureKit 'Repair-AbandonedRun.ps1') -Runtime VR -Run $integration | Out-Null
    Check ((Get-FileHash -LiteralPath $settings).Hash -eq $session.originalHash) 'abandoned-run command restores prelaunch SteamVR settings'
    Check ((Get-Content -LiteralPath $custom -Raw) -eq 'original custom INI') 'abandoned-run command also restores the game INI'
    Check (-not (Test-Path -LiteralPath $plugin) -and -not (Test-Path -LiteralPath (Join-Path $fixtureRoot '.state\deploy-VR.json'))) 'abandoned-run command removes only its recorded deployment'
    Check ((Get-Content -LiteralPath (Join-Path $integration 'summary.json') -Raw | ConvertFrom-Json).recovered) 'abandoned-run command records completed prelaunch recovery'
    $foreignLease = Join-Path $fixtureRoot '.state\lease.json'
    Write-AtomicJson $foreignLease @{ claim_token = 'another-run' }
    $beforeLease = (Get-FileHash -LiteralPath $foreignLease).Hash
    $message = Rejected { & (Join-Path $fixtureKit 'Repair-AbandonedRun.ps1') -Runtime VR -Run $integration }
    Check ($message -match 'lease belongs to another' -and (Get-FileHash -LiteralPath $foreignLease).Hash -eq $beforeLease) 'stale recovery cannot release another run lease'
    "ALL $checks PRIVATE STEAMVR MOCK CHECKS PASS"
} finally {
    $env:NMN_INGAME_SANDBOX = $previous
    Remove-Variable -Name NmnSteamVRFixtureProcesses -Scope Global -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected SteamVR fixture cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
