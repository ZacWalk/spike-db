#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$request = [Console]::In.ReadLine() | ConvertFrom-Json -AsHashtable -ErrorAction Stop
$root = Split-Path $PSScriptRoot
. (Join-Path $root '.dd/core.ps1')
foreach ($module in Get-DDRuntimeModules) { . (Join-Path $root ".dd/$module") }

$manifest = Read-DDManifest $root -ForBuild
if ($request.schema -ne 1 -or $request.projectRoot -ne $root -or
    $request.command -notin @('check', 'audit', 'asan', 'lowmem', 'all') -or
    $request.dryRun -isnot [bool] -or $request.parameters -isnot [hashtable]) {
    throw 'Expected a schema 1 SpikeDB verification request from dd.'
}
$parameters = @{}
$definitions = $manifest.commands[$request.command].parameters
foreach ($name in $request.parameters.Keys) {
    if (-not $definitions.ContainsKey($name)) { throw "Unknown verification parameter: $name" }
}
foreach ($name in $definitions.Keys) {
    $value = if ($request.parameters.ContainsKey($name)) { $request.parameters[$name] } else { $definitions[$name].default }
    $parameters[$name] = Convert-DDParameterValue $value $definitions[$name] $name
}
$platform = Get-DDPlatform
$hostName = if ($IsWindows) { 'windows' } else { 'linux' }
$config = $parameters.config.ToLowerInvariant()
$passes = switch ($request.command) {
    'all' { @('check', 'audit', 'cache16', 'cache32', 'asan') }
    'lowmem' { @('cache16', 'cache32') }
    default { @($request.command) }
}
$plans = @(
    foreach ($pass in $passes) {
        $variant = "$pass-$($parameters.arch.ToLowerInvariant())"
        @{
            variant = $variant
            configuration = $config
            preset = "$hostName-variant-$config"
            directory = "build/$platform/variants/$variant/$config"
            cache = @{
                SPIKEDB_AVX512 = $(if ($parameters.arch -eq 'AVX512') { 'ON' } else { 'OFF' })
                SPIKEDB_AUDIT = $(if ($pass -eq 'audit') { 'ON' } else { 'OFF' })
                SPIKEDB_SANITIZE = $(if ($pass -eq 'asan') { 'ON' } else { 'OFF' })
                SPIKEDB_TEST_CACHE = $(if ($pass -eq 'cache16') { '16' } elseif ($pass -eq 'cache32') { '32' } else { '' })
            }
        }
    }
)
$results = @()
if (-not $request.dryRun) {
    Assert-DDDependencies $root
    $env:SPIKEDB_TEST_MODE = if ($parameters.quick) { 'quick' } else { '' }
    foreach ($plan in $plans) {
        $env:SPIKEDB_VARIANT = $plan.variant
        foreach ($name in $plan.cache.Keys) {
            [Environment]::SetEnvironmentVariable($name, $plan.cache[$name], 'Process')
        }
        # Reuse the pinned dd build/test pipeline, with a distinct state key so
        # variant verification cannot invalidate normal build/clean metadata.
        $stateKey = "$($plan.variant)-$config"
        $manifest.build[$platform][$stateKey] = $plan.preset
        $results += Invoke-DDBuild $root $manifest $stateKey @{}
        $results += Invoke-DDTests $root $manifest $stateKey @{ label = '^spike_db$' } @()
    }
}
@{
    schema = 1
    data = @{ plans = $plans; results = $results; quick = $parameters.quick }
    files = @($plans | ForEach-Object directory)
} | ConvertTo-Json -Depth 15 -Compress | Write-Output
