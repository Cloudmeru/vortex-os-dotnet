#Requires -Version 7.0
# Quick smoke test for v0.3.5 engine features: --recipe, agent_roster walker, --memory-show populated, reviewer gate.
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$skillPath = Join-Path $root '..\vortex-os-skill\skill.ps1'
$skillRoot = Join-Path $root '..\vortex-os-skill'
$engineDir = $root
if (-not (Test-Path $skillPath)) { throw "skill.ps1 not found at $skillPath" }

# Prepend the engine source dir to PSModulePath so the skill loads
# OUR v0.3.5 build instead of the user-scope v0.3.0 install.
$env:PSModulePath = $engineDir + [IO.Path]::PathSeparator + $env:PSModulePath
$env:VORTEX_MODULE_PATH = $engineDir

$scratchHome = Join-Path $env:TEMP "vortex-test-v035-" + (New-Guid).ToString('N').Substring(0, 8)
$env:VORTEX_HOME = $scratchHome
$env:VORTEX_NO_AUTO_UPDATE = '1'
[IO.Directory]::CreateDirectory($scratchHome) | Out-Null
$swarmsDir = Join-Path $scratchHome 'swarms'

$g_pass = 0; $g_fail = 0
function Check { param($L, $T) if (& $T) { $script:g_pass++; Write-Host "  PASS  $L" } else { $script:g_fail++; Write-Host "  FAIL  $L" -ForegroundColor Red } }

try {
    Write-Host "VORTEX-OS engine v0.3.5 smoke tests"
    Write-Host "==================================================="

    # ---- G18: --recipe ----
    Write-Host ""
    Write-Host "[G18] --recipe shortcut"
    $smokeTemplate = Join-Path $skillRoot 'templates\recipe_smoke.json'
    Set-Content -LiteralPath $smokeTemplate -Value '{"name":"recipe_smoke","version":"0.0.0","objective_template":"smoke","substitutions":{},"deliverables":[],"hitl_gates":[],"self_heal_targets":[]}' -Encoding UTF8
    $recipeOut = (& pwsh -NoProfile -File $skillPath --recipe recipe_smoke 2>&1 | Out-String)
    Check "G18: --recipe finds template by basename" { $recipeOut -match 'recipe_smoke|recipe' }
    $missingOut = (& pwsh -NoProfile -File $skillPath --recipe no_such_recipe 2>&1 | Out-String)
    $missingExit = $LASTEXITCODE
    Check "G18: --recipe with unknown name exits non-zero" { $missingExit -ne 0 }
    Check "G18: --recipe with unknown name prints an error" { $missingOut -match 'Recipe not found|ERROR' }
    Remove-Item -LiteralPath $smokeTemplate -Force -ErrorAction SilentlyContinue

    # ---- G19: agent_roster walker ----
    Write-Host ""
    Write-Host "[G19] agent_roster walker"
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

    # ---- G20: --memory-show populated ----
    Write-Host ""
    Write-Host "[G20] --memory-show with a populated store"
    # Create a minimal project so compile-memory has something to work with
    $projName = "v035_smoke"
    $projDir = Join-Path $scratchHome "deliverables\$projName"
    New-Item -ItemType Directory -Path (Join-Path $projDir 'deliverables') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $projDir '.vortex_project') -Value $projName -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $projDir 'deliverables\readme.md') -Value 'smoke test' -Encoding UTF8
    & pwsh -NoProfile -File $skillPath -Project $projName --compile-memory --project $projName 2>&1 | Out-Null
    $memOut = (& pwsh -NoProfile -File $skillPath -Project $projName --memory-show $projName 2>&1 | Out-String)
    Check "G20: --memory-show returns a non-empty slice" { $memOut.Length -gt 100 }
    Check "G20: --memory-show is not an error" { $memOut -notmatch 'no memory slice' }

    # ---- G21: Reviewer gate on --package ----
    Write-Host ""
    Write-Host "[G21] Reviewer gate on --package"
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
    # Make sure the agent manifests referenced by the gate exist
    Set-Content -LiteralPath (Join-Path $skillRoot 'agents\media-stack.json') -Value '{"name":"media-stack","version":"0.2.0","kind":"dynamic","tier":3}' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $skillRoot 'agents\reviewer.quality.json') -Value '{"name":"reviewer.quality","version":"0.1.0","kind":"dynamic","tier":2}' -Encoding UTF8
    $pkgOut = (& pwsh -NoProfile -File $skillPath --package $swarmId 2>&1 | Out-String)
    Check "G21: --package prints the Reviewer gate line" { $pkgOut -match 'Reviewer gate: reviewer.quality' -or $pkgOut -match 'REVIEWER_INVOKE' }

    Write-Host ""
    Write-Host "==================================================="
    Write-Host "Passed: $g_pass    Failed: $g_fail"
    if ($g_fail -gt 0) { exit 1 } else { exit 0 }
} finally {
    if (Test-Path $scratchHome) {
        try { [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($scratchHome, 'OnlyErrorDialogs', 'SendToRecycleBin') } catch {}
    }
}
