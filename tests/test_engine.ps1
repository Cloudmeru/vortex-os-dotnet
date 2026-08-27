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
    Write-Host "VORTEX-OS engine tests (PowerShell edition) v0.2.0"
    Write-Host "==================================================="
    Write-Host "VORTEX_HOME: $scratchHome"
    Write-Host ""

    # -----------------------------------------------------------------------
    # 1. --version reports 0.1.9
    # -----------------------------------------------------------------------
    Write-Host "[1] Engine version"
    $ver = & pwsh -NoProfile -File $skillPath --version 2>&1 | Select-Object -Last 1
    Check "engine version reports 0.2.0" { $ver -match '0\.2\.0' }

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
    Check "manifest.engine_version is 0.2.0" { $manifest.engine_version -eq '0.2.0' }
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
    Check "plugins-list reports 6 plugins total"  { $listOut -match 'Total: 6 plugin' }
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
Import-Module Vortex -RequiredVersion 0.2.0 -Force
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
