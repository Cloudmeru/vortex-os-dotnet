#Requires -Version 7.0
<#
.SYNOPSIS
    End-to-end engine unit tests (PowerShell edition).

.DESCRIPTION
    Runs the new v0.1.9 commands end-to-end through skill.ps1 and asserts
    the expected outcomes. This is the runnable counterpart to test_engine.cpp
    (which can't be launched due to a known .NET 5+ /SUBSYSTEM bug; see
    src/build-tests.ps1 for details).

    Tests:
      1. Engine version reports 0.1.9
      2. Decisions on a fresh home
      3. Decision record + list round-trip
      4. Packager dry-run + real run
      5. Packager refuses to overwrite existing files
      6. Golden Path template replay (item 2)

    Exits 0 on success, 1 on any failure.
#>
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$skillPath = Join-Path $root '..\vortex-os-skill\skill.ps1'
if (-not (Test-Path $skillPath)) { throw "skill.ps1 not found at $skillPath" }

# Use a scratch VORTEX_HOME so we don't pollute the real one.
$scratchHome = Join-Path $env:TEMP "vortex-test-" + (New-Guid).ToString('N').Substring(0, 8)
$env:VORTEX_HOME = $scratchHome
$env:VORTEX_NO_AUTO_UPDATE = '1'
[IO.Directory]::CreateDirectory($scratchHome) | Out-Null

$g_pass = 0
$g_fail = 0
function Check {
    param([string]$Label, [scriptblock]$Test)
    $result = & $Test
    if ($result) {
        $script:g_pass++
        Write-Host "  PASS  $Label"
    } else {
        $script:g_fail++
        Write-Host "  FAIL  $Label" -ForegroundColor Red
    }
}

