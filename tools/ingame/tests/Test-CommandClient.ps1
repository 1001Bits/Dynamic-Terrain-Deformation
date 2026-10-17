# Mailbox protocol fixtures only; no native code, game process or input is used.
$ErrorActionPreference = 'Stop'
$kit = Split-Path $PSScriptRoot
$sandbox = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('nmn-command-test-' + [guid]::NewGuid().ToString('N'))
$previous = $env:NMN_INGAME_SANDBOX
$env:NMN_INGAME_SANDBOX = $sandbox
$fails = 0
$mode = 'success'
$fake = $null
function Check([bool]$Ok, [string]$Label) { if ($Ok) { "PASS $Label" } else { "FAIL $Label"; $script:fails++ } }
function Get-Process { param($Id, $Name, $ErrorAction) if ($Id) { $fake } }
function Start-Sleep {
	param($Milliseconds, $Seconds)
	if (-not $mailbox -or -not (Test-Path (Join-Path $mailbox 'commands.txt'))) { return }
	$lines = @(Get-Content -LiteralPath (Join-Path $mailbox 'commands.txt'))
	$ack = @{ sequence = [long]$lines[0]; executed = $lines.Count - 1; ok = $true; pid = 999998; exterior = $true }
	if ($mode -eq 'partial') { $ack.executed-- }
	if ($mode -eq 'foreign') { $ack.pid = 999997 }
	$ack | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $mailbox "$($lines[0]).json")
}
try {
	. "$kit\IngameCommon.ps1"
	$mailbox = Join-Path $sandbox '.state\runs\fixture\driver'
	New-Item -ItemType Directory -Force -Path $mailbox | Out-Null
	$r = Get-Runtime OG
	Enter-GameLock $r | Out-Null
	$fake = [pscustomobject]@{ Id = 999998; Path = (Join-Path $r.Game $r.Exe); HasExited = $false }
	@{ pid = $fake.Id } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $mailbox 'ready.json')
	$result = & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands @('player.setpos x -81920', '@status')
	Check ($result.ok -and $result.executed -eq 2 -and $result.pid -eq $fake.Id) 'owned mailbox accepts a complete acknowledgement'
	$payload = @(Get-Content -LiteralPath (Join-Path $mailbox 'commands.txt'))
	Check ($payload.Count -eq 3 -and $payload[1] -eq 'player.setpos x -81920' -and $payload[2] -eq '@status') 'command payload preserves exact lines'
	$next = & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands '@status'
	Check ($next.sequence -gt $result.sequence) 'sequences increase between requests'
	$hash = (Get-FileHash (Join-Path $mailbox 'commands.txt')).Hash
	$rejected = $false; try { & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands "@status`nqqq" } catch { $rejected = $true }
	Check ($rejected -and (Get-FileHash (Join-Path $mailbox 'commands.txt')).Hash -eq $hash) 'embedded newlines rejected without publishing'
	$rejected = $false; try { & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $sandbox -Commands '@status' } catch { $rejected = $true }
	Check $rejected 'mailboxes outside this kit run folder rejected'
	$fake.Path = 'C:\foreign\Fallout4.exe'
	$rejected = $false; try { & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands '@status' } catch { $rejected = $true }
	Check $rejected 'foreign process path rejected'
	$fake.Path = Join-Path $r.Game $r.Exe
	$mode = 'partial'
	$rejected = $false; try { & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands '@status' } catch { $rejected = $true }
	Check $rejected 'partial execution cannot pass'
	$mode = 'foreign'
	$rejected = $false; try { & "$kit\Send-TestCommand.ps1" -Runtime OG -Directory $mailbox -Commands '@status' } catch { $rejected = $true }
	Check $rejected 'acknowledgement from a different PID rejected'
} finally {
	if (Get-Command Exit-GameLock -ErrorAction SilentlyContinue) { Exit-GameLock }
	$env:NMN_INGAME_SANDBOX = $previous
	$resolved = [IO.Path]::GetFullPath($sandbox)
	$allowed = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
	if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected sandbox cleanup path' }
	if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
"`n$(if ($fails) { "$fails FAILED" } else { 'ALL COMMAND CLIENT TESTS PASS' })"
exit $(if ($fails) { 1 } else { 0 })
