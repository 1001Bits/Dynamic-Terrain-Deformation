# Test-only mailbox client. Acknowledgement means the console call returned;
# check state/logs/screenshots separately to prove the command's actual effect.
param(
	[Parameter(Mandatory)][ValidateSet('OG', 'AE', 'VR')][string]$Runtime,
	[Parameter(Mandatory)][string]$Directory,
	[Parameter(Mandatory)][string[]]$Commands,
	[int]$TimeoutSeconds = 30
)
. "$PSScriptRoot\IngameCommon.ps1"
$r = Get-Runtime $Runtime
Assert-LockHeld $r
$folder = (Resolve-Path -LiteralPath $Directory).Path
$runs = [IO.Path]::GetFullPath((Join-Path $script:StateDir 'runs')).TrimEnd('\') + '\'
if (-not $folder.StartsWith($runs, [StringComparison]::OrdinalIgnoreCase)) { throw 'The mailbox must be in this kit''s run directory.' }
$ready = Get-Content -LiteralPath (Join-Path $folder 'ready.json') -Raw | ConvertFrom-Json
$process = Get-Process -Id $ready.pid -ErrorAction Stop
if ($process.Path -ne (Join-Path $r.Game $r.Exe)) { throw 'The driver PID does not belong to the reserved game install.' }
if ($Commands.Count -eq 0 -or $Commands.Count -gt 64 -or @($Commands | Where-Object {
	$_ -match '[\r\n\x00]' -or $_.Length -gt 2048 -or ($_.StartsWith('@') -and $_ -ne '@status')
}).Count) { throw 'Invalid command batch.' }
$sequence = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$path = Join-Path $folder 'commands.txt'
if (Test-Path -LiteralPath $path) {
	$previous = [long](Get-Content -LiteralPath $path -TotalCount 1)
	$sequence = [Math]::Max($sequence, $previous + 1)
}
$temporary = "$path.$PID.tmp"
Set-Content -LiteralPath $temporary -Value (@([string]$sequence) + $Commands) -Encoding utf8NoBOM
Move-Item -LiteralPath $temporary -Destination $path -Force
$ack = Join-Path $folder "$sequence.json"
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not (Test-Path -LiteralPath $ack)) {
	if ($process.HasExited) { throw 'The owned game exited before acknowledging the command.' }
	if ((Get-Date) -ge $deadline) { throw 'Native command acknowledgement timed out.' }
	Start-Sleep -Milliseconds 200
}
$result = Get-Content -LiteralPath $ack -Raw | ConvertFrom-Json
if (-not $result.ok -or $result.executed -ne $Commands.Count -or $result.pid -ne $process.Id) { throw "Command task failed: $(Get-Content -LiteralPath $ack -Raw)" }
$result
