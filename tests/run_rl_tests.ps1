$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

function CacheSimPath {
    if (Test-Path ".\cache_sim.exe") {
        return ".\cache_sim.exe"
    }
    return ".\cache_sim"
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

gcc -I. -o tests\rl_unit_tests.exe tests\rl_unit_tests.c cache.c util.c -lm
& tests\rl_unit_tests.exe
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

Write-Host "run_rl_tests: PASS"
