<#
.SYNOPSIS
    SpikeDB developer driver.

.DESCRIPTION
    One entry point for building and testing SpikeDB. Builds with CMake +
    Ninja; on Windows it locates Visual Studio and imports the MSVC
    environment itself, because Ninja drives cl.exe directly and needs it.

    Commands:
      run      build Release and run the test binary   (default)
      build    build only
      test     alias for run
      audit    build with the structural audit hook (-DSPIKEDB_AUDIT) and run
      asan     build with AddressSanitizer + UBSan and run
      lowmem   build and run with a 16- and a 32-page cache
      all      run, audit, lowmem, and asan where supported
      clean    delete build output and scratch files
      help     this text

.PARAMETER Arch
    AVX2 (default) or AVX512.

.PARAMETER Config
    Release (default) or Debug.

.EXAMPLE
    ./dd.ps1 run
.EXAMPLE
    ./dd.ps1 all -Arch AVX512
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('run', 'build', 'test', 'audit', 'asan', 'lowmem', 'all', 'clean', 'help')]
    [string]$Command = 'run',

    [ValidateSet('AVX2', 'AVX512')]
    [string]$Arch = 'AVX2',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    # Accepted for CI compatibility; sets SPIKEDB_TEST_MODE=quick.
    [switch]$Quick
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$OnWindows = $env:OS -eq 'Windows_NT'      # $IsWindows does not exist in PS 5.1
$ExeName = if ($OnWindows) { 'test_spike_db.exe' } else { 'test_spike_db' }

# A CMake cache records absolute paths, so a Windows build and a WSL build of
# the same working tree cannot share a directory.
$Platform = if ($OnWindows) { 'win' } else { 'lin' }

