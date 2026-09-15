@{
    schema = 1
    project = @{
        name = 'spike-db'
        type = 'library'
        'default-target' = 'test_spike_db'
    }
    dependencies = @{ owner = 'application' }
    build = @{
        'x64-windows' = @{ debug = 'windows-debug'; release = 'windows-release' }
        'x64-linux' = @{ debug = 'linux-debug'; release = 'linux-release' }
    }
    targets = @(
        @{
            id = 'spike_db'
            kind = 'library'
            'cmake-target' = 'spike_db'
            'test-label' = 'spike_db'
            'debug-path' = 'build/{platform}/debug/{libprefix}spike_db{lib}'
            'release-path' = 'build/{platform}/release/{libprefix}spike_db{lib}'
        },
        @{
            id = 'test_spike_db'
            kind = 'cli'
            'cmake-target' = 'test_spike_db'
            'test-label' = 'spike_db'
            'debug-path' = 'build/{platform}/debug/test_spike_db{exe}'
            'release-path' = 'build/{platform}/release/test_spike_db{exe}'
        }
    )
    commands = @{
        check = @{
            description = 'Build and run the full suite for one architecture and configuration.'
            script = 'scripts/verify.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 1800
            parameters = @{
                arch = @{ type = 'string'; choices = @('AVX2', 'AVX512'); default = 'AVX2' }
                config = @{ type = 'string'; choices = @('Release', 'Debug'); default = 'Release' }
                quick = @{ type = 'boolean'; default = $false; description = 'Legacy hint; the full suite still runs.' }
            }
        }
        audit = @{
            description = 'Build with SPIKEDB_AUDIT and run the full suite.'
            script = 'scripts/verify.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 1800
            parameters = @{
                arch = @{ type = 'string'; choices = @('AVX2', 'AVX512'); default = 'AVX2' }
                config = @{ type = 'string'; choices = @('Release', 'Debug'); default = 'Release' }
                quick = @{ type = 'boolean'; default = $false; description = 'Legacy hint; the full suite still runs.' }
            }
        }
        asan = @{
            description = 'Run the full suite with ASan (and UBSan on GCC).'
            script = 'scripts/verify.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 1800
            parameters = @{
                arch = @{ type = 'string'; choices = @('AVX2', 'AVX512'); default = 'AVX2' }
                config = @{ type = 'string'; choices = @('Release', 'Debug'); default = 'Release' }
                quick = @{ type = 'boolean'; default = $false; description = 'Legacy hint; the full suite still runs.' }
            }
        }
        lowmem = @{
            description = 'Run the full suite with 16- and 32-page caches.'
            script = 'scripts/verify.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 1800
            parameters = @{
                arch = @{ type = 'string'; choices = @('AVX2', 'AVX512'); default = 'AVX2' }
                config = @{ type = 'string'; choices = @('Release', 'Debug'); default = 'Release' }
                quick = @{ type = 'boolean'; default = $false; description = 'Legacy hint; the full suite still runs.' }
            }
        }
        all = @{
            description = 'Run check, audit, both low-memory passes and sanitizers.'
            script = 'scripts/verify.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 7200
            parameters = @{
                arch = @{ type = 'string'; choices = @('AVX2', 'AVX512'); default = 'AVX2' }
                config = @{ type = 'string'; choices = @('Release', 'Debug'); default = 'Release' }
                quick = @{ type = 'boolean'; default = $false; description = 'Legacy hint; the full suite still runs.' }
            }
        }
    }
}
