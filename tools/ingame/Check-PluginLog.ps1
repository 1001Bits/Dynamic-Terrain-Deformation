<#
.SYNOPSIS
  Reads NMN_DeformableTerrain.log and says whether a run went right.

.DESCRIPTION
  Checks the start-up chain (runtime, hooks, device, shader registry, clipmap), whether the
  landscape was actually routed and tessellated once in the world, and that nothing went
  wrong (errors, link failures, swallowed exceptions, dropped stamps). Also pulls the
  profiler's numbers out when EnableProfiler was on. Exit code 0 = PASS, 1 = FAIL.

.EXAMPLE
  .\Check-PluginLog.ps1 -Runtime OG
  .\Check-PluginLog.ps1 -Log 'C:\...\runs\20261012-...\NMN_DeformableTerrain.log' -Runtime VR -Json out.json
#>
param(
	[ValidateSet('OG', 'AE', 'VR')][string]$Runtime = 'OG',
	[string]$Log,
	[switch]$StartupOnly,  # only the chain up to "loaded"/"Clipmap ready" (main menu, no world)
	[switch]$Baseline,  # perf-off deliberately has no clipmap or terrain routing
	[string]$Json
)

. "$PSScriptRoot\IngameCommon.ps1"
if (-not $Log) { $Log = (Get-Runtime $Runtime).PluginLog }
if (-not (Test-Path -LiteralPath $Log)) { Write-Output "FAIL: no log at $Log"; exit 1 }

$lines = Get-Content -LiteralPath $Log
$text = $lines -join "`n"
$results = [System.Collections.Generic.List[object]]::new()

function Expect([string]$Name, [string]$Pattern, [switch]$World) {
	if ($World -and $StartupOnly) { return }
	$hit = $lines | Select-String -Pattern $Pattern | Select-Object -First 1
	$results.Add([pscustomobject]@{ Check = $Name; Pass = [bool]$hit; Detail = if ($hit) { $hit.Line.Trim() } else { "missing /$Pattern/" } })
}

function Forbid([string]$Name, [string]$Pattern) {
	$hits = @($lines | Select-String -Pattern $Pattern)
	$results.Add([pscustomobject]@{ Check = $Name; Pass = $hits.Count -eq 0; Detail = if ($hits) { "$($hits.Count)x, first: $($hits[0].Line.Trim())" } else { 'none' } })
}

# ── start-up ─────────────────────────────────────────────────────────────────────────
Expect 'plugin loading'            'NMN_DeformableTerrain v[0-9.]+ .* is loading'
Expect 'runtime accepted'          'Detected runtime: Fallout 4'
Expect 'device creation watched'   'ShaderRegistry: watching device creation \(D3D11Create'
Expect 'plugin loaded'             'NMN_DeformableTerrain loaded'
Expect 'D3D device'                'D3D ready: device='
Expect 'vertex shaders recorded'   'ShaderRegistry: [1-9][0-9]* vertex shader\(s\) recorded'
if (-not $Baseline) { Expect 'clipmap ready' 'Clipmap ready: \d level' }
Expect 'G-buffer hooks'            'Hooked BSDFPrePassShader::SetupGeometry/RestoreGeometry'
if ($Runtime -eq 'VR') {
	Expect 'VR frame hook'         'Hooked BSGraphics::Renderer::Unlock in Render_PreUI'
} else {
	Expect 'frame hook'            'Hooked DrawWorld::DeferredPrePass'
	Expect 'snow spray hook'       'Hooked DrawWorld::Forward'
}
Expect 'depth pre-pass hook'       'Hooked BSGraphics::Renderer::RegisterForZPrePass'
Expect 'impact hooks'              'Impacts: explosions hooked, bullets hooked, lasers hooked, arrows hooked'

# ── in the world ─────────────────────────────────────────────────────────────────────
if ($Baseline) {
	Expect 'disabled baseline in world' -World 'Exterior world frame: terrain=false clipmap=false'
	Expect 'baseline profiler running' -World 'GPU frame span\s+avg\s+[0-9.]+ ms'
	Forbid 'baseline routed terrain' 'Camera gate: ROUTING|Terrain shader pair:'
	Forbid 'baseline dispatched field' 'Clipmap dispatch\s+avg'
} else {
	Expect 'landscape layout'          -World 'Routed vertex shader .* landscape layout recognised'
	Expect 'terrain shaders ready'     -World 'Terrain shader pair: (compiler [0-9.]+ ms, ready|reused compiled bytecode, ready)'
	Expect 'player camera routed'      -World 'Camera gate: ROUTING'
	Expect 'depth pre-pass handover'   -World 'Depth pre-pass: near terrain is drawn by the G-buffer pass only'
}

