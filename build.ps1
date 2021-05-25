<#
.SYNOPSIS
  Build SpikeDB with MSVC (Windows) or GCC (Linux/WSL).
.PARAMETER Arch
  AVX2 (default) or AVX512.
#>
param(
    [ValidateSet("AVX2", "AVX512")]
    [string]$Arch = "AVX2"
)

$ErrorActionPreference = "Stop"

$SRC  = "src/spike_db.c"
$TEST = "src/test_spike_db.c"
$OUT  = "build"

if (-not (Test-Path $OUT)) { New-Item -ItemType Directory -Path $OUT | Out-Null }

if ($IsLinux) {
    # GCC build (Linux / WSL)
    $flags = @("-O2", "-mavx2", "-msse4.2", "-I", "src")
    if ($Arch -eq "AVX512") {
        $flags += "-mavx512f"
        $flags += "-D_SPIKEDB_COMPILE_AVX512"
        Write-Host "[*] Building with GCC (AVX-512)"
    } else {
        Write-Host "[*] Building with GCC (AVX2)"
    }
    & gcc @flags $SRC $TEST -o "$OUT/test_spike_db"
} else {
    # MSVC build (Windows)
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
}

if ($LASTEXITCODE -eq 0) {
    $exe = if ($IsLinux) { "$OUT/test_spike_db" } else { "$OUT\test_spike_db.exe" }
    Write-Host "`nBuild succeeded: $exe"
} else {
    Write-Host "Build FAILED."
    exit 1
}
