# Real Win32 integration: window ownership, private capture, and hung-window timeout.
# No game, shared install, global input or desktop switch is used.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
. "$kit/PrivateDesktop.ps1"
$run = Join-Path $kit ('.state/private-desktop-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$fixture = Join-Path $run 'fixture.ps1'
@'
param([string]$Run)
Add-Type -AssemblyName System.Windows.Forms
$form = [Windows.Forms.Form]::new()
$form.Text = 'Terrain desktop fixture'
$form.ClientSize = [Drawing.Size]::new(320, 180)
$form.BackColor = [Drawing.Color]::Magenta
$timer = [Windows.Forms.Timer]::new()
$timer.Interval = 100
$timer.Add_Tick({
    if (Test-Path -LiteralPath (Join-Path $Run 'hang')) {
        $timer.Stop()
        Set-Content -LiteralPath (Join-Path $Run 'hanging') -Value 'yes'
        Start-Sleep -Seconds 90
    }
})
$timer.Start()
[Windows.Forms.Application]::Run($form)
'@ | Set-Content -LiteralPath $fixture
$desktop = $null
$child = $null
$checks = @()
function Check([bool]$OK, [string]$Name) {
    $script:checks += [pscustomobject]@{pass=$OK;name=$Name}
    if (-not $OK) { throw "FAIL $Name" }
    "PASS $Name"
}
try {
    $desktop = [NmnIngame.PrivateDesktop]::new()
    $child = $desktop.Launch((Join-Path $PSHOME 'pwsh.exe'), $run,
        [string[]]@('-NoProfile', '-STA', '-File', $fixture, '-Run', $run))
    $deadline = (Get-Date).AddSeconds(15)
    $window = [IntPtr]::Zero
    while ($window -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $window = $desktop.FindWindow($child.Id)
    }
    Check ($desktop.VerifyWindow($window, $child.Id) -eq $desktop.Name) 'GUI child uses the private desktop'
    $record = @{desktop=$desktop.Name;gamePid=$child.Id;gamePath=$child.Path;gameStartUtcTicks=$child.StartTime.ToUniversalTime().Ticks}
    $record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'private-desktop.json')
    # Capture from caller-relative paths even though its worker uses another cwd.
    $relativeRun = Resolve-Path -LiteralPath $run -Relative
    Save-PrivateGameWindow -Run $relativeRun -Path (Join-Path $relativeRun 'fixture.png')
    Add-Type -AssemblyName System.Drawing
    $picture = [Drawing.Bitmap]::new((Join-Path $run 'fixture.png'))
    try { Check ($picture.Width -eq 320 -and $picture.Height -eq 180 -and $picture.GetPixel(100,100).ToArgb() -eq [Drawing.Color]::Magenta.ToArgb()) 'Capture contains only the owned client window' }
    finally { $picture.Dispose() }
    $rejected = $false
    try { [void][NmnIngame.PrivateDesktop]::new('Default') } catch { $rejected = $true }
    Check $rejected 'Default desktop cannot be used as a test desktop'
    $rejected = $false
    try { [void]$desktop.VerifyWindow($window, $PID) } catch { $rejected = $true }
    Check $rejected 'Wrong process identity is rejected'
    Set-Content -LiteralPath (Join-Path $run 'hang') -Value 'yes'
    $deadline = (Get-Date).AddSeconds(5)
    while (-not (Test-Path (Join-Path $run 'hanging')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Check (Test-Path (Join-Path $run 'hanging')) 'Fixture window is deliberately unresponsive'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    try { Save-PrivateGameWindow -Run $run -Path (Join-Path $run 'hung.png') -TimeoutSeconds 4 }
    catch { $timedOut = $_.Exception.Message -like '*timed out*' }
    Check (($timedOut -or (Test-Path (Join-Path $run 'hung.png'))) -and $watch.Elapsed.TotalSeconds -lt 10) 'Hung window cannot stall capture cleanup'
    $sleepWorker = Join-Path $run 'sleep-worker.ps1'
    Set-Content -LiteralPath $sleepWorker -Value 'Start-Sleep -Seconds 90'
    $watch.Restart()
    $timedOut = $false
    try { Invoke-PrivateDesktopWorker -Desktop $desktop -Arguments @('-NoProfile', '-File', $sleepWorker) -TimeoutSeconds 4 | Out-Null }
    catch { $timedOut = $_.Exception.Message -like '*timed out*' }
    Check ($timedOut -and $watch.Elapsed.TotalSeconds -lt 10) 'Blocked capture worker is killed at the timeout'
    Check ([NmnIngame.PrivateDesktop]::InputDesktopName() -eq $desktop.InputBefore) 'User input desktop remains unchanged'
} finally {
    if ($child) { if (-not $child.HasExited) { $child.Kill(); [void]$child.WaitForExit(3000) }; $child.Dispose() }
    if ($desktop) { $desktop.Dispose() }
    $checks | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'checks.json')
}
"ALL PRIVATE DESKTOP CHECKS PASS; evidence: $run"