try {
    Write-Host "VORTEX-OS engine tests (PowerShell edition) v0.1.9"
    Write-Host "==================================================="
    Write-Host "VORTEX_HOME: $scratchHome"
    Write-Host ""

    # -----------------------------------------------------------------------
    # 1. --version reports 0.1.9
    # -----------------------------------------------------------------------
    Write-Host "[1] Engine version"
    $ver = & pwsh -NoProfile -File $skillPath --version 2>&1 | Select-Object -Last 1
    Check "engine version reports 0.1.9" { $ver -match '0\.1\.9' }

    # -----------------------------------------------------------------------
    # 2. --decision-list on a fresh home
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[2] Decisions on a fresh home"
    $list = (& pwsh -NoProfile -File $skillPath --decision-list 2>&1 | Out-String)
    Check "decision-list on empty home says (no decisions)" { $list -match 'no decisions recorded yet' }

    # -----------------------------------------------------------------------
    # 3. --decision-record + --decision-list round-trip
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[3] Decision record + list round-trip"
    & pwsh -NoProfile -File $skillPath --decision-record --task t1 --gate g1 --severity HIGH --choice "approve script" --reason "looks good" --episode 1 2>&1 | Out-Null
    & pwsh -NoProfile -File $skillPath --decision-record --task t1 --gate g2_moral_hinge --severity CRITICAL --choice "deepen the peril" --reason "operator pick" --episode 1 2>&1 | Out-Null
    & pwsh -NoProfile -File $skillPath --decision-record --task t2 --gate g1 --severity HIGH --choice "approve script" --reason "ep2" --episode 2 2>&1 | Out-Null
    $list = (& pwsh -NoProfile -File $skillPath --decision-list 2>&1 | Out-String)
    Check "decision-list shows the CRITICAL decision text" { $list -match 'deepen the peril' }
    Check "decision-list shows the gate name" { $list -match 'g2_moral_hinge' }
    Check "decision-list shows a timestamp" { ($list -match '2026-') -or ($list -match '20[0-9]{2}-') }

    # The decision_history.json file should be valid JSON.
    $historyFile = Join-Path $scratchHome 'state\decision_history.json'
    Check "decision_history.json was created" { Test-Path $historyFile }
    $json = Get-Content $historyFile -Raw | ConvertFrom-Json
    Check "history has 3 entries" { $json.decisions.Count -eq 3 }

    # -----------------------------------------------------------------------
    # 4. --package dry-run + real run
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[4] Packager dry-run + real run"
    # Build a synthetic swarm deliverables/ dir.
    $swarmId = 'test_swarm_packaging'
    $delivDir = Join-Path $scratchHome "swarms\active_$swarmId\deliverables"
    [IO.Directory]::CreateDirectory($delivDir) | Out-Null
    'Episode 1 script body'           | Set-Content (Join-Path $delivDir 'script.md') -Encoding UTF8
    '{"protagonist":"Eira"}'           | Set-Content (Join-Path $delivDir 'character_bible_delta.json') -Encoding UTF8
    'fake-wav-bytes'                   | Set-Content (Join-Path $delivDir 'ambient.wav') -Encoding UTF8 -NoNewline

    # Dry run
    $dryOut = (& pwsh -NoProfile -File $skillPath -Project pkg_test --package $swarmId --dry-run 2>&1 | Out-String)
    Check "packager --dry-run prints DRY RUN" { $dryOut -match 'DRY RUN' }
    Check "packager --dry-run lists script.md" { $dryOut -match 'would copy: script.md' }
    $dryDest = Join-Path $scratchHome 'deliverables\pkg_test'
    Check "no files copied on dry run" { -not (Test-Path (Join-Path $dryDest 'script.md')) }

    # Real run
    $realOut = (& pwsh -NoProfile -File $skillPath -Project pkg_test --package $swarmId 2>&1 | Out-String)
    Check "packager real run copies script.md" { $realOut -match 'Copied: script.md' }
    Check "packager real run copies character_bible_delta.json" { $realOut -match 'Copied: character_bible_delta.json' }
    Check "packager real run copies ambient.wav" { $realOut -match 'Copied: ambient.wav' }
    Check "script.md now exists in deliverables/" { Test-Path (Join-Path $dryDest 'script.md') }
    Check ".manifest.json now exists in deliverables/" { Test-Path (Join-Path $dryDest '.manifest.json') }

    $manifest = Get-Content (Join-Path $dryDest '.manifest.json') -Raw | ConvertFrom-Json
    Check "manifest.swarm_id is $swarmId" { $manifest.swarm_id -eq $swarmId }
    Check "manifest.project is pkg_test" { $manifest.project -eq 'pkg_test' }
    Check "manifest.engine_version is 0.1.9" { $manifest.engine_version -eq '0.1.9' }
    Check "manifest.summary.copied is 3" { $manifest.summary.copied -eq 3 }
    Check "manifest.summary.skipped is 0" { $manifest.summary.skipped -eq 0 }
    Check "manifest.files has 3 entries" { $manifest.files.Count -eq 3 }
    Check "checksum is 16 hex chars" { $manifest.files[0].checksum.Length -eq 16 }

    # -----------------------------------------------------------------------
    # 5. --package refuses to overwrite (idempotency / refuse-to-overwrite per ADR-015)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[5] Packager refuses to overwrite existing files"
    $reOut = (& pwsh -NoProfile -File $skillPath -Project pkg_test --package $swarmId 2>&1 | Out-String)
    Check "packager refuses to overwrite on second run" { $reOut -match 'EXISTS, refusing to overwrite' }

    # -----------------------------------------------------------------------
    # 6. --dispatch-template renders the template + carries prior decision
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[6] Golden Path template replay"
    $templatePath = Join-Path $root '..\vortex-os-skill\templates\episode_pattern.json'
    if (-not (Test-Path $templatePath)) { throw "template not found at $templatePath" }
    $out = (& pwsh -NoProfile -File $skillPath -Project trial_of_echoes `
        --dispatch-template $templatePath `
        --episode-number 2 `
        --task ep2_smoke `
        --protagonist='Eira Vance' `
        --antagonist='Director Hale' `
        --setting='Solstice Bay' `
        --diegetic-clock='11 days after the seal was opened' 2>&1 | Out-String)
    Check "template replay prints the prior decision warning" { $out -match 'Prior CRITICAL-gate decision' }
    Check "template replay writes the rendered objective" { $out -match 'Wrote rendered objective' }
    Check "template replay carries the prior decision text into operator_choice" { $out -match 'deepen the peril' }

    $renderedFile = Join-Path $scratchHome 'tasks\ep2_smoke.md'
    Check "rendered objective file exists" { Test-Path $renderedFile }
    $body = Get-Content $renderedFile -Raw
    Check "rendered body has the episode number" { $body -match 'Episode 2' }
    Check "rendered body has the protagonist name" { $body -match 'Protagonist: Eira Vance' }
    Check "rendered body has the antagonist name" { $body -match 'Antagonist: Director Hale' }
    Check "rendered body has the setting" { $body -match 'Setting: Solstice Bay' }
    Check "rendered body has the diegetic clock" { $body -match 'Diegetic clock: 11 days' }
    Check "rendered body has the prior operator decision" { $body -match 'Prior operator decision: deepen the peril' }

    # -----------------------------------------------------------------------
    # Summary
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "==================================================="
    Write-Host "Passed: $g_pass    Failed: $g_fail"
    Write-Host ""
    if ($g_fail -gt 0) {
        Write-Host "TESTS FAILED" -ForegroundColor Red
        exit 1
    }
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
    exit 0
}
finally {
    # Clean up the scratch home.
    if (Test-Path $scratchHome) {
        try { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($scratchHome, 'OnlyErrorDialogs', 'SendToRecycleBin') } catch {}
    }
}
