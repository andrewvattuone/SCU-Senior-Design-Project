$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

function CacheSimPath {
    if (Test-Path ".\cache_sim_skel\cache_sim.exe") {
        return ".\cache_sim_skel\cache_sim.exe"
    }
    return ".\cache_sim_skel\cache_sim"
}

function Write-TestFile($path, $content) {
    Set-Content -LiteralPath $path -Value $content -NoNewline
}

function Run-Sim($config, $trace) {
    $exe = CacheSimPath
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $exe $config $trace 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    return @{ Code = $code; Output = ($output -join "`n") }
}

function Assert-True($condition, $message) {
    if (-not $condition) {
        throw "FAIL: $message"
    }
}

$tmp = Join-Path $root "tests\_tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

gcc -o cache_sim_skel\cache_sim.exe cache_sim_skel\main.c cache_sim_skel\cache.c cache_sim_skel\util.c -lm
Assert-True ($LASTEXITCODE -eq 0) "cache_sim builds from current sources"

gcc -o tests\rl_unit_tests.exe tests\rl_unit_tests.c cache_sim_skel\cache.c cache_sim_skel\util.c -lm
Assert-True ($LASTEXITCODE -eq 0) "RL unit tests compile"
& .\tests\rl_unit_tests.exe
Assert-True ($LASTEXITCODE -eq 0) "C RL unit tests exited successfully"

$sixLineConfig = Join-Path $tmp "six_line_lru.cfg"
$ordinaryTrace = Join-Path $tmp "ordinary.trace"
Write-TestFile $sixLineConfig @"
16
2
6
3
2
1
"@
Write-TestFile $ordinaryTrace @"
R 0x0
W 0x4 0xaa
R 0x8
R 0x0
"@
$run = Run-Sim $sixLineConfig $ordinaryTrace
Assert-True ($run.Code -eq 0) "six-line config and ordinary trace run with RL disabled"
Assert-True ($run.Output -notmatch "RL Statistics") "RL disabled does not print RL statistics"

$metaTrace = Join-Path $tmp "metadata_disabled.trace"
Write-TestFile $metaTrace @"
R 0x0 M 1
W 0x4 0xaa S 3
R 0x8 E 1
"@
$run = Run-Sim $sixLineConfig $metaTrace
Assert-True ($run.Code -eq 0) "optional metadata is accepted with RL disabled"
Assert-True ($run.Output -notmatch "RL Statistics") "optional metadata does not enable RL"

foreach ($policy in 0, 1, 2, 3, 5) {
    $plainConfig = Join-Path $tmp "plain_policy_$policy.cfg"
    Write-TestFile $plainConfig @"
16
2
6
3
2
$policy
"@
    $run = Run-Sim $plainConfig $ordinaryTrace
    Assert-True ($run.Code -eq 0) "plain policy $policy runs with RL disabled"
    Assert-True ($run.Output -notmatch "RL Statistics") "plain policy $policy does not use RL"
}

$rlConfig = Join-Path $tmp "rl_state.cfg"
$rlTrace = Join-Path $tmp "rl_state.trace"
Write-TestFile $rlConfig @"
16
2
6
3
2
1
1
0.5
4
"@
Write-TestFile $rlTrace @"
R 0x0 M 1
R 0x4 E 1
R 0x8 S 3
R 0x0 M 1
"@
$run = Run-Sim $rlConfig $rlTrace
Assert-True ($run.Code -eq 0) "RL-State trace runs"
Assert-True ($run.Output -match "history hits / returned evictions: 1 / 1") "returning history tag counted once"
Assert-True ($run.Output -match "negative rewards: 1") "negative reward is reported"
Assert-True ($run.Output -match "set 0 Q: M -0.500000") "Q update uses alpha"

$invalidConfig = Join-Path $tmp "invalid_random_rl.cfg"
Write-TestFile $invalidConfig @"
16
2
6
3
2
0
1
0.5
4
"@
$run = Run-Sim $invalidConfig $rlTrace
Assert-True ($run.Code -ne 0) "RL rejects unsupported base policies"
Assert-True ($run.Output -match "supported only with LRU or LFU") "unsupported policy error is clear"

$missingMetaTrace = Join-Path $tmp "missing_metadata.trace"
Write-TestFile $missingMetaTrace @"
R 0x0 M 1
R 0x4
"@
$run = Run-Sim $rlConfig $missingMetaTrace
Assert-True ($run.Code -ne 0) "RL rejects missing trace metadata"
Assert-True ($run.Output -match "trace line 2") "missing metadata error names trace line"

$badSharerTrace = Join-Path $tmp "bad_sharer.trace"
Write-TestFile $badSharerTrace @"
R 0x0 M 5
"@
$run = Run-Sim $rlConfig $badSharerTrace
Assert-True ($run.Code -ne 0) "RL rejects sharer_count above num_cores"
Assert-True ($run.Output -match "Invalid sharer_count") "bad sharer error is clear"

# legacy 3-optional-line RL config (rl_mode, rl_alpha, num_cores) still works, epsilon defaults to 0
$run = Run-Sim $rlConfig $rlTrace
Assert-True ($run.Code -eq 0) "legacy 3-line RL config still runs"
Assert-True ($run.Output -match "RL epsilon: 0\.000000") "legacy config defaults epsilon to 0"
Assert-True ($run.Output -match "exploration decisions: 0") "epsilon 0 never explores"

# new 4-optional-line RL config (rl_mode, rl_alpha, rl_epsilon, num_cores)
$epsConfig = Join-Path $tmp "rl_epsilon.cfg"
Write-TestFile $epsConfig @"
16
2
6
3
2
1
1
0.5
1.0
4
"@
$epsTraceLines = for ($i = 0; $i -lt 50; $i++) { "R 0x{0:x} M 1" -f ($i * 4) }
$epsTrace = Join-Path $tmp "rl_epsilon.trace"
Write-TestFile $epsTrace (($epsTraceLines -join "`n") + "`n")
$run = Run-Sim $epsConfig $epsTrace
Assert-True ($run.Code -eq 0) "4-line RL config with epsilon runs"
Assert-True ($run.Output -match "RL epsilon: 1\.000000") "epsilon is printed in cache config"
Assert-True ($run.Output -match "RL decisions: 48") "48 real RL decisions counted"
Assert-True ($run.Output -match "exploration decisions: 48") "epsilon 1 explores on every RL decision"
Assert-True ($run.Output -match "exploitation decisions: 0") "epsilon 1 never exploits"

# invalid epsilon values are rejected
foreach ($badEpsilon in "-0.1", "1.1", "abc") {
    $badConfig = Join-Path $tmp "rl_bad_epsilon.cfg"
    Write-TestFile $badConfig @"
16
2
6
3
2
1
1
0.5
$badEpsilon
4
"@
    $run = Run-Sim $badConfig $rlTrace
    Assert-True ($run.Code -ne 0) "epsilon $badEpsilon is rejected"
    Assert-True ($run.Output -match "Invalid rl_epsilon") "epsilon $badEpsilon error is clear"
}

Write-Host "run_rl_tests: PASS"
