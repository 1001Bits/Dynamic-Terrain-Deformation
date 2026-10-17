# Dot-source alongside IngameCommon.ps1. Each capture runs in a disposable process:
# PrintWindow can block indefinitely, so it must never run on the cleanup thread.
if (-not ('NmnIngame.PrivateDesktop' -as [type])) { Add-Type -Path "$PSScriptRoot/PrivateDesktop.cs" }
function Invoke-PrivateDesktopWorker {
    param([Parameter(Mandatory)]$Desktop, [Parameter(Mandatory)][string[]]$Arguments,
        [ValidateRange(1, 30)][int]$TimeoutSeconds = 20)
    $worker = $Desktop.Launch((Join-Path $PSHOME 'pwsh.exe'), $PSScriptRoot, $Arguments)
    try {
        if (-not $worker.WaitForExit($TimeoutSeconds * 1000)) {
            $worker.Kill(); [void]$worker.WaitForExit(3000)
            throw "Private desktop worker timed out after $TimeoutSeconds seconds"
        }
        return $worker.ExitCode
    } finally {
        if (-not $worker.HasExited) { $worker.Kill(); [void]$worker.WaitForExit(3000) }
        $worker.Dispose()
    }
}
function Save-PrivateGameWindow {
    param([Parameter(Mandatory)][string]$Run, [Parameter(Mandatory)][string]$Path,
        [ValidateRange(1, 30)][int]$TimeoutSeconds = 20)
    $ErrorActionPreference = 'Stop'
    # The worker starts in the kit directory, which may differ from the caller's.
    $Run = (Resolve-Path -LiteralPath $Run).Path
    $Path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    $record = Get-Content -LiteralPath (Join-Path $Run 'private-desktop.json') -Raw | ConvertFrom-Json
    $desktop = [NmnIngame.PrivateDesktop]::new($record.desktop)
    try {
        $code = Invoke-PrivateDesktopWorker -Desktop $desktop -TimeoutSeconds $TimeoutSeconds -Arguments @(
            '-NoProfile', '-File', "$PSScriptRoot/Capture-PrivateWindow.ps1", '-Run', $Run, '-Path', $Path)
        if ($code -ne 0 -or -not (Test-Path -LiteralPath $Path)) {
            throw "Private window capture failed (exit $code); see $Path.error.txt"
        }
    } finally {
        $desktop.Dispose()
    }
}
