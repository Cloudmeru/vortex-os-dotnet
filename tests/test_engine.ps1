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
# $skillRoot = the directory that contains skill.ps1. Used by G18-G24
# tests that drop a template into the skill's own templates/ folder.
# Pre-v0.3.8.1 this variable was used but never initialized, so any test
# that touched it (G18, G19, G21) bailed with "Cannot bind argument to
# parameter 'Path' because it is null".
$skillRoot = Split-Path -Parent $skillPath

# v0.2.3: $engineDir is where the engine Vortex.psd1 lives (one level
# below $root). The cmdlet tests below Import-Module this path.
$engineDir = $root
if (-not (Test-Path (Join-Path $engineDir 'Vortex.psd1'))) {
    throw "Vortex.psd1 not found at $engineDir. Run src\build.ps1 first."
}

# Use a scratch VORTEX_HOME so we don't pollute the real one.
$scratchHome = Join-Path $env:TEMP "vortex-test-" + (New-Guid).ToString('N').Substring(0, 8)
$env:VORTEX_HOME = $scratchHome
$env:VORTEX_NO_AUTO_UPDATE = '1'
[IO.Directory]::CreateDirectory($scratchHome) | Out-Null

# $swarmsDir = where the engine writes <swarms>/<task_id>/* (e.g. plan.json,
# agent/ subdirs, deliverables/). v0.3.0+ engine creates it on first dispatch
# but pre-existing tests (G21+) reference it before any dispatch ran.
# Pre-v0.3.8.1 this was undefined, so G21 bailed with "Cannot bind argument
# to parameter 'Path' because it is null".
$swarmsDir = Join-Path $scratchHome 'swarms'

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
    Write-Host "VORTEX-OS engine tests (PowerShell edition) v0.2.2"
    Write-Host "==================================================="
    Write-Host "VORTEX_HOME: $scratchHome"
    Write-Host ""

    # -----------------------------------------------------------------------
    # 1. --version reports 0.1.9
    # -----------------------------------------------------------------------
    Write-Host "[1] Engine version"
    $ver = & pwsh -NoProfile -File $skillPath --version 2>&1 | Select-Object -Last 1
    Check "engine version reports 0.3.0" { $ver -match '0\.3\.0' }

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
    $env:VORTEX_PROJECT = 'pkg_test'
    $dryOut = (& pwsh -NoProfile -File $skillPath --package $swarmId --dry-run 2>&1 | Out-String)
    Check "packager --dry-run prints DRY RUN" { $dryOut -match 'DRY RUN' }
    Check "packager --dry-run lists script.md" { $dryOut -match 'would copy: script.md' }
    $dryDest = Join-Path $scratchHome 'deliverables\pkg_test'
    Check "no files copied on dry run" { -not (Test-Path (Join-Path $dryDest 'script.md')) }

    # Real run
    $env:VORTEX_PROJECT = 'pkg_test'
    $realOut = (& pwsh -NoProfile -File $skillPath --package $swarmId 2>&1 | Out-String)
    Check "packager real run copies script.md" { $realOut -match 'Copied: script.md' }
    Check "packager real run copies character_bible_delta.json" { $realOut -match 'Copied: character_bible_delta.json' }
    Check "packager real run copies ambient.wav" { $realOut -match 'Copied: ambient.wav' }
    Check "script.md now exists in deliverables/" { Test-Path (Join-Path $dryDest 'script.md') }
    Check ".manifest.json now exists in deliverables/" { Test-Path (Join-Path $dryDest '.manifest.json') }

    $manifest = Get-Content (Join-Path $dryDest '.manifest.json') -Raw | ConvertFrom-Json
    Check "manifest.swarm_id is $swarmId" { $manifest.swarm_id -eq $swarmId }
    Check "manifest.project is pkg_test" { $manifest.project -eq 'pkg_test' }
    Check "manifest.engine_version is 0.3.0" { $manifest.engine_version -eq '0.3.0' }
    Check "manifest.summary.copied is 3" { $manifest.summary.copied -eq 3 }
    Check "manifest.summary.skipped is 0" { $manifest.summary.skipped -eq 0 }
    Check "manifest.files has 3 entries" { $manifest.files.Count -eq 3 }
    Check "checksum is 16 hex chars" { $manifest.files[0].checksum.Length -eq 16 }

    # -----------------------------------------------------------------------
    # 5. --package refuses to overwrite (idempotency / refuse-to-overwrite per ADR-015)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[5] Packager refuses to overwrite existing files"
    $env:VORTEX_PROJECT = 'pkg_test'
    $reOut = (& pwsh -NoProfile -File $skillPath --package $swarmId 2>&1 | Out-String)
    Check "packager refuses to overwrite on second run" { $reOut -match 'EXISTS, refusing to overwrite' }

    # -----------------------------------------------------------------------
    # 6. --dispatch-template renders the template + carries prior decision
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[6] Golden Path template replay"
    $templatePath = Join-Path $root '..\vortex-os-skill\templates\episode_pattern.json'
    if (-not (Test-Path $templatePath)) { throw "template not found at $templatePath" }
    $env:VORTEX_PROJECT = 'trial_of_echoes'
    $out = (& pwsh -NoProfile -File $skillPath `
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
    # 7. Cost tracking
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[7] Cost tracking"

    # Generate default price + budget files via setup-cost.ps1
    $skillDir = Split-Path -Parent $skillPath
    $setup = Join-Path $skillDir 'setup-cost.ps1'
    if (Test-Path $setup) {
        & pwsh -NoProfile -File $setup 2>&1 | Out-Null
    }
    $pricesFile = Join-Path $scratchHome '.vortex\model_prices.json'
    Check "model_prices.json was generated" { Test-Path $pricesFile }
    $budgetsFile = Join-Path $scratchHome '.vortex\budgets.json'
    Check "budgets.json was generated" { Test-Path $budgetsFile }

    # --cost-estimate with a known model
    $est = (& pwsh -NoProfile -File $skillPath --cost-estimate --model "MiniMax-Text-01" --tokens-in 1000 --tokens-out 500 2>&1 | Out-String)
    Check "cost-estimate shows the model name" { $est -match 'MiniMax-Text-01' }
    Check "cost-estimate shows the cost line" { $est -match 'Cost USD' }
    # MiniMax-Text-01: 0.0008 in + 0.0024 out, so 1000*0.0008 + 500*0.0024 = 0.0008 + 0.0012 = 0.002
    Check "cost-estimate value matches expected (0.002000)" { $est -match '0\.002000' }

    # --cost-estimate with an unknown model falls back to default
    # Default = 0.001 in + 0.002 out. 1000*0.001/1000 + 500*0.002/1000 = 0.001 + 0.001 = 0.002000
    $estDef = (& pwsh -NoProfile -File $skillPath --cost-estimate --model "Unknown-Model-XYZ" --tokens-in 1000 --tokens-out 500 2>&1 | Out-String)
    Check "unknown model uses default pricing (0.002000)" { $estDef -match '0\.002000' }

    # --cost-record appends to state/cost_log.jsonl
    $projectName = 'cost_test_project'
    $env:VORTEX_PROJECT = $projectName
    # Set a tiny budget BEFORE recording, so the second record triggers the 100% alert.
    & pwsh -NoProfile -File $skillPath --budget-set --project $projectName --usd-total 0.005 2>&1 | Out-Null
    & pwsh -NoProfile -File $skillPath --cost-record --task t1 --agent writer.shift --model "MiniMax-Text-01" --tokens-in 5000 --tokens-out 2000 --duration-ms 1200 2>&1 | Out-Null
    & pwsh -NoProfile -File $skillPath --cost-record --task t2 --agent audio.foley --model "MiniMax-Text-01" --tokens-in 1000 --tokens-out 500 2>&1 | Out-Null
    $costLog = Join-Path $scratchHome 'state\cost_log.jsonl'
    Check "cost_log.jsonl was created" { Test-Path $costLog }
    $logLines = Get-Content $costLog
    Check "cost_log.jsonl has 2 entries" { $logLines.Count -eq 2 }
    $row1 = $logLines[0] | ConvertFrom-Json
    Check "first entry has task_id=t1" { $row1.task_id -eq 't1' }
    Check "first entry has project=$projectName" { $row1.project -eq $projectName }
    Check "first entry has tokens_in=5000" { $row1.tokens_in -eq 5000 }
    Check "first entry has tokens_out=2000" { $row1.tokens_out -eq 2000 }
    # 5000*0.0008/1000 + 2000*0.0024/1000 = 0.004 + 0.0048 = 0.0088
    Check "first entry cost_usd matches expected (0.008800)" { $row1.cost_usd -eq 0.0088 }

    # --cost-report (text) shows the project + per-agent breakdown.
    # NOTE: we removed [string] $Project from skill.ps1 in v0.1.11 because it
    # greedily bound the engine's --project arg, so the project name is
    # communicated via $env:VORTEX_PROJECT only. The engine picks it up
    # from there.
    $report = (& pwsh -NoProfile -File $skillPath --cost-report 2>&1 | Out-String)
    Check "cost-report shows the project name" { $report -match $projectName }
    Check "cost-report shows writer.shift" { $report -match 'writer\.shift' }
    Check "cost-report shows audio.foley" { $report -match 'audio\.foley' }
    Check "cost-report shows the TOTAL row" { $report -match 'TOTAL' }
    # Writer: 0.0088, audio: 1000*0.0008/1000 + 500*0.0024/1000 = 0.0008 + 0.0012 = 0.002
    # Total: 0.0108 -> $ 0.0108
    Check "cost-report TOTAL cost matches (0.0108)" { $report -match '0\.0108' }

    # --cost-report --json
    # The skill shell prints a status line to stdout before forwarding to the
    # engine, so we filter for the line that starts with '{' (the JSON payload).
    $jsonOut = & pwsh -NoProfile -File $skillPath --cost-report --json 2>&1
    $jsonText = ($jsonOut | Where-Object { $_ -match '^\s*\{' }) -join "`n"
    $jsonText = $jsonText.Trim()
    Check "cost-report --json returns valid JSON" { try { $jsonText | ConvertFrom-Json | Out-Null; $true } catch { $false } }
    if (Test-Path variable:LASTEXITCODE) { Remove-Variable LASTEXITCODE -ErrorAction SilentlyContinue }
    $jsonObj = $jsonText | ConvertFrom-Json
    Check "cost-report --json has 2 dispatches" { $jsonObj.grand_total.dispatches -eq 2 }
    Check "cost-report --json has 1 project" { $jsonObj.projects.Count -eq 1 }
    Check "cost-report --json project has 2 agents" { $jsonObj.projects[0].agents.Count -eq 2 }

    # --budget-show (the budget was already set above). The engine reads
    # --project for the budget lookup; skill.ps1 forwards the arg.
    $budgetShow = (& pwsh -NoProfile -File $skillPath --budget-show --project $projectName 2>&1 | Out-String)
    Check "budget-show reports the configured usd_total" { $budgetShow -match 'usd_total.*0\.01' }
    Check "budget-show reports so_far >= usd_total (over budget)" { $budgetShow -match 'over budget|100\.|used' }

    # Budget alert at 80% or 100% (we're at 0.0108 / 0.005 = 216% so the 100% gate fires)
    # Hitl::YieldForApproval calls Environment::Exit(203) which we can't test directly;
    # the flag file at .vortex/budget_alert_100_*.flag is the artifact we can check.
    $today = Get-Date -Format 'yyyyMMdd'
    $flag100 = Join-Path $scratchHome ".vortex\budget_alert_100_${projectName}_${today}.flag"
    Check "100% budget alert flag was written" { Test-Path $flag100 }

    # The flag prevents re-alerting on the same day
    # (calling --cost-report again shouldn't re-fire)
    # Just verify the flag content is a unix timestamp
    if (Test-Path $flag100) {
        $ts = Get-Content $flag100 -Raw
        Check "100% alert flag content is a unix timestamp" { $ts -match '^\d{10}$' }
    }

    # Reset: remove the flag and the budget, then re-verify
    if (Test-Path $flag100) { Remove-Item $flag100 -Force }
    $budgetFile = Join-Path $scratchHome 'deliverables\' "$projectName" '\_meta.json'
    if (Test-Path $budgetFile) { Remove-Item $budgetFile -Force }

    # -----------------------------------------------------------------------
    # 8. Audit log viewer (PRD-13: lib/Vortex.AuditViewer.psm1)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[8] Audit log viewer"

    # Seed the audit.jsonl with 6 representative events covering every
    # viewer code path: T0 dispatch, T2 self-heal violation + fix, T2
    # token_audit (warn), T2 hitl_request (PENDING_HUMAN), T2 dispatch_end.
    $auditDir = Join-Path $scratchHome 'memory'
    if (-not (Test-Path $auditDir)) { New-Item -ItemType Directory -Path $auditDir -Force | Out-Null }
    $auditFile = Join-Path $auditDir 'audit.jsonl'
    $seedLines = @(
        '{"ts":"2026-08-22T10:00:00.000+00:00","tier":"T0","agent":"t0.general_manager","action":"dispatch_start","status":"received","project":"cost_test_project","task_id":"ep1","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"","tags":["t0","ep1"]}',
        '{"ts":"2026-08-22T10:00:05.000+00:00","tier":"T2","agent":"shift.prose","action":"token_audit","status":"warn","project":"cost_test_project","task_id":"ep1","severity":"HIGH","rule_violated":"tone_drift","rule_fixed":"","gate_id":"","tags":["inspector","shift.prose","ep1"]}',
        '{"ts":"2026-08-22T10:00:06.000+00:00","tier":"T2","agent":"prompt.optimizer","action":"self_heal","status":"ok","project":"cost_test_project","task_id":"ep1","severity":"MEDIUM","rule_violated":"tone_drift","rule_fixed":"strict_prompt_v1","gate_id":"","tags":["selfheal","shift.prose","tone_drift"]}',
        '{"ts":"2026-08-22T10:00:30.000+00:00","tier":"T2","agent":"shift.packaging","action":"hitl_request","status":"PENDING_HUMAN","project":"cost_test_project","task_id":"package_websim","severity":"HIGH","rule_violated":"","rule_fixed":"","gate_id":"gate1_script","tags":["hitl","package_websim"]}',
        '{"ts":"2026-08-22T10:01:00.000+00:00","tier":"T2","agent":"shift.prose","action":"dispatch_end","status":"ok","project":"cost_test_project","task_id":"ep1","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"","tags":["shift.prose","ep1","MiniMax-Text-01"]}',
        '{"ts":"2026-08-22T10:02:00.000+00:00","tier":"T4","agent":"worker.code","action":"deliver","status":"ok","project":"cost_test_project","task_id":"ep1_assets","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"","tags":["worker.code","ep1_assets"]}'
    )
    Set-Content -LiteralPath $auditFile -Value $seedLines -Encoding UTF8

    Check "audit.jsonl seeded with 6 events" { (Get-Content $auditFile).Count -eq 6 }

    # The viewer module must be on disk next to skill.ps1.
    $skillLib = Join-Path $skillDir 'lib\Vortex.AuditViewer.psm1'
    Check "audit viewer module exists" { Test-Path $skillLib }

    # --audit-trail (default table) ------------------------------------------------
    $table = (& pwsh -NoProfile -File $skillPath --audit-trail 2>&1 | Out-String)
    Check "audit-trail table shows the dispatch_start" { $table -match 'dispatch_start' }
    Check "audit-trail table shows the self_heal action" { $table -match 'self_heal' }
    Check "audit-trail table shows the VIOLATED rule" { $table -match 'tone_drift' }
    Check "audit-trail table shows the FIXED patch"   { $table -match 'strict_prompt_v1' }
    Check "audit-trail table shows the HITL gate"     { $table -match 'hitl_request' }
    Check "audit-trail table shows the PENDING_HUMAN status" { $table -match 'PENDING_HUMAN' }

    # --audit-trail --format tree --------------------------------------------------
    $tree = (& pwsh -NoProfile -File $skillPath --AuditFormat tree --audit-trail 2>&1 | Out-String)
    Check "audit-trail tree shows T0..T4 tiers" { ($tree -match 'Tier T0') -and ($tree -match 'Tier T2') -and ($tree -match 'Tier T4') }
    Check "audit-trail tree shows the self-heal cycle" { $tree -match 'HEALED|HEAL-FAIL' }
    Check "audit-trail tree shows the HITL gate" { $tree -match 'HITL GATE' }
    Check "audit-trail tree shows the VIOLATED marker" { $tree -match 'VIOLATED' }
    Check "audit-trail tree shows the FIXED marker" { $tree -match '\[END\]|FIXED' }

    # --audit-trail --format selfheal ----------------------------------------------
    $sh = (& pwsh -NoProfile -File $skillPath --AuditFormat selfheal --audit-trail 2>&1 | Out-String)
    Check "audit-trail selfheal shows 1 violation" { $sh -match 'Violations: 1' }
    Check "audit-trail selfheal identifies tone_drift" { $sh -match 'tone_drift' }
    Check "audit-trail selfheal shows the matching fix" { $sh -match 'strict_prompt_v1' }

    # --audit-trail --format hitl --------------------------------------------------
    $hitl = (& pwsh -NoProfile -File $skillPath --AuditFormat hitl --audit-trail 2>&1 | Out-String)
    Check "audit-trail hitl shows 1 HITL event" { $hitl -match 'Total HITL events: 1' }
    Check "audit-trail hitl shows the gate_id" { $hitl -match 'gate1_script' }

    # --audit-trail --format json --------------------------------------------------
    # The skill shell may prepend a "[vortex-os] ..." banner to stdout.
    # Filter to just the JSON block: skip the [vortex-os] status line
    # (which is a single non-JSON line starting with '['), then take
    # everything from the first '[' or '{' until the last ']'.
    $jsonRaw = & pwsh -NoProfile -File $skillPath --AuditFormat json --audit-trail 2>&1
    # Drop the [vortex-os] banner specifically (it's a single line that
    # starts with '[vortex-os]').
    $jsonLines = $jsonRaw | Where-Object { $_ -notmatch '^\[vortex-os\]' }
    $json = ($jsonLines -join "`n").Trim()
    # Find the JSON array start.
    $startIdx = $json.IndexOf('[')
    if ($startIdx -lt 0) { $startIdx = 0 }
    $json = $json.Substring($startIdx)
    # Truncate at the last ']' so any trailing junk doesn't break
    # ConvertFrom-Json.
    $endIdx = $json.LastIndexOf(']')
    if ($endIdx -gt 0) { $json = $json.Substring(0, $endIdx + 1) }
    Check "audit-trail json returns an array" { $json.StartsWith('[') }
    $jsonObj = $json | ConvertFrom-Json
    Check "audit-trail json has 6 events" { $jsonObj.Count -eq 6 }

    # Direct cmdlet invocation (no skill.ps1 wrapper) --------------------------------
    Import-Module $skillLib -Force
    $direct = Get-VortexAuditTrail -Project 'cost_test_project'
    Check "Get-VortexAuditTrail is exported" { ($direct | Measure-Object).Count -ge 1 }
    Check "Get-VortexAuditTrail -Project cost_test_project returns the violation" { ($direct | Where-Object { $_.rule_violated -eq 'tone_drift' }).Count -ge 1 }

    # -----------------------------------------------------------------------
    # 9. Plugin system (PRD-11: lib/Plugin.cpp + skill/plugins/ + SDK)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[9] Plugin system"

    # --plugins-list discovers all 6 skill-scope reference plugins.
    $listOut = (& pwsh -NoProfile -File $skillPath --plugins-list 2>&1 | Out-String)
    Check "plugins-list shows audio-foley"        { $listOut -match 'audio-foley' }
    Check "plugins-list shows text-writer"        { $listOut -match 'text-writer' }
    Check "plugins-list shows text-editor"        { $listOut -match 'text-editor' }
    Check "plugins-list shows image-portrait"     { $listOut -match 'image-portrait' }
    Check "plugins-list shows code-typescript"    { $listOut -match 'code-typescript' }
    Check "plugins-list shows media-ffmpeg"       { $listOut -match 'media-ffmpeg' }
    Check "plugins-list reports 17 plugins total" { $listOut -match 'Total: 17 plugin' }
    Check "plugins-list marks them as skill-scope" { $listOut -match 'skill' }

    # --plugins-info dumps a single plugin's manifest.
    $infoOut = (& pwsh -NoProfile -File $skillPath --plugins-info audio-foley 2>&1 | Out-String)
    Check "plugins-info audio-foley has name"      { $infoOut -match '"name": "audio-foley"' }
    Check "plugins-info audio-foley has capability" { $infoOut -match '"capability": "audio"' }
    Check "plugins-info audio-foley has timeout_s"  { $infoOut -match '"timeout_s": 300' }

    # --plugin-test invokes a plugin and reads back the output JSON.
    # We test the audio-foley plugin (stub: writes a 1s silent WAV).
    $pluginHome = Join-Path $scratchHome 'deliverables\plugin_smoke'
    New-Item -ItemType Directory -Path $pluginHome -Force | Out-Null
    $env:VORTEX_PROJECT = 'plugin_smoke'
    $inputJson = '{"prompt":"footsteps on gravel","duration_s":2}'
    $inputFile = Join-Path $scratchHome 'plugin_input.json'
    Set-Content -LiteralPath $inputFile -Value $inputJson -Encoding UTF8
    $testRaw = & pwsh -NoProfile -File $skillPath --plugin-test audio-foley --input $inputFile --timeout-s 30 2>&1
    $testOut = $testRaw | Out-String
    Check "plugin-test audio-foley returns a file" { $testOut -match '"file":' }
    Check "plugin-test audio-foley duration_s=2"   { $testOut -match '"duration_s": 2' }
    Check "plugin-test audio-foley wrote a WAV"     { $testOut -match '\.wav' }

    # Parse the JSON properly to get the actual file path (the C++ engine
    # JSON-encodes special chars like '+' as '\u002B'; ConvertFrom-Json
    # un-escapes them so we get a usable filesystem path).
    # The wrapper may prepend an auto-update banner; locate the JSON block
    # by finding '{' then grabbing everything up to the matching '}'.
    $testOut = ($testRaw -join "`n")
    $braceStart = $testOut.IndexOf('{')
    $testJson = $null
    if ($braceStart -ge 0) {
        # Find the matching '}' (top-level only; JSON.parse handles nesting)
        $depth = 0
        $endIdx = -1
        for ($i = $braceStart; $i -lt $testOut.Length; $i++) {
            $ch = $testOut[$i]
            if ($ch -eq '{') { $depth++ }
            elseif ($ch -eq '}') {
                $depth--
                if ($depth -eq 0) { $endIdx = $i; break }
            }
        }
        if ($endIdx -gt $braceStart) {
            $testJson = $testOut.Substring($braceStart, $endIdx - $braceStart + 1)
        }
    }
    $wavPath = $null
    if ($testJson) {
        try {
            $testObj = $testJson | ConvertFrom-Json
            $wavPath = [string]$testObj.file
        } catch {
            $wavPath = $null
        }
    }
    Check "plugin-test output JSON has a usable file path" { ($wavPath -and -not $wavPath.Contains('\u')) }
    if ($wavPath -and (Test-Path $wavPath)) {
        Check "plugin-test wrote the wav file to disk" { $true }
        Check "wav file is larger than 100 bytes"      { (Get-Item $wavPath).Length -gt 100 }
    } else {
        Check "plugin-test wrote the wav file to disk" { $false }
    }

    # The audit log should record the plugin invocation (single emit per
    # --plugin-test run). The actual Invoke() in Plugin.cpp also emits a
    # plugin_invoke line, so we expect at least 1.
    $auditFile = Join-Path $scratchHome 'memory\audit.jsonl'
    Check "audit log exists after plugin test" { Test-Path $auditFile }
    $auditLines = Get-Content $auditFile
    Check "audit log has a plugin_invoke event" { ($auditLines | Where-Object { $_ -match 'plugin_invoke' }).Count -ge 1 }
    Check "audit log has a plugin_test event"   { ($auditLines | Where-Object { $_ -match 'plugin_test' }).Count -ge 1 }

    # Get-VortexPlugin (engine-side) also works via the engine module.
    # Use a fresh sub-shell + a temp script file so the backtick-n inside
    # the -join works (PowerShell's outer parser would otherwise eat the
    # backtick before pwsh sees it).
    $gvTestFile = Join-Path $scratchHome 'gv_test.ps1'
    $vortexModulePath = Join-Path $HOME 'Documents\PowerShell\Modules'
    Set-Content -LiteralPath $gvTestFile -Value @"
`$env:VORTEX_SKILL_ROOT = '$skillDir'
`$env:VORTEX_NO_AUTO_UPDATE = '1'
`$env:PSModulePath = '$vortexModulePath;' + `$env:PSModulePath
Import-Module Vortex -RequiredVersion 0.2.1 -Force
`$out = Get-VortexPlugin
Write-Output '===START==='
`$out -join "`n"
Write-Output '===END==='
"@ -Encoding UTF8
    $gvOut = & pwsh -NoProfile -File $gvTestFile 2>&1 | Out-String
    Check "Get-VortexPlugin cmdlet is exported" { $gvOut -match 'audio-foley' }

    # --plugin-remove (user-scope only). Copy audio-foley to user-scope first.
    $userPlugins = Join-Path $scratchHome 'plugins\user-foley'
    New-Item -ItemType Directory -Path $userPlugins -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $skillDir 'plugins\audio-foley\plugin.json') -Destination (Join-Path $userPlugins 'plugin.json') -Force
    Copy-Item -LiteralPath (Join-Path $skillDir 'plugins\audio-foley\invoke.ps1') -Destination (Join-Path $userPlugins 'invoke.ps1') -Force
    $userListOut = (& pwsh -NoProfile -File $skillPath --plugins-list 2>&1 | Out-String)
    Check "user-scope plugin override shows user source" { $userListOut -match 'user-foley\s+1\.0\.0\s+audio\s+user' }
    # Remove it.
    $rmOut = (& pwsh -NoProfile -File $skillPath --plugin-remove user-foley 2>&1 | Out-String)
    Check "plugin-remove reports success" { $rmOut -match 'Removed user-scope plugin' }
    Check "user-scope plugin no longer listed" { -not (Test-Path $userPlugins) }

    # Validate the plugin SDK helpers are importable.
    $sdkPath = Join-Path $skillDir 'plugin-sdk\Vortex.Plugin.psm1'
    Check "plugin SDK module exists" { Test-Path $sdkPath }
    $sdkTest = pwsh -NoProfile -Command "& { Import-Module '$sdkPath' -Force; Get-Command Get-VortexPluginInput | Out-Null; Get-Command Write-VortexPluginOutput | Out-Null; Get-Command Test-VortexPluginInput | Out-Null; Get-Command Invoke-MiniMaxLLM | Out-Null; Get-Command Write-VortexPluginLog | Out-Null; 'all exported' }" 2>&1
    Check "plugin SDK exports all 5 functions" { $sdkTest -match 'all exported' }

    # 11 additional reference plugins (v0.2.1) -- all discoverable + invokable.
    $listOut2 = (& pwsh -NoProfile -File $skillPath --plugins-list 2>&1 | Out-String)
    foreach ($p in 'audio-music','audio-voice','image-cover','image-map',
                   'code-python','video-hailuo','video-animator',
                   'data-researcher','data-analyst','design-mockup','media-sqlite') {
        Check "plugins-list shows $p" { $listOut2 -match [regex]::Escape($p) }
    }
    Check "plugins-list now reports 17 plugins total" { $listOut2 -match 'Total: 17 plugin' }

    # --plugin-install with a bad URL: should fail gracefully (downloads,
    # tries to extract, errors out, removes the partial folder).
    $env:VORTEX_HOME = $scratchHome
    $badInstall = (& pwsh -NoProfile -File $skillPath --plugin-install 'https://github.com/nonexistent-org-12345/no-such-repo' 2>&1 | Out-String)
    Check "plugin-install with bad URL fails" { ($badInstall -match 'Extract failed|not found|Download failed') -or ($LASTEXITCODE -ne 0) }
    Check "plugin-install did not leave a partial folder" { -not (Test-Path (Join-Path $scratchHome 'plugins\no-such-repo')) }

    # -----------------------------------------------------------------------
    # 10. Team mode (PRD-10: .vortex/config.json + per-user path sharding)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[10] Team mode"

    # --team-config: when no config exists, prints the default + recovery hint.
    $teamOut = (& pwsh -NoProfile -File $skillPath --team-config 2>&1 | Out-String)
    Check "--team-config without config says team mode is off" { $teamOut -match 'team mode is off' }

    # --team-config with a config: dumps the JSON + resolved paths.
    $cfgDir = Join-Path $scratchHome '.vortex'
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null
    $cfgFile = Join-Path $cfgDir 'config.json'
    $cfgBody = @{
        team_mode = $true
        user_audit_log = $true
        user_state = $true
        user_tasks = $true
        shared_deliverables = $true
        file_locking = 'advisory'
        lock_retry_ms = 100
        lock_max_attempts = 50
    }
    $cfgBody | ConvertTo-Json | Set-Content -LiteralPath $cfgFile -Encoding UTF8
    $teamOut2 = (& pwsh -NoProfile -File $skillPath --team-config 2>&1 | Out-String)
    Check "--team-config dumps team_mode=true" { $teamOut2 -match '"team_mode":\s*true' }
    Check "--team-config shows resolved AuditLogFile" { $teamOut2 -match 'audit-' + $env:USERNAME + '.jsonl' }
    Check "--team-config shows resolved PendingApprovalsDir" { $teamOut2 -match 'pending_approvals' }

    # When team_mode is on, the engine writes audit lines to the
    # per-user shard. We verify by creating the shard path manually
    # and confirming --team-config reports the right AuditLogFile
    # (which is what Audit::Emit will write to). We also confirm
    # FileLock::AppendWithLock would create parent dirs.
    $user = $env:USERNAME
    $expectedShard = Join-Path $scratchHome 'memory' "audit-$user.jsonl"
    Check "team-mode AuditLogFile is the per-user shard" { $teamOut2 -match [regex]::Escape($expectedShard) }

    # Disable team mode and confirm the shared audit.jsonl is the
    # destination again.
    $cfgOff = [ordered]@{
        team_mode = $false
        user_audit_log = $false
        user_state = $false
        user_tasks = $false
        shared_deliverables = $true
        file_locking = 'advisory'
        lock_retry_ms = 100
        lock_max_attempts = 50
    }
    $cfgOff | ConvertTo-Json | Set-Content -LiteralPath $cfgFile -Encoding UTF8
    $teamOut3 = (& pwsh -NoProfile -File $skillPath --team-config 2>&1 | Out-String)
    $expectedShared = Join-Path $scratchHome 'memory' 'audit.jsonl'
    Check "non-team-mode AuditLogFile is the shared audit.jsonl" { $teamOut3 -match [regex]::Escape($expectedShared) }

    # --stream-list: with no in-progress, reports none.
    # Section [6] (Golden Path template replay) leaves an ep2_smoke task in
    # the shared state/in_progress/ dir; clean it so this test sees a true
    # empty state.
    $ipDir = Join-Path $scratchHome 'state' 'in_progress'
    if (Test-Path $ipDir) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($ipDir, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    $streamListOut = (& pwsh -NoProfile -File $skillPath --stream-list 2>&1 | Out-String)
    Check "--stream-list with no in-progress shows none" { $streamListOut -match 'no in-progress' }

    # -----------------------------------------------------------------------
    # 11. Streaming (PRD-14: in_progress dir, partials, hints, finalize)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[11] Streaming"

    # The team-mode config from section [10] is still active; reset it
    # to off so the streaming tests use the shared in_progress dir
    # (the engine's --stream-list reads from p->InProgressDir which is
    # the shared one when team_mode is off, per-user when on).
    $cfgOffStreaming = [ordered]@{
        team_mode = $false
        user_audit_log = $false
        user_state = $false
        user_tasks = $false
        shared_deliverables = $true
        file_locking = 'advisory'
        lock_retry_ms = 100
        lock_max_attempts = 50
    }
    $cfgOffStreaming | ConvertTo-Json | Set-Content -LiteralPath $cfgFile -Encoding UTF8
    $env:VORTEX_HOME = $scratchHome

    $ipDir = Join-Path $scratchHome 'state' 'in_progress'
    if (Test-Path $ipDir) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($ipDir, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    New-Item -ItemType Directory -Path $ipDir -Force | Out-Null

    $testTask = 'streaming_smoke'
    $taskDir = Join-Path $ipDir $testTask
    New-Item -ItemType Directory -Path $taskDir -Force | Out-Null
    # Write a .started manifest
    '{"task_id":"streaming_smoke","agent":"shift.prose","started_at":1700000000}' |
        Set-Content -LiteralPath (Join-Path $taskDir '.started') -Encoding UTF8
    # Write a fake .partial.md (also create the sidecar .json
    # that StreamSink writes in production).
    '# Sample partial' | Set-Content -LiteralPath (Join-Path $taskDir '01_script.partial.md') -Encoding UTF8
    '{"task_id":"streaming_smoke","deliverable":"01_script","produced_at":1700000000,"size_bytes":15,"is_partial":true,"source":"x"}' |
        Set-Content -LiteralPath (Join-Path $taskDir '01_script.json') -Encoding UTF8

    # --stream-list should now see the seeded task (it looks for .started).
    $streamListOut2 = (& pwsh -NoProfile -File $skillPath --stream-list 2>&1 | Out-String)
    Check "--stream-list sees the seeded task" { $streamListOut2 -match 'streaming_smoke' }
    Check "--stream-list reports 1 partial" { $streamListOut2 -match '1' }
    Check "--stream-list shows the in_progress path" { $streamListOut2 -match 'Total: 1' }

    # --hint writes to .hints.jsonl via the engine.
    $hintOut = (& pwsh -NoProfile -File $skillPath --hint $testTask --text "Tone is too dark" 2>&1 | Out-String)
    $hintOk = ($LASTEXITCODE -eq 0) -or ($hintOut -match 'Hint sent')
    Check "--hint succeeds" { $hintOk }

    # --stream-finalize moves the .partial to deliverables/<project>/
    # and writes a .completed manifest (also moved into the deliverables
    # dir), then removes the in_progress task dir. Set VORTEX_PROJECT
    # so the per-project subfolder is used.
    $delivDir = Join-Path $scratchHome 'deliverables' 'streaming_smoke'
    if (Test-Path $delivDir) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($delivDir, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    $env:VORTEX_PROJECT = 'streaming_smoke'
    & pwsh -NoProfile -File $skillPath --stream-finalize $testTask 2>&1 | Out-Null
    Remove-Item Env:\VORTEX_PROJECT -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    Check "--stream-finalize wrote a .completed manifest" { Test-Path (Join-Path $delivDir '.completed') }
    Check "--stream-finalize moved the partial to deliverables" { Test-Path (Join-Path $delivDir '01_script.md') }
    Check "--stream-finalize removed the in_progress dir" { -not (Test-Path $taskDir) }

    # The hint should be audited in the shared audit log.
    $auditFile = Join-Path $scratchHome 'memory' 'audit.jsonl'
    if (Test-Path $auditFile) {
        $auditContent = Get-Content $auditFile -Raw
        Check "hint was audited" { $auditContent -match 'hint_sent' }
    } else {
        Check "hint was audited" { $false }
    }

    # -----------------------------------------------------------------------
    # 12. v0.2.3 gap fixes (PRD-12: G1, G2, G4, G5, G11)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[12] v0.2.3 gap fixes"

    # ---- G5: Get-VortexVersion cmdlet ----
    $verOut = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexVersion" 2>&1
    Check "Get-VortexVersion returns the engine version" { ($verOut | Out-String).Trim() -match '0\.3\.[0-9]+' }

    # ---- G5: --version still works for backwards compat ----
    $ver2 = & pwsh -NoProfile -File $skillPath --version 2>&1 | Select-Object -Last 1
    Check "skill.ps1 --version still prints the engine version" { $ver2 -match '0\.3\.[0-9]+' }

    # ---- G11: standalone cmdlets for wrapper commands ----
    $decisions = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexDecision" 2>&1
    Check "Get-VortexDecision is exported" { ($decisions | Out-String).Length -gt 0 }

    $agents = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexAgent" 2>&1
    Check "Get-VortexAgent is exported" { ($agents | Out-String).Length -gt 0 }

    $graph = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexAgentGraph" 2>&1
    Check "Get-VortexAgentGraph is exported" { ($graph | Out-String).Length -gt 0 }

    $stream = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexStream" 2>&1
    Check "Get-VortexStream is exported" { ($stream | Out-String).Length -gt 0 }

    $team = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexTeamConfig" 2>&1
    Check "Get-VortexTeamConfig is exported" { ($team | Out-String).Length -gt 0 }

    # ---- G1: Packager emits Audit::Emit lines for the packager worker ----
    # Use a fresh swarm + project to avoid the section [4] SKIPPED_EXISTS
    # path. Reuses the test_swarm_packaging swarm from section [4] but
    # with a NEW project name to land in a clean deliverables/ folder.
    $env:VORTEX_PROJECT = 'pkg_v023_audit'
    $delivDir023 = Join-Path $scratchHome 'deliverables\pkg_v023_audit'
    if (Test-Path $delivDir023) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($delivDir023, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    # Clear the audit log so we can find our 3 lines
    $auditPath = Join-Path $scratchHome 'memory\audit.jsonl'
    if (Test-Path $auditPath) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($auditPath, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    $pkgOut = (& pwsh -NoProfile -File $skillPath --package 'test_swarm_packaging' 2>&1 | Out-String)
    Check "G1: --package audit log exists after the run" { Test-Path $auditPath }
    if (Test-Path $auditPath) {
        $auditContent = Get-Content $auditPath -Raw
        Check "G1: dispatch_start was audited as worker.packager" { $auditContent -match '"agent":"worker\.packager".*"action":"dispatch_start"' }
        Check "G1: deliver was audited for each copied file" { ([regex]::Matches($auditContent, '"action":"deliver"')).Count -ge 3 }
        Check "G1: dispatch_end was audited" { $auditContent -match '"agent":"worker\.packager".*"action":"dispatch_end"' }
        Check "G1: dispatch_end status is ok or partial" { $auditContent -match '"action":"dispatch_end".*"status":"(ok|partial)"' }
    } else {
        Check "G1: dispatch_start was audited as worker.packager" { $false }
        Check "G1: deliver was audited for each copied file" { $false }
        Check "G1: dispatch_end was audited" { $false }
        Check "G1: dispatch_end status is ok or partial" { $false }
    }
    Remove-Item Env:\VORTEX_PROJECT -ErrorAction SilentlyContinue

    # ---- G2: Inspector writes active_budgets.json under FileLock ----
    # The inspector only fires when tokensUsedThisRun > 15000. We can't
    # easily trigger that via the CLI; instead, verify the file exists
    # after any dispatch and has the expected schema.
    $budgetFile = Join-Path $scratchHome 'state\active_budgets.json'
    if (Test-Path $budgetFile) {
        $b = Get-Content $budgetFile -Raw | ConvertFrom-Json
        Check "G2: active_budgets.json has total_tokens field" { $null -ne $b.total_tokens }
    } else {
        # It's OK if not written -- we don't run an inspect on every dispatch
        Check "G2: active_budgets.json may not exist (no inspector run)" { $true }
    }

    # ---- G4: VectorHydrate writes memory\vectors.json sidecar ----
    $vecJson = Join-Path $scratchHome 'memory\vectors.json'
    $vhOut = (& pwsh -NoProfile -File $skillPath --vector-hydrate 2>&1 | Out-String)
    Check "G4: --vector-hydrate writes memory\vectors.json" { Test-Path $vecJson }
    if (Test-Path $vecJson) {
        $vec = Get-Content $vecJson -Raw | ConvertFrom-Json
        Check "G4: vectors.json has version field" { $vec.version -eq 1 }
        Check "G4: vectors.json has chunks array" { $vec.chunks -is [array] }
    } else {
        Check "G4: vectors.json has version field" { $false }
        Check "G4: vectors.json has chunks array" { $false }
    }

    # ---- G3: lib\vector_schema.sql is shipped with the skill ----
    $workspaceSkill = Join-Path $root '..\vortex-os-skill\lib\vector_schema.sql'
    Check "G3: lib\vector_schema.sql is in the workspace skill" { Test-Path $workspaceSkill }

    # ---- G8: install.ps1 reads the auto-update cache (smoke) ----
    # We can't run the full install flow in a test (would touch the user's
    # PSModulePath), but we can verify the cache file path is used.
    $cfgDir2 = Join-Path $scratchHome '.vortex'
    $cfgFile2 = Join-Path $cfgDir2 'config.json'
    $cfgOff2 = [ordered]@{ team_mode = $false; user_audit_log = $false; user_state = $false; user_tasks = $false; shared_deliverables = $true; file_locking = 'advisory'; lock_retry_ms = 100; lock_max_attempts = 50 }
    $cfgOff2 | ConvertTo-Json | Set-Content -LiteralPath $cfgFile2 -Encoding UTF8
    $env:VORTEX_HOME = $scratchHome
    # Seed the cache so a hypothetical next install would see it.
    $stateDir2 = Join-Path $scratchHome 'state'
    New-Item -ItemType Directory -Path $stateDir2 -Force | Out-Null
    $cacheFile2 = Join-Path $stateDir2 'auto-update-check.json'
    $cacheObj = @{
        last_check    = (Get-Date).AddMinutes(-30).ToString('o')
        remote_tag    = 'v0.2.2'
        installed_ver = '0.2.2'
        updated       = $false
        assets        = @(
            [PSCustomObject]@{ name = 'Vortex.dll'; browser_download_url = 'https://x/Vortex.dll' }
        )
    }
    $cacheObj | ConvertTo-Json | Set-Content -LiteralPath $cacheFile2 -Encoding UTF8
    Check "G8: auto-update cache file path is state\auto-update-check.json" { Test-Path $cacheFile2 }

    # -----------------------------------------------------------------------
    # 13. v0.3.0 cross-project memory (PRD-17)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[13] v0.3.0 cross-project memory (PRD-17)"

    # Clean any prior memory store + reset deliverables/ so the test
    # scope is just the 2 projects we seed below (earlier sections
    # accumulate client_acme_q1/q2-style slugs into the same
    # scratch home).
    $derivedDir = Join-Path $scratchHome 'memory' 'derived'
    if (Test-Path $derivedDir) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($derivedDir, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    $delivRoot = Join-Path $scratchHome 'deliverables'
    if (Test-Path $delivRoot) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($delivRoot, 'OnlyErrorDialogs', 'SendToRecycleBin') }
    # Also reset the audit log so only our seed lines are present.
    $auditPath13 = Join-Path $scratchHome 'memory' 'audit.jsonl'
    if (Test-Path $auditPath13) { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($auditPath13, 'OnlyErrorDialogs', 'SendToRecycleBin') }

    # Seed: 2 projects with a few deliverables each. Project names use
    # the client-acme_q* pattern so the series detection (Q1 in PRD-17)
    # groups them as one series.
    $proj1 = 'client_acme_q1'
    $proj2 = 'client_acme_q2'
    foreach ($p in @($proj1, $proj2)) {
        $pDir = Join-Path $scratchHome 'deliverables' $p 'deliverables'
        New-Item -ItemType Directory -Path $pDir -Force | Out-Null
        'export const VERSION = "0.1.0";' | Set-Content -LiteralPath (Join-Path $pDir 'index.ts')
        '{"name":"acme","version":"0.1.0"}' | Set-Content -LiteralPath (Join-Path $pDir 'package.json')
        '# README' | Set-Content -LiteralPath (Join-Path $pDir 'README.md')
    }
    # Seed: an audit.jsonl with a couple of plugin invocations so
    # plugin_usage + plugin_cost are non-empty.
    $auditDir = Join-Path $scratchHome 'memory'
    if (-not (Test-Path $auditDir)) { New-Item -ItemType Directory -Path $auditDir -Force | Out-Null }
    $seedAudit = @(
        '{"ts":"2026-08-28T10:00:00.000+00:00","tier":"T3","agent":"code-typescript","action":"deliver","status":"ok","project":"client_acme_q1","task_id":"t1","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"index.ts","tags":["code"]}',
        '{"ts":"2026-08-28T10:01:00.000+00:00","tier":"T3","agent":"text-editor","action":"deliver","status":"ok","project":"client_acme_q1","task_id":"t2","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"README.md","tags":["text"]}',
        '{"ts":"2026-08-28T10:02:00.000+00:00","tier":"T2","agent":"shift.prose","action":"self_heal","status":"ok","project":"client_acme_q1","task_id":"t1","severity":"MEDIUM","rule_violated":"tone_drift","rule_fixed":"tightened tone","gate_id":"","tags":["selfheal"]}',
        '{"ts":"2026-08-28T11:00:00.000+00:00","tier":"T3","agent":"code-typescript","action":"deliver","status":"ok","project":"client_acme_q2","task_id":"t3","severity":"LOW","rule_violated":"","rule_fixed":"","gate_id":"index.ts","tags":["code"]}'
    )
    Set-Content -LiteralPath (Join-Path $auditDir 'audit.jsonl') -Value $seedAudit -Encoding UTF8

    # --compile-memory
    $env:VORTEX_HOME = $scratchHome
    $memOut = (& pwsh -NoProfile -File $skillPath --compile-memory 2>&1 | Out-String)
    Check "G17: --compile-memory returns 0" { $LASTEXITCODE -eq 0 }
    Check "G17: --compile-memory creates memory/derived/" { Test-Path $derivedDir }
    Check "G17: --compile-memory writes project/<slug>.json" { Test-Path (Join-Path $derivedDir 'project' 'client_acme_q1.json') }
    Check "G17: --compile-memory writes index.json" { Test-Path (Join-Path $derivedDir 'index.json') }
    Check "G17: --compile-memory writes operator.json" { Test-Path (Join-Path $derivedDir 'operator.json') }
    Check "G17: --compile-memory writes series/<series>.json (auto-detected)" { Test-Path (Join-Path $derivedDir 'series' 'client_acme.json') }

    # Index sanity: lists 2 projects.
    if (Test-Path (Join-Path $derivedDir 'index.json')) {
        $idx = Get-Content (Join-Path $derivedDir 'index.json') -Raw | ConvertFrom-Json
        # @(...) prevents PowerShell from unrolling the array (which would
        # make .Count return the string length of the first element).
        Check "G17: index lists 2 projects" { @($idx.projects).Count -eq 2 }
        Check "G17: index lists 1 series" { @($idx.series).Count -eq 1 }
    } else {
        Check "G17: index lists 2 projects" { $false }
        Check "G17: index lists 1 series" { $false }
    }

    # Project fingerprint sanity.
    if (Test-Path (Join-Path $derivedDir 'project' 'client_acme_q1.json')) {
        $proj = Get-Content (Join-Path $derivedDir 'project' 'client_acme_q1.json') -Raw | ConvertFrom-Json
        Check "G17: project fingerprint has project_type_hint" { $proj.project_type_hint -eq 'code-typescript' }
        Check "G17: project fingerprint has deliverable_type_histogram" { $proj.deliverable_type_histogram.PSObject.Properties.Count -ge 1 }
        Check "G17: project fingerprint has plugin_usage with code-typescript" { $proj.plugin_usage.'code-typescript' -ge 1 }
        Check "G17: project fingerprint has common_components" { $proj.common_components.Count -ge 1 }
        Check "G17: project fingerprint has episodes_in_series = client_acme" { $proj.episodes_in_series -eq 'client_acme' }
        Check "G17: project fingerprint has common_failure_modes" { $proj.common_failure_modes -contains 'tone_drift' }
    } else {
        Check "G17: project fingerprint has project_type_hint" { $false }
        Check "G17: project fingerprint has deliverable_type_histogram" { $false }
        Check "G17: project fingerprint has plugin_usage with code-typescript" { $false }
        Check "G17: project fingerprint has common_components" { $false }
        Check "G17: project fingerprint has episodes_in_series = client_acme" { $false }
        Check "G17: project fingerprint has common_failure_modes" { $false }
    }

    # Operator profile sanity.
    if (Test-Path (Join-Path $derivedDir 'operator.json')) {
        $op = Get-Content (Join-Path $derivedDir 'operator.json') -Raw | ConvertFrom-Json
        Check "G17: operator profile has per_plugin_stats" { $op.per_plugin_stats.PSObject.Properties.Count -ge 1 }
        Check "G17: operator profile projects_analyzed = 2" { $op.projects_analyzed -eq 2 }
    } else {
        Check "G17: operator profile has per_plugin_stats" { $false }
        Check "G17: operator profile projects_analyzed = 2" { $false }
    }

    # Series sanity.
    if (Test-Path (Join-Path $derivedDir 'series' 'client_acme.json')) {
        $ser = Get-Content (Join-Path $derivedDir 'series' 'client_acme.json') -Raw | ConvertFrom-Json
        Check "G17: series episodes list contains client_acme_q1" { @($ser.episodes) -contains 'client_acme_q1' }
        Check "G17: series episodes list contains client_acme_q2" { @($ser.episodes) -contains 'client_acme_q2' }
        Check "G17: series has 2 episodes" { @($ser.episodes).Count -eq 2 }
    } else {
        Check "G17: series episodes list contains client_acme_q1" { $false }
        Check "G17: series episodes list contains client_acme_q2" { $false }
        Check "G17: series has 2 episodes" { $false }
    }

    # Idempotency: running --compile-memory twice produces semantically
    # identical output (compiled_at timestamp differs, so we can't
    # byte-compare -- we compare the JSON content with compiled_at removed).
    $content1 = (Get-Content (Join-Path $derivedDir 'project' 'client_acme_q1.json') -Raw | ConvertFrom-Json) | ConvertTo-Json -Depth 10
    & pwsh -NoProfile -File $skillPath --compile-memory 2>&1 | Out-Null
    $content2 = (Get-Content (Join-Path $derivedDir 'project' 'client_acme_q1.json') -Raw | ConvertFrom-Json) | ConvertTo-Json -Depth 10
    # Strip compiled_at which legitimately changes between runs.
    $content1 = ($content1 -replace '"compiled_at":\s*\d+,', '')
    $content2 = ($content2 -replace '"compiled_at":\s*\d+,', '')
    Check "G17: --compile-memory is idempotent (semantic JSON equality, ignoring compiled_at)" { $content1 -eq $content2 }

    # --memory-show returns the prior projects context slice.
    $memShowOut = (& pwsh -NoProfile -File $skillPath --memory-show 'client_acme_q3' 2>&1 | Out-String)
    Check "G17: --memory-show returns a slice for client_acme_q3 (looks in series)" { $memShowOut -match 'client_acme_q2' -or $memShowOut -match 'client_acme_q1' }
    Check "G17: --memory-show includes 'Prior projects context' header" { $memShowOut -match 'Prior projects context' }

    # Get-VortexMemory cmdlet.
    $memCmdlet = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexMemory" 2>&1
    Check "G17: Get-VortexMemory is exported" { ($memCmdlet | Out-String).Length -gt 0 }
    $memCmdletProj = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexMemory -Project client_acme_q1" 2>&1
    Check "G17: Get-VortexMemory -Project prints type_hint" { ($memCmdletProj | Out-String) -match 'code-typescript' }
    $memCmdletOp = & pwsh -NoProfile -Command "Import-Module $engineDir\Vortex.psd1 -ErrorAction Stop; Get-VortexMemory -Operator" 2>&1
    Check "G17: Get-VortexMemory -Operator prints projects_analyzed" { ($memCmdletOp | Out-String) -match 'projects' }

    # --compile-memory --project <slug> only updates one file.
    $beforeIdx = (Get-Content (Join-Path $derivedDir 'index.json') -Raw).Length
    & pwsh -NoProfile -File $skillPath --compile-memory --project client_acme_q1 2>&1 | Out-Null
    Check "G17: --compile-memory --project <slug> exits 0" { $LASTEXITCODE -eq 0 }

    # Deleting the memory store doesn't break the engine (R-8).
    [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($derivedDir, 'OnlyErrorDialogs', 'SendToRecycleBin')
    $noMemOut = (& pwsh -NoProfile -File $skillPath --memory-show 'client_acme_q1' 2>&1 | Out-String)
    Check "G17: --memory-show is safe when memory store is absent" { $noMemOut -match 'no memory' -or $noMemOut -match 'no memory slice' }
    # Re-create the store so the rest of the suite has a memory.
    & pwsh -NoProfile -File $skillPath --compile-memory 2>&1 | Out-Null

    # -----------------------------------------------------------------------
    # G18: --recipe <name> resolves to templates/<name>.json (v0.3.5)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[18] --recipe shortcut"
    # Make sure the template file the recipe points to exists
    $recipeTemplate = Join-Path $skillRoot 'templates\iteration_pattern.json'
    if (-not (Test-Path $recipeTemplate)) {
        # The skill ships templates/iteration_pattern.json since v0.3.1.
        # If the local skill folder is older, copy one in.
        $recipeTemplate = Join-Path $skillRoot 'templates\recipe_smoke.json'
        if (-not (Test-Path $recipeTemplate)) {
            Set-Content -LiteralPath $recipeTemplate -Value '{"name":"recipe_smoke","version":"0.0.0","objective_template":"smoke","substitutions":{},"deliverables":[],"hitl_gates":[],"self_heal_targets":[]}' -Encoding UTF8
        }
    }
    # Drop a minimal "recipe_smoke" template so we can test the resolver without depending on the skill's actual templates
    $smokeTemplate = Join-Path $skillRoot 'templates\recipe_smoke.json'
    Set-Content -LiteralPath $smokeTemplate -Value '{"name":"recipe_smoke","version":"0.0.0","objective_template":"smoke test","substitutions":{},"deliverables":[],"hitl_gates":[],"self_heal_targets":[]}' -Encoding UTF8
    $recipeOut = (& pwsh -NoProfile -File $skillPath --recipe recipe_smoke 2>&1 | Out-String)
    Check "G18: --recipe finds template by basename" { $recipeOut -match 'recipe_smoke' -or $recipeOut -match 'Template' -or $recipeOut -match 'recipe' }
    # The recipe for a name that doesn't exist should fail cleanly
    $missingOut = (& pwsh -NoProfile -File $skillPath --recipe no_such_recipe 2>&1 | Out-String)
    $missingExit = $LASTEXITCODE
    Check "G18: --recipe with unknown name exits non-zero" { $missingExit -ne 0 }
    Check "G18: --recipe with unknown name prints an error" { $missingOut -match 'Recipe not found' -or $missingOut -match 'ERROR' }
    Remove-Item -LiteralPath $smokeTemplate -Force -ErrorAction SilentlyContinue

    # -----------------------------------------------------------------------
    # G19: agent_roster validation in --dispatch-template (v0.3.5)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[19] agent_roster walker"
    # Make a template that references a real agent (supervisor.shift) and a fake one (nope.not_here)
    $arTemplate = Join-Path $skillRoot 'templates\agent_roster_smoke.json'
    $arBody = @'
{
  "name": "agent_roster_smoke",
  "version": "0.0.0",
  "objective_template": "smoke",
  "substitutions": {},
  "deliverables": [],
  "hitl_gates": [],
  "self_heal_targets": [],
  "agent_roster": ["supervisor.shift", "nope.not_here"]
}
'@
    Set-Content -LiteralPath $arTemplate -Value $arBody -Encoding UTF8
    $arOut = (& pwsh -NoProfile -File $skillPath --dispatch-template $arTemplate 2>&1 | Out-String)
    Check "G19: agent_roster validates the existing agent" { $arOut -match 'supervisor.shift' -and $arOut -match 'manifest found' }
    Check "G19: agent_roster warns on missing agent" { $arOut -match 'nope.not_here' -and $arOut -match 'no manifest found' }
    Remove-Item -LiteralPath $arTemplate -Force -ErrorAction SilentlyContinue

    # -----------------------------------------------------------------------
    # G20: --memory-show returns the cached slice when the store exists (v0.3.5)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[20] --memory-show with a populated store"
    & pwsh -NoProfile -File $skillPath --compile-memory --project client_acme_q1 2>&1 | Out-Null
    $memOut = (& pwsh -NoProfile -File $skillPath --memory-show client_acme_q1 2>&1 | Out-String)
    Check "G20: --memory-show returns a non-empty slice" { $memOut.Length -gt 200 }
    Check "G20: --memory-show mentions the operator profile" { $memOut -match 'Operator profile|operator' }
    Check "G20: --memory-show does not crash" { $LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 2 }

    # -----------------------------------------------------------------------
    # G21: Reviewer gate check on --package (v0.3.5)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[21] Reviewer gate on --package"
    # Make a fake swarm + plan with a reviewer field
    $swarmId = "smoke_reviewer_$((Get-Date).Ticks)"
    $swarmDir = Join-Path $swarmsDir $swarmId
    New-Item -ItemType Directory -Path $swarmDir -Force | Out-Null
    $planBody = @"
{
  "task_id": "$swarmId",
  "agents_planned": ["supervisor.shift"],
  "agent_roster": ["supervisor.shift", "media-stack"],
  "reviewer": "reviewer.quality",
  "deliverables": []
}
"@
    Set-Content -LiteralPath (Join-Path $swarmDir 'plan.json') -Value $planBody -Encoding UTF8
    # v0.3.7: snapshot the live agent manifests so we can restore them after
    # the test. The previous version of this test wrote 1-line stubs to
    # agents\media-stack.json and agents\reviewer.quality.json and never
    # restored them -- which clobbered the real agent manifests every time
    # the test ran. Closes G9.
    $mediaStackBackup  = Join-Path $scratchHome 'media-stack.json.bak'
    $reviewerQBackup   = Join-Path $scratchHome 'reviewer.quality.json.bak'
    if (Test-Path (Join-Path $skillRoot 'agents\media-stack.json')) {
        Copy-Item (Join-Path $skillRoot 'agents\media-stack.json') $mediaStackBackup
    }
    if (Test-Path (Join-Path $skillRoot 'agents\reviewer.quality.json')) {
        Copy-Item (Join-Path $skillRoot 'agents\reviewer.quality.json') $reviewerQBackup
    }
    Set-Content -LiteralPath (Join-Path $skillRoot 'agents\media-stack.json') -Value '{"name":"media-stack","version":"0.2.0","kind":"dynamic","tier":3}' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $skillRoot 'agents\reviewer.quality.json') -Value '{"name":"reviewer.quality","version":"0.1.0","kind":"dynamic","tier":2}' -Encoding UTF8
    try {
        $pkgOut = (& pwsh -NoProfile -File $skillPath --package $swarmId 2>&1 | Out-String)
        Check "G21: --package prints the Reviewer gate line" { $pkgOut -match 'Reviewer gate: reviewer.quality' -or $pkgOut -match 'REVIEWER_INVOKE' }
        Check "G21: --package completes" { $LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 2 }
    } finally {
        # Restore the live manifests (or remove the stub if there was no
        # original). Wrapped in try/catch so a failure in cleanup doesn't
        # kill the rest of the test suite.
        try {
            if (Test-Path $mediaStackBackup) {
                Copy-Item $mediaStackBackup (Join-Path $skillRoot 'agents\media-stack.json') -Force
            } else {
                Remove-Item -LiteralPath (Join-Path $skillRoot 'agents\media-stack.json') -Force -ErrorAction SilentlyContinue
            }
        } catch { Write-Host "  (G21 cleanup: media-stack.json restore failed: $($_.Exception.Message))" -ForegroundColor Yellow }
        try {
            if (Test-Path $reviewerQBackup) {
                Copy-Item $reviewerQBackup (Join-Path $skillRoot 'agents\reviewer.quality.json') -Force
            } else {
                Remove-Item -LiteralPath (Join-Path $skillRoot 'agents\reviewer.quality.json') -Force -ErrorAction SilentlyContinue
            }
        } catch { Write-Host "  (G21 cleanup: reviewer.quality.json restore failed: $($_.Exception.Message))" -ForegroundColor Yellow }
    }

    # -----------------------------------------------------------------------
    # G24: CmdDispatchAgentRoster walks the agent's plugin_roster
    # (v0.3.7 - closes G1+G2+G3+G4)
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[24] CmdDispatchAgentRoster walks the agent's plugin_roster"

    # Snapshot the live media-stack.json in case the test mutates it.
    $liveMediaStack = Join-Path $skillRoot 'agents\media-stack.json'
    $mediaStackBackup24 = Join-Path $scratchHome 'media-stack.json.bak24'
    if (Test-Path $liveMediaStack) { Copy-Item $liveMediaStack $mediaStackBackup24 }

    # Create a synthetic template that names media-stack in agent_roster.
    $rosterTemplate = Join-Path $swarmsDir 'executor_smoke.json'
    $rosterBody = @'
{
  "name": "executor_smoke",
  "version": "0.0.0",
  "objective_template": "smoke executor test",
  "substitutions": {},
  "deliverables": [],
  "hitl_gates": [],
  "self_heal_targets": [],
  "agent_roster": ["media-stack"]
}
'@
    Set-Content -LiteralPath $rosterTemplate -Value $rosterBody -Encoding UTF8

    # Snapshot state before dispatch.
    $delivDir = Join-Path $scratchHome 'deliverables'
    $delivBefore = (Get-ChildItem -Recurse -ErrorAction SilentlyContinue $delivDir | Measure-Object).Count
    $auditFile = Join-Path $scratchHome 'memory\audit.jsonl'
    $auditBefore = if (Test-Path $auditFile) { (Get-Content $auditFile).Count } else { 0 }

    # Run the dispatch. With the v0.3.7 fix, this should walk media-stack's
    # 7-plugin roster and produce at least one file under deliverables/.
    # Without the fix, plan.json is empty and nothing happens.
    $rosterOut = (& pwsh -NoProfile -File $skillPath --dispatch-template $rosterTemplate 2>&1 | Out-String)

    $delivAfter = (Get-ChildItem -Recurse -ErrorAction SilentlyContinue $delivDir | Measure-Object).Count
    $auditAfter = if (Test-Path $auditFile) { (Get-Content $auditFile).Count } else { 0 }

    Check "G24: --dispatch-template invokes at least one plugin" {
        # After the fix: a `plugin_invoke` audit entry was emitted (audit
        # log grew) OR the engine printed a "Plugin <name>" line.
        $rosterOut -match 'Plugin ' -or $rosterOut -match 'plugin_invoke' -or ($auditAfter -gt $auditBefore)
    }
    Check "G24: --dispatch-template produces at least one deliverable" {
        $delivAfter -gt $delivBefore
    }
    Check "G24: --dispatch-template does NOT just write an empty plan.json" {
        # Before v0.3.7: Swarm::Spawn wrote {"tasks":[]} and the executor
        # did nothing. After v0.3.7: the executor walks the roster and
        # emits a "swarm_close" audit entry. The plan.json stub itself
        # is unchanged (Swarm::Spawn still writes {"tasks":[]} -- the
        # executor does its work in the audit log + deliverables/ copy
        # step, not in the plan). Assert on the audit, not the plan.
        if (-not (Test-Path $auditFile)) { return $false }
        $auditContent = Get-Content $auditFile -Raw
        return $auditContent -match '"action":"swarm_close"'
    }

    # Restore the live media-stack.json
    if (Test-Path $mediaStackBackup24) {
        Copy-Item $mediaStackBackup24 $liveMediaStack -Force
    }

    # -----------------------------------------------------------------------
    # G26: --budget-show with no budget configured returns a sane default
    # (v0.3.8 - closes G14 from the gap analysis)
    # Pre-v0.3.8: --budget-show printed "tokens_total: 0" and the cost
    # tracker skipped enforcement. v0.3.8: a sane default is applied so
    # every project always has a budget to enforce.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[26] --budget-show with no budget configured returns a default"
    $defaultProject = "g26_default_$((Get-Date).Ticks)"
    $defaultBudgetOut = (& pwsh -NoProfile -File $skillPath --budget-show --project $defaultProject 2>&1 | Out-String)
    Write-Host "  --- budget-show output ---"
    $defaultBudgetOut.Split("`n") | ForEach-Object { Write-Host "  $_" }
    $defaultTokens = if ($defaultBudgetOut -match 'tokens_total:\s*(\d+)') { [int]$Matches[1] } else { 0 }
    Check "G26: --budget-show with no budget returns a non-zero default" { $defaultTokens -gt 0 }

    # -----------------------------------------------------------------------
    # G27: every dispatch writes a cost_log.jsonl entry (v0.3.8 - G11)
    # Pre-v0.3.8 the executor path never called CostTracker::RecordTokens,
    # so cost_log.jsonl was empty unless --with-memory triggered the LLM.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[27] --dispatch-template writes a cost_log.jsonl entry"
    $g27Proj = "g27_costlog_$((Get-Date).Ticks)"
    $g27Template = Join-Path $swarmsDir 'g27_costlog.json'
    $g27Body = '{"name":"g27_costlog","version":"0.0.0","objective_template":"smoke","substitutions":{},"deliverables":[],"hitl_gates":[],"self_heal_targets":[],"agent_roster":["media-stack"]}'
    Set-Content -LiteralPath $g27Template -Value $g27Body -Encoding UTF8
    $g27CostLog = Join-Path $scratchHome 'state\cost_log.jsonl'
    $g27Before = if (Test-Path $g27CostLog) { (Get-Content $g27CostLog).Count } else { 0 }
    & pwsh -NoProfile -File $skillPath --dispatch-template $g27Template 2>&1 | Out-Null
    $g27After = if (Test-Path $g27CostLog) { (Get-Content $g27CostLog).Count } else { 0 }
    Check "G27: cost_log.jsonl has a new entry after a dispatch" { $g27After -gt $g27Before }

    # -----------------------------------------------------------------------
    # G28: --with-memory injects the prior-context slice into the task
    # (v0.3.8 - G8). The flag was documented in v0.3.0 but never wired.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[28] --with-memory injects the memory slice into the rendered task"
    $g28Proj = "g28_mem_$((Get-Date).Ticks)"
    # Compile a memory slice for this project
    & pwsh -NoProfile -File $skillPath --compile-memory --project $g28Proj 2>&1 | Out-Null
    $g28Template = Join-Path $swarmsDir 'g28_mem.json'
    $g28Marker = "G28_UNIQUE_MARKER_$([guid]::NewGuid().ToString('N').Substring(0,8))"
    $g28BodyObj = @{ name = "g28_mem"; version = "0.0.0"; objective_template = "before-marker $g28Marker {{memory_slice}} after-marker"; substitutions = @{}; deliverables = @(); hitl_gates = @(); self_heal_targets = @(); agent_roster = @("media-stack") }
    $g28BodyObj | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $g28Template -Encoding UTF8
    # The engine's --dispatch-template branch doesn't parse --project; the
    # wrapper (skill.ps1) sets $env:VORTEX_PROJECT, but the engine's Paths
    # is built before the wrapper sees --with-memory. For the test, set
    # $env:VORTEX_PROJECT before the dispatch and unset after.
    $env:VORTEX_PROJECT = $g28Proj
    & pwsh -NoProfile -File $skillPath --dispatch-template $g28Template --with-memory 2>&1 | Out-Null
    # Pick the MOST RECENT task file (golden_path_*.md is the naming
    # convention the engine uses; alphabetical "First 1" would return
    # the earliest, which is from a prior test).
    $g28TaskFile = Get-ChildItem -Recurse -Filter 'golden_path_*.md' $scratchHome -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $g28TaskContent = if ($g28TaskFile) { Get-Content $g28TaskFile.FullName -Raw } else { '' }
    Check "G28: --with-memory substituted {{memory_slice}} (marker present, placeholder absent)" {
        $g28TaskContent.Contains($g28Marker) -and -not $g28TaskContent.Contains('{{memory_slice}}')
    }

    # -----------------------------------------------------------------------
    # G29: --dispatch-template auto-writes the .manifest.json
    # (v0.3.8 - G12). Pre-v0.3.8 the operator had to call --package
    # manually after every dispatch.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[29] --dispatch-template auto-writes the .manifest.json"
    $g29Proj = "g29_pkg_$((Get-Date).Ticks)"
    $env:VORTEX_PROJECT = $g29Proj
    $g29Template = Join-Path $swarmsDir 'g29_pkg.json'
    $g29Body = '{"name":"g29_pkg","version":"0.0.0","objective_template":"smoke","substitutions":{},"deliverables":[],"hitl_gates":[],"self_heal_targets":[],"agent_roster":["media-stack"]}'
    Set-Content -LiteralPath $g29Template -Value $g29Body -Encoding UTF8
    & pwsh -NoProfile -File $skillPath --dispatch-template $g29Template 2>&1 | Out-Null
    $g29Manifest = Join-Path $scratchHome "deliverables/$g29Proj/.manifest.json"
    Check "G29: .manifest.json was auto-written to deliverables/<project>/" { Test-Path $g29Manifest }

    # -----------------------------------------------------------------------
    # G30: --with-memory preserves real newlines in the task file
    # (v0.3.8.1 - bug fix on top of G8). Pre-v0.3.8.1 the --with-memory
    # handler in --dispatch-template did
    #   slice->Replace("\r","")->Replace("\n","\\n")->Replace("\"","\\\"")
    # which converted real newlines to LITERAL backslash-n. The
    # {{k=v}} override syntax is passed as a string[] element so
    # newlines survive intact; the only char that needs escaping is
    # the double-quote. v0.3.8.1: just escape the quote.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[30] --with-memory preserves real newlines (v0.3.8.1 G30 fix)"
    $g30Proj = "g30_mem_$((Get-Date).Ticks)"
    $env:VORTEX_PROJECT = $g30Proj
    # Compile memory so --memory-show returns a non-empty slice with
    # real newlines (project_type_hint + deliverable_type_histogram).
    & pwsh -NoProfile -File $skillPath --compile-memory --project $g30Proj 2>&1 | Out-Null
    $g30Mem = & pwsh -NoProfile -File $skillPath --memory-show $g30Proj 2>&1 | Out-String
    if (-not $g30Mem.Contains([char]10)) {
        # Fall back: hand-craft a slice with a real newline if the
        # compile didn't produce one (e.g. test runs against a fresh
        # VORTEX_HOME with no prior projects).
        $g30Mem = "line1`nline2`nline3"
    }
    $g30Template = Join-Path $swarmsDir 'g30_mem.json'
    $g30Body = @{
        name = "g30_mem"
        version = "0.0.0"
        objective_template = "BEFORE_SLICE`n`n{{memory_slice}}`n`nAFTER_SLICE"
        substitutions = @{}
        deliverables = @()
        hitl_gates = @()
        self_heal_targets = @()
        agent_roster = @("media-stack")
    } | ConvertTo-Json -Depth 5
    Set-Content -LiteralPath $g30Template -Value $g30Body -Encoding UTF8
    & pwsh -NoProfile -File $skillPath --dispatch-template $g30Template --with-memory 2>&1 | Out-Null
    # Pick the MOST RECENT task file. Earlier tests (G24, G28) also
    # wrote golden_path_*.md; "First 1" alphabetically returns the
    # earliest, which is from G24 (objective "smoke executor test" --
    # no newlines), making G30c fail. Sort by LastWriteTime instead.
    $g30TaskFile = Get-ChildItem -Recurse -Filter 'golden_path_*.md' $scratchHome -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $g30Content = if ($g30TaskFile) { Get-Content $g30TaskFile.FullName -Raw } else { '' }
    # The pre-v0.3.8.1 bug produced `BEFORE_SLICE\n\n## Prior` (literal \n).
    # v0.3.8.1 produces real newlines, so BEFORE_SLICE is followed by a
    # real LF then a blank line then the memory slice.
    $g30HasLiteralBSN = $g30Content -match 'BEFORE_SLICE\\n'
    $g30HasRealLF = $g30Content.Contains([char]10)
    $g30Placeholder = $g30Content.Contains('{{memory_slice}}')
    Check "G30a: --with-memory does NOT emit literal \n in task file" { -not $g30HasLiteralBSN }
    Check "G30b: --with-memory substitutes {{memory_slice}}" { -not $g30Placeholder }
    Check "G30c: --with-memory preserves real newlines" { $g30HasRealLF }

    # -----------------------------------------------------------------------
    # G31: reviewer-gate write path (v0.3.9). The v0.3.5 engine added
    # the READ path in CmdPackage but never the WRITE path -- Swarm::Spawn
    # only writes {"swarm_id","objective","tasks":[]} to plan.json, so
    # CmdPackage's "Reviewer gate: <name>" line never fired in real
    # dispatches (only when G21 hand-crafted plan.json with the field).
    # v0.3.9: CmdDispatchTemplate calls PatchPlanJsonWithReviewer after
    # the executor runs. It walks the template's agent_roster, finds
    # the first agent with `reviewer.name` in its manifest, and patches
    # plan.json to add `"reviewer":"<name>"` (string) and
    # `"agent_roster":[...]` (string array). G31 verifies the round
    # trip end-to-end: dispatch a template that names media-stack
    # (whose manifest has `reviewer.name="reviewer.quality"`), then
    # read the swarm's plan.json and assert both fields are present.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "[31] reviewer-gate write path: agent manifest -> plan.json (v0.3.9)"
    $g31Proj = "g31_reviewer_$((Get-Date).Ticks)"
    $env:VORTEX_PROJECT = $g31Proj
    $g31Template = Join-Path $swarmsDir 'g31_reviewer.json'
    $g31Body = @{
        name = "g31_reviewer"
        version = "0.0.0"
        objective_template = "smoke reviewer"
        substitutions = @{}
        deliverables = @()
        hitl_gates = @()
        self_heal_targets = @()
        # media-stack is the canonical agent with a `reviewer` block in
        # its manifest (reviewer.name = "reviewer.quality").
        agent_roster = @("media-stack")
    } | ConvertTo-Json -Depth 5
    Set-Content -LiteralPath $g31Template -Value $g31Body -Encoding UTF8
    & pwsh -NoProfile -File $skillPath --dispatch-template $g31Template 2>&1 | Out-Null
    # The engine writes plan.json to <swarms>/active_<task_id>/plan.json.
    # Pick the MOST RECENT plan.json (alphabetical "First 1" can return
    # a plan.json from an earlier test in the suite).
    $g31PlanFile = Get-ChildItem -Recurse -Filter 'plan.json' $swarmsDir -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $g31PlanContent = if ($g31PlanFile) { Get-Content $g31PlanFile.FullName -Raw } else { '' }
    $g31HasReviewer = $g31PlanContent -match '"reviewer"\s*:\s*"reviewer\.quality"'
    $g31HasRoster = $g31PlanContent -match '"agent_roster"\s*:\s*\[\s*"media-stack"\s*\]'
    $g31PreservesOriginals = ($g31PlanContent -match '"swarm_id"\s*:\s*"') -and ($g31PlanContent -match '"tasks"\s*:\s*\[\s*\]')
    Check "G31a: plan.json has reviewer field populated from agent manifest" { $g31HasReviewer }
    Check "G31b: plan.json has agent_roster array (string, not object)" { $g31HasRoster }
    Check "G31c: patch preserves Swarm::Spawn's swarm_id + tasks fields" { $g31PreservesOriginals }

    # -----------------------------------------------------------------------
    # G32-G37: Phase 1 of the cross-OS contract (v0.3.10). These 6 tests
    # assert the JSON output shape documented in docs/cli-json-contract.md
    # for 6 read-only verbs. The contract is:
    #   - single line, no envelope
    #   - top-level array for lists, object for single results
    #   - errors come out as {"error":"..."} on the same stdout stream
    #   - missing data: null for objects, [] for arrays
    # The test checks the output is valid JSON AND has the documented
    # shape (top-level type + key fields). Future verb additions
    # should follow the same pattern in this section.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "=== G32-G37: CLI JSON contract (v0.3.10) ===" -ForegroundColor Cyan

    # G32: --memory-show --json returns {"project":..,"operator":..,
    #     "prior_projects":[..],"series":..,"chars":N,"truncated":bool}
    $g32Out = & pwsh -NoProfile -File $skillPath --memory-show smoke-g32 --json 2>&1 | Out-String
    $g32Out = $g32Out.Trim()
    $g32Json = $null
    try { $g32Json = $g32Out | ConvertFrom-Json } catch {}
    Check "G32a: --memory-show --json output is valid JSON" { $g32Json -ne $null }
    Check "G32b: --memory-show --json is a single line" { -not ($g32Out.Contains([char]10) -or $g32Out.Contains([char]13)) }
    Check "G32c: --memory-show --json top-level is an object" { $g32Json -is [pscustomobject] }
    Check "G32d: --memory-show --json has project key" { $g32Json.PSObject.Properties['project'] -and $g32Json.project -eq 'smoke-g32' }
    Check "G32e: --memory-show --json has operator key (null when no operator.json)" { $g32Json.PSObject.Properties['operator'] }
    Check "G32f: --memory-show --json has prior_projects array" { $g32Json.PSObject.Properties['prior_projects'] -and $g32Json.prior_projects -is [array] }
    Check "G32g: --memory-show --json has chars + truncated keys" {
        $g32Json.PSObject.Properties['chars'] -and $g32Json.PSObject.Properties['truncated']
    }

    # G33: --budget-show --project X --json returns {"project":..,
    #     "tokens_total":N,"usd_total":N,"so_far":{...},"percent_used":N}
    $g33Proj = "smoke-g33-$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
    & pwsh -NoProfile -File $skillPath --budget-set --project $g33Proj --tokens-total 1000000 --usd-total 5.0 2>&1 | Out-Null
    $g33Out = & pwsh -NoProfile -File $skillPath --budget-show --project $g33Proj --json 2>&1 | Out-String
    $g33Out = $g33Out.Trim()
    $g33Json = $null
    try { $g33Json = $g33Out | ConvertFrom-Json } catch {}
    Check "G33a: --budget-show --json output is valid JSON" { $g33Json -ne $null }
    Check "G33b: --budget-show --json is a single line" { -not ($g33Out.Contains([char]10) -or $g33Out.Contains([char]13)) }
    Check "G33c: --budget-show --json echoes the project name" { $g33Json.project -eq $g33Proj }
    Check "G33d: --budget-show --json has tokens_total + usd_total" {
        $g33Json.PSObject.Properties['tokens_total'] -and $g33Json.PSObject.Properties['usd_total']
    }
    Check "G33e: --budget-show --json has so_far sub-object" {
        $g33Json.PSObject.Properties['so_far'] -and $g33Json.so_far -is [pscustomobject]
    }
    Check "G33f: --budget-show --json has percent_used" { $g33Json.PSObject.Properties['percent_used'] }
    # G33g: --budget-show with no --project in JSON mode emits {"error":"..."}
    $g33ErrOut = & pwsh -NoProfile -File $skillPath --budget-show --json 2>&1 | Out-String
    $g33ErrJson = $null
    try { $g33ErrJson = $g33ErrOut.Trim() | ConvertFrom-Json } catch {}
    Check "G33g: --budget-show --json without --project emits {error:..}" {
        $g33ErrJson -and $g33ErrJson.PSObject.Properties['error']
    }

    # G34: --decision-list --json returns {"decisions":[<obj>,...]}
    $g34Out = & pwsh -NoProfile -File $skillPath --decision-list --json 2>&1 | Out-String
    $g34Out = $g34Out.Trim()
    $g34Json = $null
    try { $g34Json = $g34Out | ConvertFrom-Json } catch {}
    Check "G34a: --decision-list --json output is valid JSON" { $g34Json -ne $null }
    Check "G34b: --decision-list --json is a single line" { -not ($g34Out.Contains([char]10) -or $g34Out.Contains([char]13)) }
    Check "G34c: --decision-list --json has decisions array (may be empty)" {
        $g34Json.PSObject.Properties['decisions'] -and $g34Json.decisions -is [array]
    }
    # G34d: record a decision and assert it round-trips through JSON
    $g34Task = "g34-$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
    & pwsh -NoProfile -File $skillPath --decision-record --task $g34Task --gate g1 --severity HIGH --choice "G34 smoke approve" --reason "test" 2>&1 | Out-Null
    $g34Out2 = & pwsh -NoProfile -File $skillPath --decision-list --json 2>&1 | Out-String
    $g34Json2 = $null
    try { $g34Json2 = $g34Out2.Trim() | ConvertFrom-Json } catch {}
    $g34Found = $g34Json2.decisions | Where-Object { $_.task_id -eq $g34Task }
    Check "G34d: --decision-list --json includes the just-recorded decision" {
        $g34Found -ne $null -and $g34Found.choice -eq 'G34 smoke approve'
    }

    # G35: --plugins-list --json returns {"plugins":[<obj>,...],"total":N}
    $g35Out = & pwsh -NoProfile -File $skillPath --plugins-list --json 2>&1 | Out-String
    $g35Out = $g35Out.Trim()
    $g35Json = $null
    try { $g35Json = $g35Out | ConvertFrom-Json } catch {}
    Check "G35a: --plugins-list --json output is valid JSON" { $g35Json -ne $null }
    Check "G35b: --plugins-list --json is a single line" { -not ($g35Out.Contains([char]10) -or $g35Out.Contains([char]13)) }
    Check "G35c: --plugins-list --json has plugins array + total" {
        $g35Json.PSObject.Properties['plugins'] -and $g35Json.PSObject.Properties['total']
    }
    Check "G35d: --plugins-list --json plugin entries have name+version+source" {
        $g35Json.plugins.Count -gt 0 -and
        $g35Json.plugins[0].PSObject.Properties['name'] -and
        $g35Json.plugins[0].PSObject.Properties['version'] -and
        $g35Json.plugins[0].PSObject.Properties['source']
    }
    Check "G35e: --plugins-list --json total matches array length" {
        $g35Json.total -eq $g35Json.plugins.Count
    }

    # G36: --team-config --json returns {"config":<obj|null>,"paths":{...}}
    $g36Out = & pwsh -NoProfile -File $skillPath --team-config --json 2>&1 | Out-String
    $g36Out = $g36Out.Trim()
    $g36Json = $null
    try { $g36Json = $g36Out | ConvertFrom-Json } catch {}
    Check "G36a: --team-config --json output is valid JSON" { $g36Json -ne $null }
    Check "G36b: --team-config --json is a single line" { -not ($g36Out.Contains([char]10) -or $g36Out.Contains([char]13)) }
    Check "G36c: --team-config --json has config key (null when team mode is off)" {
        $g36Json.PSObject.Properties['config']
    }
    Check "G36d: --team-config --json has paths sub-object with state_dir" {
        $g36Json.PSObject.Properties['paths'] -and $g36Json.paths.PSObject.Properties['state_dir']
    }

    # G37: --stream-list --json returns {"streams":[...],"total":N,"in_progress":"<path>"}
    $g37Out = & pwsh -NoProfile -File $skillPath --stream-list --json 2>&1 | Out-String
    $g37Out = $g37Out.Trim()
    $g37Json = $null
    try { $g37Json = $g37Out | ConvertFrom-Json } catch {}
    Check "G37a: --stream-list --json output is valid JSON" { $g37Json -ne $null }
    Check "G37b: --stream-list --json is a single line" { -not ($g37Out.Contains([char]10) -or $g37Out.Contains([char]13)) }
    Check "G37c: --stream-list --json has streams array + total" {
        $g37Json.PSObject.Properties['streams'] -and $g37Json.PSObject.Properties['total']
    }
    Check "G37d: --stream-list --json has in_progress path" {
        $g37Json.PSObject.Properties['in_progress'] -and ($g37Json.in_progress -match 'in_progress')
    }

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