# ── must not happen ──────────────────────────────────────────────────────────────────
Forbid 'critical lines'            '\[CRIT\]'
Forbid 'error lines'               '\[ERROR\]'
Forbid 'unrecognised layout'       'landscape layout NOT recognised'
Forbid 'link check refused'        'does not link to the vertex shader'
Forbid 'swallowed exception'       "this frame's work was skipped"
Forbid 'no shader bytecode'        'has no recorded bytecode|no vertex shaders were seen being created'
Forbid 'VR camera source lost'     'renderer context gave no camera'
Forbid 'dropped stamps'            'Stamps:.*\bDROPPED\b'

$warnings = @($lines | Select-String -Pattern '\[WARN\]')
$dropped = @($lines | Select-String -Pattern 'Stamps:.*\bDROPPED\b')
$refused = @($lines | Select-String -Pattern 'Camera gate: refusing')
$resets = @($lines | Select-String -Pattern 'World state reset \(([^)]*)\)' | ForEach-Object { $_.Matches[0].Groups[1].Value })

# ── profiler (EnableProfiler = 1) ────────────────────────────────────────────────────
function LastAverage([string]$Label) {
	$hit = $lines | Select-String -Pattern ([regex]::Escape($Label) + '\s+avg\s+([0-9.]+) ms') | Select-Object -Last 1
	if ($hit) { [double]$hit.Matches[0].Groups[1].Value } else { $null }
}
$profile = [ordered]@{
	gpuFrameMs      = LastAverage 'GPU frame span'
	clipmapMs       = LastAverage 'Clipmap dispatch'
	landscapeMs     = LastAverage 'Landscape'
	bracketCpuMs    = LastAverage 'Bracket CPU'
	gatherCpuMs     = LastAverage 'Gather CPU'
	coverageCpuMs   = LastAverage 'Coverage CPU'
	shelterCpuMs    = LastAverage 'Shelter CPU'
	routing         = ($lines | Select-String -Pattern 'Routing landscape seen.*' | Select-Object -Last 1 | ForEach-Object { $_.Line.Trim() })
	geometry        = ($lines | Select-String -Pattern 'Geometry\s+HS .*' | Select-Object -Last 1 | ForEach-Object { $_.Line.Trim() })
}

$failed = @($results | Where-Object { -not $_.Pass })
foreach ($r in $results) {
	'{0}  {1,-26} {2}' -f ($(if ($r.Pass) { 'PASS' } else { 'FAIL' }), $r.Check, $r.Detail) | Write-Output
}
Write-Output ''
Write-Output "World resets: $(if ($resets) { $resets -join ', ' } else { 'none' })"
if ($refused) { Write-Output 'Camera gate refused an orthographic view; correlate its time with the UI state and render pass.' }
Write-Output "Warnings: $($warnings.Count)$(if ($dropped) { " (incl. $($dropped.Count) DROPPED-stamps lines)" })"
if ($profile.gpuFrameMs -ne $null) {
	Write-Output ('Profile (last report): frame {0} ms | clipmap {1} ms | landscape {2} ms | CPU bracket {3} / gather {4} / coverage {5} / shelter {6} ms' -f `
		$profile.gpuFrameMs, $profile.clipmapMs, $profile.landscapeMs, $profile.bracketCpuMs, $profile.gatherCpuMs, $profile.coverageCpuMs, $profile.shelterCpuMs)
	if ($profile.routing) { Write-Output "  $($profile.routing)" }
	if ($profile.geometry) { Write-Output "  $($profile.geometry)" }
}
Write-Output ''
Write-Output $(if ($failed.Count -eq 0) { "LOG CHECK PASS ($Log)" } else { "LOG CHECK FAIL: $($failed.Count) check(s) ($Log)" })

if ($Json) {
	[ordered]@{
		log = $Log; runtime = $Runtime; startupOnly = [bool]$StartupOnly; baseline = [bool]$Baseline
		pass = $failed.Count -eq 0; checks = $results; worldResets = $resets
		warnings = $warnings.Count; dropped = $dropped.Count; profile = $profile
	} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Json -Encoding utf8NoBOM
}
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
