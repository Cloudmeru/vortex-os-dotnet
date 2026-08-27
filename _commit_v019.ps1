#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
Push-Location 'C:\latihan\vortex-os-dotnet'

# 1. Stage all changes
git add -A
$staged = git diff --cached --stat
Write-Host "--- Staged for commit ---" -ForegroundColor Cyan
Write-Host $staged
Write-Host "-------------------------" -ForegroundColor Cyan

# 2. Commit
$msg = "feat: v0.1.9 -- packager worker, Golden Path template replay, operator-driven branching, engine unit tests`n`n- lib/Packager.{h,cpp} : --package <swarm_id> [--dry-run]; copies swarm deliverables to deliverables/<project>/, writes .manifest.json (engine_version, summary, per-file status + bytes + checksum). Refuses to overwrite (ADR-015).`n- lib/Template.{h,cpp} : --dispatch-template <template.json> reads objective_template + substitutions, fills {{episode_number}} / {{operator_choice}} / {{key}} from CLI overrides, writes tasks/<task_id>.md, dispatches via V4.`n- lib/Decisions.{h,cpp} : --decision-record / --decision-list; persists to state/decision_history.json. CRITICAL-gate approvals via --hitl-approve auto-record so template replays auto-carry the prior operator pick.`n- src/lib/Hitl.cpp : wires CmdHitlApprove/Deny to call Decisions::Append when severity == CRITICAL.`n- tests/test_engine.cpp : C++ unit-test source (Slugify, Substitute, decisions round-trip, ShortChecksum). Cannot be launched as a .exe due to a known .NET 5+ link.exe /SUBSYSTEM bug; see header comment in src/build-tests.ps1.`n- tests/test_engine.ps1 : PowerShell end-to-end tests (33 assertions) that exercise the same surface via skill.ps1. Exits 0 / 1.`n- src/build-tests.ps1 : compiles + links the test exe; verifies the engine surface builds without errors.`n- src/skill.cpp : 3 new CLI commands + bumped version to 0.1.9.`
n- src/build.ps1 : adds the 3 new lib sources + System.Security.Cryptography.dll to $fuList.`
n- CHANGELOG.md : full v0.1.9 entry."
git commit -m $msg
if ($LASTEXITCODE -ne 0) { throw "git commit failed: $LASTEXITCODE" }

# 3. Push
git push origin main
if ($LASTEXITCODE -ne 0) { throw "git push failed: $LASTEXITCODE" }

# 4. Tag v0.1.9
git tag -a v0.1.9 -m "v0.1.9 - packager worker + Golden Path template replay + operator-driven branching + engine unit tests"
git push origin v0.1.9
if ($LASTEXITCODE -ne 0) { throw "git push tag failed: $LASTEXITCODE" }

# 5. Show the new head + tag
git log --oneline -3
Write-Host ""
Write-Host "Tag pushed: v0.1.9" -ForegroundColor Green
Write-Host "GitHub Actions release.yml will now build Vortex.dll + Vortex.psm1 + Vortex.psd1 + ijwhost.dll and attach them to the v0.1.9 release." -ForegroundColor Green

Pop-Location
