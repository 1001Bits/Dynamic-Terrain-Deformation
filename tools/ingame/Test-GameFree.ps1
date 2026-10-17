<#
.SYNOPSIS
  Read-only: can a test run now? Lists each runtime's install, build and lock state.
.EXAMPLE
  .\Test-GameFree.ps1            # all runtimes
  .\Test-GameFree.ps1 -Runtime OG -Quiet; if ($LASTEXITCODE -eq 0) { 'free' }
#>
param([ValidateSet('OG', 'AE', 'VR')][string]$Runtime, [switch]$Quiet)

. "$PSScriptRoot\IngameCommon.ps1"
$keys = if ($Runtime) { @($Runtime) } else { @('OG', 'AE', 'VR') }
$allFree = $true

if (-not $Quiet) {
	$art = Get-BuildArtifacts
	Write-Host "Build: $($art.Commit)$(if ($art.Dirty) { ' (+ uncommitted src/data changes)' }), dll SHA-256 $($art.Sha256)"
	$free = [Math]::Round((Get-FreePhysicalMemoryGB), 1)
	Write-Host "Free physical memory: $free GB"
	Write-Host ''
}

foreach ($key in $keys) {
	try {
		$r = Get-Runtime $key
		$state = Test-RuntimeFree $r
		$deployed = Test-Path (Join-Path $script:StateDir "deploy-$key.json")
		$foreign = Test-Path (Join-Path $r.Plugins "$script:PluginName.dll")
		if (-not $state.Free) { $allFree = $false }
		if (-not $Quiet) {
			Write-Host ('{0,-3} {1,-22} {2,-5} {3}' -f $key, $r.Name, $(if ($state.Free) { 'FREE' } else { 'BUSY' }), $state.Reason)
			if ($deployed) { Write-Host '      this kit has the plugin deployed there (Deploy-Plugin.ps1 -Remove to undo)' }
			elseif ($foreign) { Write-Host '      a plugin DLL not deployed by this kit is already in Data\F4SE\Plugins' }
		}
	} catch {
		$allFree = $false
		if (-not $Quiet) { Write-Host ('{0,-3} {1}' -f $key, $_.Exception.Message) }
	}
}
exit $(if ($allFree) { 0 } else { 1 })
