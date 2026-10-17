param([Parameter(Mandatory)][string]$Run, [Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
$desktop = $null
$bitmap = $null
$graphics = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
$timing = [ordered]@{workerPid=$PID;started=(Get-Date).ToString('o');stage='starting'}
function Capture-Stage([string]$Stage) {
    $timing.stage = $Stage
    $timing.elapsedSeconds = $clock.Elapsed.TotalSeconds
    $timing | ConvertTo-Json | Set-Content -LiteralPath "$Path.capture.json"
}
try {
    Capture-Stage starting
    . "$PSScriptRoot/PrivateDesktop.ps1"
    $record = Get-Content -LiteralPath (Join-Path $Run 'private-desktop.json') -Raw | ConvertFrom-Json
    $game = Get-Process -Id $record.gamePid
    if ($game.Path -ne $record.gamePath -or $game.StartTime.ToUniversalTime().Ticks -ne $record.gameStartUtcTicks) {
        throw 'Owned game identity no longer matches'
    }
    $desktop = [NmnIngame.PrivateDesktop]::new($record.desktop)
    $window = $desktop.FindWindow($game.Id)
    [void]$desktop.VerifyWindow($window, $game.Id)
    $bounds = [NmnIngame.PrivateDesktop+Rect]::new()
    if (-not [NmnIngame.PrivateDesktop]::GetClientRect($window, [ref]$bounds)) { throw 'No client rectangle' }
    Add-Type -AssemblyName System.Drawing
    $bitmap = [Drawing.Bitmap]::new($bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        Capture-Stage printing
        if (-not [NmnIngame.PrivateDesktop]::PrintWindow($window, $dc, 3)) { throw 'PrintWindow failed' }
    } finally { $graphics.ReleaseHdc($dc) }
    Capture-Stage encoding
    $temporary = "$Path.$PID.tmp"
    $bitmap.Save($temporary, [Drawing.Imaging.ImageFormat]::Png)
    Move-Item -LiteralPath $temporary -Destination $Path -Force
    Capture-Stage complete
} catch {
    $_ | Out-String | Set-Content -LiteralPath "$Path.error.txt"
    exit 1
} finally {
    if ($graphics) { $graphics.Dispose() }
    if ($bitmap) { $bitmap.Dispose() }
    if ($desktop) { $desktop.Dispose() }
}
