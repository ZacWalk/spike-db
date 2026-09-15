#requires -Version 7.4
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = Split-Path $PSScriptRoot
$pwsh = Join-Path $PSHOME $(if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' })

function Invoke-Driver([string[]]$Arguments, [int]$ExpectedExit = 0) {
    $output = & $pwsh -NoProfile -File (Join-Path $root 'dd.ps1') @Arguments --project $root --non-interactive --json
    $exitCode = $LASTEXITCODE
    $result = $output | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    if ($exitCode -ne $ExpectedExit -or $result.exitCode -ne $ExpectedExit -or $result.ok -ne ($ExpectedExit -eq 0)) {
        throw "dd $($Arguments -join ' ') returned unexpected status: $output"
    }
    return $result
}

$upstream = Get-Content (Join-Path $root 'docs/dd-upstream.json') -Raw | ConvertFrom-Json -AsHashtable
if ($upstream.repository -ne 'https://github.com/ZacWalk/dd' -or
    $upstream.revision -ne 'b64dc38fca7d7a1e9397a70d0ed80d04dc61e1d1' -or
    $upstream.version -ne '0.2.0') {
    throw 'Expected the reviewed dd v0.2.0 release.'
}
$vendoredFiles = @('dd.ps1') + @(
    Get-ChildItem (Join-Path $root '.dd') -Recurse -File -Force |
        ForEach-Object { [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/') } |
        Where-Object { -not $_.StartsWith('.dd/state/') }
)
if (Compare-Object ($vendoredFiles | Sort-Object) ($upstream.files.Keys | Sort-Object)) {
    throw 'Provenance must cover every vendored file, including templates, and no generated state.'
}
foreach ($file in $upstream.files.Keys) {
    if ($upstream.files[$file] -notmatch '^[A-F0-9]{64}$' -or
        (Get-FileHash (Join-Path $root $file) -Algorithm SHA256).Hash -cne $upstream.files[$file]) {
        throw "Vendored dd differs from reviewed provenance: $file"
    }
}
$attributes = Get-Content (Join-Path $root '.gitattributes')
if (-not @($attributes | Where-Object { $_ -match '^/dd\.ps1 -text(?: |$)' }).Count -or
    -not @($attributes | Where-Object { $_ -match '^/\.dd/\*\* -text(?: |$)' }).Count) {
    throw 'Vendored files must remain byte-stable on fresh Git checkouts.'
}

$manifest = Import-PowerShellDataFile (Join-Path $root 'dd.psd1')
$presets = Get-Content (Join-Path $root 'CMakePresets.json') -Raw | ConvertFrom-Json -AsHashtable
if ($manifest.project.type -ne 'library' -or $manifest.project.'default-target' -ne 'test_spike_db' -or
    $manifest.dependencies.owner -ne 'application') {
    throw 'Expected a CMake-owned library with the existing test CLI as default.'
}
foreach ($platform in @('x64-windows', 'x64-linux')) {
    foreach ($config in @('debug', 'release')) {
        $name = $manifest.build[$platform][$config]
        $configure = @($presets.configurePresets | Where-Object name -eq $name)
        if ($configure.Count -ne 1 -or $configure[0].binaryDir -ne ('${sourceDir}/build/' + $platform + '/' + $config)) {
            throw "Unexpected $platform $config configure preset."
        }
        foreach ($phase in @('buildPresets', 'testPresets')) {
            $preset = @($presets[$phase] | Where-Object name -eq $name)
            if ($preset.Count -ne 1 -or $preset[0].configurePreset -ne $name) { throw "Invalid $phase mapping: $name" }
        }
    }
}
$metadata = (Invoke-Driver @('targets')).data
if ($metadata.targets.Count -ne 2 -or $metadata.defaultTarget -ne 'test_spike_db') { throw 'Expected two native targets.' }
foreach ($id in @('spike_db', 'test_spike_db')) {
    $target = @($metadata.targets | Where-Object id -eq $id)
    $expectedKind = if ($id -eq 'spike_db') { 'library' } else { 'cli' }
    if ($target.Count -ne 1 -or $target[0].kind -ne $expectedKind -or $target[0].cmakeTarget -ne $id -or
        $target[0].runnable -ne ($id -eq 'test_spike_db') -or $target[0].testLabel -ne 'spike_db') {
        throw "Incorrect target metadata for $id."
    }
}
$library = $manifest.targets | Where-Object id -eq 'spike_db'
foreach ($config in @('debug', 'release')) {
    if ($library["$config-path"] -ne "build/{platform}/$config/{libprefix}spike_db{lib}") {
        throw 'Archive paths must expand correctly for both MSVC and GCC.'
    }
}
foreach ($verb in @('run', 'launch')) {
    $refusal = Invoke-Driver @($verb, 'spike_db') 2
    if (($refusal.errors -join ' ') -notmatch 'library.*no executable') { throw 'Library execution must fail before building.' }
}

$commands = (Invoke-Driver @('commands')).data.commands
if (Compare-Object @('all', 'asan', 'audit', 'check', 'lowmem') @($commands.name | Sort-Object)) {
    throw 'Expected all SpikeDB verification commands.'
}
foreach ($command in $commands) {
    if (-not $command.scriptExists -or $command.effects -ne 'write' -or -not $command.supportsDryRun) {
        throw "Invalid project command contract: $($command.name)"
    }
}
$stateBefore = @(Get-ChildItem (Join-Path $root '.dd/state') -File -ErrorAction SilentlyContinue |
    ForEach-Object { "$($_.Name):$((Get-FileHash $_.FullName).Hash)" })
foreach ($arch in @('AVX2', 'AVX512')) {
    foreach ($config in @('Release', 'Debug')) {
        $response = Invoke-Driver @('all', '--arch', $arch, '--config', $config, '--quick', 'true', '--dry-run')
        $plan = $response.data.result
        if ($plan.plans.Count -ne 5 -or $plan.results.Count -ne 0 -or -not $plan.quick) { throw 'Invalid all dry-run response.' }
        $passes = @('check', 'audit', 'cache16', 'cache32', 'asan')
        for ($i = 0; $i -lt $passes.Count; $i++) {
            $entry = $plan.plans[$i]
            $variant = "$($passes[$i])-$($arch.ToLowerInvariant())"
            $platform = if ($IsWindows) { 'x64-windows' } else { 'x64-linux' }
            if ($entry.variant -ne $variant -or
                $entry.directory -ne "build/$platform/variants/$variant/$($config.ToLowerInvariant())" -or
                $entry.configuration -ne $config.ToLowerInvariant()) { throw 'Variant paths must isolate platform, configuration and architecture.' }
            $expected = @{
                SPIKEDB_AVX512 = $(if ($arch -eq 'AVX512') { 'ON' } else { 'OFF' })
                SPIKEDB_AUDIT = $(if ($i -eq 1) { 'ON' } else { 'OFF' })
                SPIKEDB_SANITIZE = $(if ($i -eq 4) { 'ON' } else { 'OFF' })
                SPIKEDB_TEST_CACHE = $(if ($i -eq 2) { '16' } elseif ($i -eq 3) { '32' } else { '' })
            }
            foreach ($key in $expected.Keys) {
                if ($entry.cache[$key] -ne $expected[$key]) { throw "Wrong $key for $variant." }
            }
        }
    }
}
foreach ($command in @('check', 'audit', 'asan', 'lowmem')) {
    $plan = (Invoke-Driver @($command, '--dry-run')).data.result
    $expectedPasses = if ($command -eq 'lowmem') { @('cache16-avx2', 'cache32-avx2') } else { @("$command-avx2") }
    if (($plan.plans.variant -join ',') -ne ($expectedPasses -join ',') -or $plan.quick -or
        @($plan.plans | Where-Object configuration -ne 'release').Count) { throw "Wrong defaults for $command." }
}
$null = Invoke-Driver @('check', '--arch', 'invalid', '--dry-run') 2
$null = Invoke-Driver @('check', '--config', 'invalid', '--dry-run') 2
$null = Invoke-Driver @('check', '--unknown', 'true', '--dry-run') 2
$null = Invoke-Driver @('audit') 2
$stateAfter = @(Get-ChildItem (Join-Path $root '.dd/state') -File -ErrorAction SilentlyContinue |
    ForEach-Object { "$($_.Name):$((Get-FileHash $_.FullName).Hash)" })
if (($stateBefore -join "`n") -ne ($stateAfter -join "`n")) { throw 'Dry-runs and invalid requests must not change build state.' }
Write-Host 'PASS pinned runtime, presets, library selection, variant plans, validation and non-mutating dry-runs'
