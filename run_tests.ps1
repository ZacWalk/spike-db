<#
.SYNOPSIS
  Build and run the SpikeDB test suite.
.PARAMETER Arch
  AVX2 (default) or AVX512.
#>
param(
    [ValidateSet("AVX2", "AVX512")]
    [string]$Arch = "AVX2"
)

$ErrorActionPreference = "Stop"

Write-Host "======================================================"
Write-Host "  SpikeDB  -  Build & Test"
Write-Host "======================================================"
Write-Host ""

$SRC  = "src/spike_db.c"
$TEST = "src/test_spike_db.c"
$OUT  = "build"

if (-not (Test-Path $OUT)) { New-Item -ItemType Directory -Path $OUT | Out-Null }

if ($IsLinux) {
    # ---------- GCC build (Linux / WSL) ----------
    $flags = @("-O2", "-mavx2", "-msse4.2", "-I", "src")
    if ($Arch -eq "AVX512") {
        $flags += "-mavx512f"
        $flags += "-D_SPIKEDB_COMPILE_AVX512"
        Write-Host "[*] Building with GCC (AVX-512)"
    } else {
        Write-Host "[*] Building with GCC (AVX2)"
    }
    Write-Host ""
    & gcc @flags $SRC $TEST -o "$OUT/test_spike_db"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n*** BUILD FAILED ***"
        exit 1
    }
    $exe = "$OUT/test_spike_db"
} else {
    # ---------- Locate VS Developer Environment ----------
    $vcvars = $null
    foreach ($year in @("2026", "2025", "2024", "2022", "2019")) {
        foreach ($edition in @("Enterprise", "Professional", "Community", "BuildTools")) {
            $candidate = "C:\Program Files\Microsoft Visual Studio\$year\$edition\VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $candidate) {
                $vcvars = $candidate
                break
            }
        }
        if ($vcvars) { break }
    }

    if (-not $vcvars) {
        # Check if cl.exe is already on PATH (running from Developer PS)
        $cl = Get-Command cl -ErrorAction SilentlyContinue
        if (-not $cl) {
            Write-Host "ERROR: Could not find vcvarsall.bat or cl.exe.  Run from a Developer PowerShell."
            exit 1
        }
        Write-Host "[*] Using cl.exe already on PATH"
    } else {
        Write-Host "[*] Using: $vcvars"
        # Import the MSVC environment into the current PowerShell session
        cmd /c "`"$vcvars`" x64 >nul 2>&1 && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
        Write-Host "[*] Environment: x64"
    }

    Write-Host ""

    $archFlag = if ($Arch -eq "AVX512") { "/arch:AVX512" } else { "/arch:AVX2" }
    $clArgs = @("/nologo", "/W4", "/O2", $archFlag)
    if ($Arch -eq "AVX512") {
        $clArgs += "/D_SPIKEDB_COMPILE_AVX512"
        Write-Host "[*] Building with MSVC (AVX-512)"
    } else {
        Write-Host "[*] Building with MSVC (AVX2)"
    }
    $clArgs += @("/I", "src", $SRC, $TEST, "/Fe:$OUT\test_spike_db.exe", "/Fo:$OUT\", "/link", "/SUBSYSTEM:CONSOLE")
    & cl @clArgs

    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n*** BUILD FAILED ***"
        exit 1
    }
    $exe = "$OUT\test_spike_db.exe"
}

Write-Host ""
Write-Host "[*] Build OK"
Write-Host ""

# ---------- Run tests ----------
Write-Host "======================================================"
Write-Host "  Running tests ..."
Write-Host "======================================================"
Write-Host ""

& $exe
$testRC = $LASTEXITCODE

Write-Host ""
if ($testRC -eq 0) {
    Write-Host "======================================================"
    Write-Host "  ALL TESTS PASSED"
    Write-Host "======================================================"
} else {
    Write-Host "======================================================"
    Write-Host "  SOME TESTS FAILED  (exit code $testRC)"
    Write-Host "======================================================"
}

# Clean up temp db file if present
if (Test-Path "test_spike_db.dat")  { Remove-Item "test_spike_db.dat" -Force }
if (Test-Path "test_spike_db.dat.lock") { Remove-Item "test_spike_db.dat.lock" -Force }

exit $testRC