function Write-Step($text) {
    Write-Host ''
    Write-Host "==> $text" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# Toolchain. Ninja invokes cl.exe directly and relies on INCLUDE/LIB, which
# only vcvars sets, so import it unless we are already inside a dev shell.
# ---------------------------------------------------------------------------
function Initialize-MsvcEnvironment {
    if (-not $OnWindows -or $env:VCINSTALLDIR) { return }

    $vcvars = $null
    foreach ($year in @('18', '2026', '2025', '2024', '2022', '2019')) {
        foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
            $candidate = "C:\Program Files\Microsoft Visual Studio\$year\$edition\VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $candidate) { $vcvars = $candidate; break }
        }
        if ($vcvars) { break }
    }
    if (-not $vcvars) {
        if (Get-Command cl -ErrorAction SilentlyContinue) { return }
        throw 'Could not find vcvarsall.bat or cl.exe. Install Visual Studio or run from a Developer PowerShell.'
    }

    cmd /c "`"$vcvars`" x64 >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

# cmake and ninja are usually only inside the VS install, not on PATH.
function Find-Tool([string]$name) {
    $found = Get-Command $name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    if (-not $OnWindows) { throw "$name not found on PATH." }

    $vsRoots = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Directory -ErrorAction SilentlyContinue |
               ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue }
    $suffix = switch ($name) {
        'cmake' { 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
        'ninja' { 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' }
        default { $null }
    }
    foreach ($root in $vsRoots) {
        $candidate = Join-Path $root.FullName $suffix
        if (Test-Path $candidate) { return $candidate }
    }
    throw "$name not found on PATH or in any Visual Studio install."
}

$script:CMake = $null
$script:Ninja = $null

function Initialize-Toolchain {
    if ($script:CMake) { return }
    Initialize-MsvcEnvironment
    $script:CMake = Find-Tool 'cmake'
    $script:Ninja = Find-Tool 'ninja'
}

# ---------------------------------------------------------------------------
# Build / run
# ---------------------------------------------------------------------------
function Invoke-Build([string]$dir, [string[]]$options) {
    Initialize-Toolchain

    $args = @(
        '-S', '.', '-B', $dir, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$script:Ninja",
        "-DCMAKE_BUILD_TYPE=$Config"
    )
    if ($Arch -eq 'AVX512') { $args += '-DSPIKEDB_AVX512=ON' }
    $args += $options

    # Configure output is noise unless it fails.
    $log = & $script:CMake @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | Write-Host
        throw "configure failed ($dir)"
    }

    & $script:CMake --build $dir
    if ($LASTEXITCODE -ne 0) { throw "build failed ($dir)" }
}

function Invoke-Suite([string]$dir, [string]$label) {
    $exe = Join-Path $dir $ExeName
    if (-not (Test-Path $exe)) { throw "missing $exe" }

    if ($Quick) { $env:SPIKEDB_TEST_MODE = 'quick' }
    else { Remove-Item Env:SPIKEDB_TEST_MODE -ErrorAction SilentlyContinue }

    Write-Step "Running $label"
    & $exe
    $rc = $LASTEXITCODE

    Remove-Item 'tmp/test_spike_db.dat' -ErrorAction SilentlyContinue
    Remove-Item 'tmp/test_spike_db.dat.lock' -ErrorAction SilentlyContinue

    if ($rc -ne 0) { throw "$label FAILED (exit $rc)" }
}

function Invoke-Variant([string]$dir, [string[]]$options, [string]$label) {
    Write-Step "Building $label"
    Invoke-Build $dir $options
    Invoke-Suite $dir $label
}

# ---------------------------------------------------------------------------
switch ($Command) {

    'help' {
        Get-Help $PSCommandPath -Detailed
    }

    'clean' {
        Write-Step 'Cleaning'
        Remove-Item 'build' -Recurse -Force -ErrorAction SilentlyContinue
        Get-ChildItem 'tmp' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.dat', '.lock', '.txt' } |
            Remove-Item -Force
        Write-Host 'Removed build/ and tmp/ scratch files.'
    }

    'build' {
        $dir = "build/$Platform-$($Config.ToLower())-$($Arch.ToLower())"
        Write-Step "Building $Config $Arch"
        Invoke-Build $dir @()
        Write-Host "Built $(Join-Path $dir $ExeName)"
    }

    { $_ -in 'run', 'test' } {
        $dir = "build/$Platform-$($Config.ToLower())-$($Arch.ToLower())"
        Write-Step "Building $Config $Arch"
        Invoke-Build $dir @()
        Invoke-Suite $dir "$Config $Arch"
        Write-Host ''
        Write-Host 'ALL TESTS PASSED' -ForegroundColor Green
    }

    'audit' {
        Invoke-Variant "build/$Platform-audit" @('-DSPIKEDB_AUDIT=ON') 'audit build'
        Write-Host ''
        Write-Host 'AUDIT BUILD PASSED' -ForegroundColor Green
    }

    'asan' {
        Invoke-Variant "build/$Platform-asan" @('-DSPIKEDB_SANITIZE=ON') 'sanitizer build'
        Write-Host ''
        Write-Host 'SANITIZER BUILD PASSED' -ForegroundColor Green
    }

    'lowmem' {
        # A leaked page pin is invisible at the default cache size and only
        # surfaces once the clock sweep runs out of victims.
        foreach ($pages in @(16, 32)) {
            Invoke-Variant "build/$Platform-cache$pages" @("-DSPIKEDB_TEST_CACHE=$pages") "$pages-page cache"
        }
        Write-Host ''
        Write-Host 'LOW-MEMORY PASSES PASSED' -ForegroundColor Green
    }

    'all' {
        & $PSCommandPath run    -Arch $Arch -Config $Config
        & $PSCommandPath audit  -Arch $Arch -Config $Config
        & $PSCommandPath lowmem -Arch $Arch -Config $Config
        if (-not $OnWindows) { & $PSCommandPath asan -Arch $Arch -Config $Config }
        Write-Host ''
        Write-Host 'EVERYTHING PASSED' -ForegroundColor Green
    }
}
