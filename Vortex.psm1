# =============================================================================
# VORTEX-OS — PowerShell 7+ module
# =============================================================================
# Loads the native C++/CLI class library (Vortex.dll) into PowerShell's
# existing .NET 10 CLR and exposes its CLI dispatcher as PowerShell cmdlets.
#
#   PS> Import-Module Vortex
#   PS> Get-VortexAgent                  # list all 3 supervisor/inspector agents
#   PS> Invoke-Vortex --agents-lint     # lint them
#   PS> Invoke-Vortex --dispatch-master my_project\objective.md
#   PS> Get-VortexHitlPending
#   PS> Approve-VortexHitl -TaskId package_websim
#
# This is the modern .NET 5+ architecture for C++/CLI: the C++ project is a
# class library, not an executable; PowerShell (or any other .NET host) is
# what loads and runs it. We do NOT need apphost.exe, ijwhost.dll bootstrap,
# or runtimeconfig.json — PowerShell already has the CLR initialized.
# =============================================================================
$ErrorActionPreference = 'Stop'

# --- PS edition / version guard ---------------------------------------------
# Requires PowerShell 7+ (Core edition) because Vortex.dll targets .NET 10,
# which PS5 / Windows PowerShell cannot load. The error message points
# the operator at install-powershell7.ps1 (shipped with the skill) so a
# fresh Windows install with only PS5.1 can bootstrap itself without
# having to know about winget / msi / pwsh first.
if ($PSVersionTable.PSEdition -ne 'Core' -or $PSVersionTable.PSVersion.Major -lt 7) {
    $msg = @"
VORTEX-OS requires PowerShell 7+ (Core edition) because Vortex.dll targets .NET 10,
which the built-in Windows PowerShell 5.1 cannot load.

  Detected:  $($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition))
  Need:      PowerShell 7.2+ (Core)

Bootstrap PowerShell 7 from this PowerShell 5.1 session by running:

  powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\install-powershell7.ps1`"

Or, if you already have pwsh 7 installed, just invoke this module with `pwsh` instead
of `powershell`: `pwsh -NoProfile -Command "Import-Module Vortex; Get-VortexAgent"`.

The install-powershell7.ps1 script downloads the official Microsoft PS7 MSI to a
temp folder and runs it silently with /quiet /norestart flags. It works on
Windows 10 1809+ and Server 2019+. No admin elevation is required when running
under the current user's context.
"@
    throw $msg
}

# --- Locate the package root and load the C++/CLI DLL -----------------------
# $PSScriptRoot is the directory containing this .psm1. The DLL + native
# runtime stub (ijwhost.dll) live next to it.
$script:VortexRoot = $PSScriptRoot
$dllPath = Join-Path $script:VortexRoot 'Vortex.dll'
$ijwHostPath = Join-Path $script:VortexRoot 'ijwhost.dll'

if (-not (Test-Path $dllPath)) {
    throw "Vortex.dll not found at $dllPath. Run src\build.ps1 first."
}
if (-not (Test-Path $ijwHostPath)) {
    throw "ijwhost.dll not found at $ijwHostPath. Copy it from the .NET 10 host pack: C:\Program Files\dotnet\packs\Microsoft.NETCore.App.Host.win-x64\10.0.*\runtimes\win-x64\native\ijwhost.dll"
}

# Add-Type loads the mixed-mode C++/CLI assembly into the current AppDomain.
# This is the single line that bridges PowerShell -> C++/CLI. Once loaded,
# the Vortex.Skill and Vortex.Verify types are visible just like any other
# .NET type.
Add-Type -Path $dllPath

# --- Internal helper: invoke the C++/CLI dispatcher with an argv array ------
# The C++/CLI engine writes its results to Console.Out; we capture that
# stream for the duration of the call and emit each line as pipeline
# output. The engine's exit code is stored in $script:VortexLastRc and is
# also available via the public Get-VortexLastExitCode cmdlet (for
# scripts like skill.ps1 that want to `exit $rc` after dispatching).
#
# The exit code is NOT emitted to the pipeline -- mixing it with the
# captured lines would corrupt downstream Format-Table / Where-Object
# pipelines. Callers that need it should call Get-VortexLastExitCode.
#
# Skill root resolution: the engine resolves the package root (where
# agents/, state/, memory/, deliverables/ live) from the first argument
# to Vortex.Skill::Run. The skill's skill.ps1 / verify.ps1 set
# $env:VORTEX_SKILL_ROOT to the skill folder so the engine finds the
# skill's agents/. If the env var is unset (e.g. a user did
# `Import-Module Vortex` in a fresh session), we fall back to the DLL's
# directory, which matches the bash `cd $(dirname $0) && pwd` semantics.
$script:VortexLastRc = 0
function script:Invoke-Skill {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)]
        [string[]] $Arguments
    )
    $netArgs = [string[]] $Arguments
    $root = if ($env:VORTEX_SKILL_ROOT) { $env:VORTEX_SKILL_ROOT } else { $dllPath }
    $sw = [System.IO.StringWriter]::new()
    $prevOut = [Console]::Out
    try {
        [Console]::SetOut($sw)
        $script:VortexLastRc = [Vortex.Skill]::Run($root, $netArgs)
    } finally {
        [Console]::SetOut($prevOut)
    }
    $captured = $sw.ToString().TrimEnd("`r", "`n")
    if ($captured.Length -gt 0) {
        # Emit each non-empty line as a separate pipeline element so callers
        # can pipe to Where-Object / ForEach-Object / Tee-Object as usual.
        foreach ($line in ($captured -split "(`r`n)|(`n)")) {
            if ($line -ne $null -and $line.Length -gt 0) { Write-Output $line }
        }
    }
}

# =============================================================================
# Public cmdlets
# =============================================================================

function Invoke-Vortex {
<#
.SYNOPSIS
    Dispatcher for the VORTEX-OS engine. Forwards arguments to Vortex.Skill.Run.
.DESCRIPTION
    Thin wrapper over the C++/CLI dispatcher. Use this for any subcommand
    that doesn't have a dedicated cmdlet below (--agents-inspect,
    --dispatch-template, --inspector-check, etc.).
.EXAMPLE
    PS> Invoke-Vortex --agents-discover
.EXAMPLE
    PS> Invoke-Vortex --dispatch-master my_project\objective.md
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)]
        [string[]] $Arguments
    )
    Invoke-Skill -Arguments $Arguments
}

function Get-VortexAgent {
<#
.SYNOPSIS
    List all VORTEX-OS agents (supervisor.store, supervisor.shift, inspector.governance).
#>
    [CmdletBinding()]
    param(
        [switch] $IncludeDeprecated,
        [switch] $AsJson
    )
    $args = @('--agents-discover')
    if ($IncludeDeprecated) { $args += '--include-deprecated' }
    if ($AsJson) { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Get-VortexAuditTrail {
<#
.SYNOPSIS
    Print the last 50 entries of the VORTEX-OS audit log (memory\audit.jsonl).
#>
    [CmdletBinding()]
    param()
    Invoke-Skill -Arguments @('--audit-trail')
}

function Get-VortexHitlPending {
<#
.SYNOPSIS
    List pending Human-in-the-Loop approval requests.
#>
    [CmdletBinding()]
    param()
    Invoke-Skill -Arguments @('--hitl-status')
}

function Approve-VortexHitl {
<#
.SYNOPSIS
    Approve a pending HITL request, releasing the VORTEX-OS gate.
.PARAMETER TaskId
    The task_id that was printed in the PENDING_HUMAN halt message.
.EXAMPLE
    PS> Approve-VortexHitl -TaskId package_websim
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)]
        [string] $TaskId
    )
    Invoke-Skill -Arguments @('--hitl-approve', $TaskId)
}

function Deny-VortexHitl {
<#
.SYNOPSIS
    Deny a pending HITL request, aborting the VORTEX-OS gate.
.PARAMETER TaskId
    The task_id that was printed in the PENDING_HUMAN halt message.
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)]
        [string] $TaskId
    )
    Invoke-Skill -Arguments @('--hitl-deny', $TaskId)
}

function Test-VortexPackage {
<#
.SYNOPSIS
    Run the VORTEX-OS post-upload verification (the C++/CLI verify engine).
.DESCRIPTION
    Returns $true if all checks pass, $false otherwise. Use this as a CI
    gate or as a smoke test after `src/build.ps1`.
.EXAMPLE
    PS> if (Test-VortexPackage) { Write-Host "ready to deploy" } else { throw "verification failed" }
#>
    [CmdletBinding()]
    param()
    $rc = [Vortex.Verify]::Run($script:VortexRoot)
    return ($rc -eq 0)
}

function Get-VortexLastExitCode {
<#
.SYNOPSIS
    Return the exit code from the most recent Invoke-Vortex / Invoke-Skill call.
.DESCRIPTION
    The Vortex cmdlets emit only their data (no trailing exit code) so they
    can be piped cleanly to Format-Table, Where-Object, etc. Scripts that
    want to honor the engine's exit code (e.g. skill.ps1 doing
    `exit $rc`) should call Get-VortexLastExitCode after the cmdlet.
.EXAMPLE
    PS> Get-VortexAgent
    PS> exit (Get-VortexLastExitCode)
#>
    [CmdletBinding()]
    param()
    return $script:VortexLastRc
}

function Get-VortexVersion {
<#
.SYNOPSIS
    Print the VORTEX-OS engine version (loaded Vortex.dll).
.DESCRIPTION
    The version is read from the Vortex.psd1 manifest next to Vortex.dll
    (the same ModuleVersion field the release workflow publishes). This is
    the canonical answer to "which engine is installed"; equivalent to
    running `skill.ps1 --version` but does not require the skill to be
    on disk.
.EXAMPLE
    PS> Get-VortexVersion
    0.3.0
#>
    [CmdletBinding()]
    param()
    $psd1 = Join-Path $script:VortexRoot 'Vortex.psd1'
    if (-not (Test-Path $psd1)) {
        throw "Vortex.psd1 not found at $psd1. The Vortex module is corrupt or incomplete."
    }
    $meta = Import-PowerShellDataFile -LiteralPath $psd1
    return $meta.ModuleVersion
}

function Get-VortexMemory {
<#
.SYNOPSIS
    Browse the cross-project memory store at $VORTEX_HOME\memory\derived\.
.DESCRIPTION
    v0.3.0 (PRD-17): the memory store has 3 derived artifact types --
    project fingerprints, an operator profile, and series progression
    templates. Run `skill.ps1 --compile-memory` first to populate it,
    then use this cmdlet to inspect what the engine learned.
.PARAMETER Project
    Return the fingerprint for one project. Returns the contents of
    memory\derived\project\<slug>.json if it exists.
.PARAMETER Series
    Return the progression template for one series (e.g. "client-acme").
.PARAMETER Operator
    Return the global operator profile (memory\derived\operator.json).
.PARAMETER As
    summary (default) -- print a one-paragraph digest.
    detail         -- print the full JSON.
    json           -- return the raw JSON string for ConvertFrom-Json.
.PARAMETER Recompile
    Run `skill.ps1 --compile-memory` first so the memory is fresh.
.EXAMPLE
    PS> Get-VortexMemory
    PS> Get-VortexMemory -Project client_acme
    PS> Get-VortexMemory -Series client-acme
    PS> Get-VortexMemory -Operator -As detail
#>
    [CmdletBinding()]
    param(
        [string] $Project,
        [string] $Series,
        [switch] $Operator,
        [ValidateSet('summary', 'detail', 'json')]
        [string] $As = 'summary',
        [switch] $Recompile
    )
    if ($Recompile) { Invoke-Skill -Arguments @('--compile-memory') | Out-Null }
    $root = if ($env:VORTEX_HOME) { $env:VORTEX_HOME } else { (Join-Path $env:APPDATA 'Vortex-OS') }
    $derived = Join-Path $root 'memory\derived'
    if (-not (Test-Path $derived)) {
        Write-Host "(no memory store at $derived; run skill.ps1 --compile-memory first)"
        return
    }
    $file = $null
    if ($Project) { $file = Join-Path $derived "project\$Project.json" }
    elseif ($Series) { $file = Join-Path $derived "series\$Series.json" }
    elseif ($Operator) { $file = Join-Path $derived 'operator.json' }
    else {
        # No filter -- print the index.
        $file = Join-Path $derived 'index.json'
    }
    if (-not (Test-Path $file)) {
        Write-Host "(no memory file at $file; run skill.ps1 --compile-memory to populate)"
        return
    }
    $content = Get-Content -LiteralPath $file -Raw
    if ($As -eq 'json') { return $content }
    if ($As -eq 'detail') {
        Write-Host $content
        return
    }
    # summary: print the first few key fields.
    try {
        $obj = $content | ConvertFrom-Json
    } catch {
        Write-Host "(invalid JSON in $file)"
        return $content
    }
    $schema = $obj.schema
    Write-Host "schema:        $schema"
    if ($obj.PSObject.Properties['project'])            { Write-Host "project:       $($obj.project)" }
    if ($obj.PSObject.Properties['series'])              { Write-Host "series:        $($obj.series)" }
    if ($obj.PSObject.Properties['projects_analyzed'])   { Write-Host "projects:      $($obj.projects_analyzed)" }
    if ($obj.PSObject.Properties['compiled_at'])         { $ts = [datetime]'1970-01-01Z'; $ts = $ts.AddSeconds([int]$obj.compiled_at); Write-Host ("compiled_at:   {0:o}" -f $ts.ToUniversalTime()) }
    if ($obj.PSObject.Properties['project_type_hint'])   { Write-Host "type_hint:     $($obj.project_type_hint)" }
    if ($obj.PSObject.Properties['stats']) {
        $s = $obj.stats
        if ($s.PSObject.Properties['deliverables_total']) { Write-Host "deliverables:  $($s.deliverables_total)" }
        if ($s.PSObject.Properties['cost_usd_total'])     { Write-Host ("cost_usd:      {0:0.0000}" -f [double]$s.cost_usd_total) }
        if ($s.PSObject.Properties['tokens_total'])       { Write-Host "tokens:        $($s.tokens_total)" }
        if ($s.PSObject.Properties['self_heal_cycles'])   { Write-Host "self_heals:    $($s.self_heal_cycles)" }
        if ($s.PSObject.Properties['hitl_gates'])         { Write-Host "hitl_gates:    $($s.hitl_gates)" }
    }
    if ($obj.PSObject.Properties['plugin_usage'] -and $obj.plugin_usage.PSObject.Properties.Count -gt 0) {
        Write-Host "plugin_usage:"
        $obj.plugin_usage.PSObject.Properties | Sort-Object Value -Descending | ForEach-Object {
            Write-Host ("  {0,-20} {1}" -f $_.Name, $_.Value)
        }
    }
    if ($obj.PSObject.Properties['common_components'] -and $obj.common_components.Count -gt 0) {
        Write-Host "common_components:"
        $obj.common_components | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }
    }
    if ($obj.PSObject.Properties['episodes'] -and $obj.episodes.Count -gt 0) {
        Write-Host "episodes:"
        $obj.episodes | ForEach-Object { Write-Host "  $_" }
    }
}

function Get-VortexPlugin {
<#
.SYNOPSIS
    List installed VORTEX-OS plugins, or dump a single plugin's manifest.
.DESCRIPTION
    The engine discovers plugins from two locations:
      1. $VORTEX_HOME\plugins\         (user-scope, durable)
      2. <skill>\plugins\              (skill-scope, ships with the skill)
    On conflict (same name in both) the user-scope plugin wins.
.PARAMETER Name
    The name of a specific plugin. If omitted, lists all discovered plugins.
.PARAMETER AsJson
    (Added v0.3.10) Return the listing as a single-line JSON object per
    docs/cli-json-contract.md instead of a table. Useful for piping
    into ConvertFrom-Json / Where-Object. Has no effect when -Name
    is set (the engine's --plugins-info already emits JSON).
.EXAMPLE
    PS> Get-VortexPlugin                       # list all
    PS> Get-VortexPlugin -Name audio-foley    # dump audio-foley's manifest
    PS> Get-VortexPlugin -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [string] $Name,
        [switch] $AsJson
    )
    if ($Name) {
        Invoke-Skill -Arguments @('--plugins-info', $Name)
    } else {
        $args = @('--plugins-list')
        if ($AsJson) { $args += '--json' }
        Invoke-Skill -Arguments $args
    }
}

# v0.2.3 (G11): expose more wrapper commands as standalone cmdlets so
# users don't have to round-trip through skill.ps1 for the common ops.
# These are thin wrappers (single-arg pass-throughs) -- the engine
# remains the source of truth for argument parsing and behavior.

function Get-VortexDecision {
<#
.SYNOPSIS
    List the operator decision history (memory\decision_history.json).
.DESCRIPTION
    Returns each decision as a row: task, gate, severity, choice, reason,
    timestamp. CRITICAL/HIGH decisions are highlighted.
.PARAMETER AsJson
    (Added v0.3.10) Return the history as a single-line
    {"decisions":[<obj>,<obj>,...]} JSON object per docs/cli-json-contract.md.
    Pipe into ConvertFrom-Json to get an array of decision objects.
.EXAMPLE
    PS> Get-VortexDecision
    PS> Get-VortexDecision -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [switch] $AsJson
    )
    $args = @('--decision-list')
    if ($AsJson) { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Send-VortexDecision {
<#
.SYNOPSIS
    Record an operator decision (gate answer) in the decision history.
.DESCRIPTION
    Mirrors the --decision-record engine command. Decisions are
    append-only and feed the decision_history.json file that the
    DispatchV4 prompt builder reads to inject "operator notes" into
    the next dispatch.
.PARAMETER Task
    The task_id this decision applies to.
.PARAMETER Gate
    The gate name (e.g. "g1", "g2_moral_hinge").
.PARAMETER Choice
    The operator's choice text.
.PARAMETER Reason
    Free-form justification (stored in the audit log).
.PARAMETER Severity
    LOW / MEDIUM / HIGH / CRITICAL. Default: HIGH.
.PARAMETER Episode
    Optional episode number for episodic projects.
.EXAMPLE
    PS> Send-VortexDecision -Task ep1 -Gate g1 -Severity HIGH -Choice "approve script" -Reason "looks good" -Episode 1
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $Task,
        [Parameter(Mandatory)] [string] $Gate,
        [Parameter(Mandatory)] [string] $Choice,
        [string] $Reason = '',
        [ValidateSet('LOW', 'MEDIUM', 'HIGH', 'CRITICAL')] [string] $Severity = 'HIGH',
        [int] $Episode = 0
    )
    $args = @('--decision-record', '--task', $Task, '--gate', $Gate,
              '--severity', $Severity, '--choice', $Choice, '--reason', $Reason)
    if ($Episode -gt 0) { $args += @('--episode', $Episode) }
    Invoke-Skill -Arguments $args
}

function Get-VortexCostReport {
<#
.SYNOPSIS
    Print the cost rollup across all dispatched agents.
.DESCRIPTION
    Same as --cost-report. By default prints a text table. Pass -Json
    for the machine-readable rollup (use this with ConvertFrom-Json).
.PARAMETER Project
    Filter to a single project.
.PARAMETER Since
    Only show dispatches from the last N days.
.PARAMETER Agent
    Filter to a single agent.
.PARAMETER AsJson
    Return the rollup as JSON.
.EXAMPLE
    PS> Get-VortexCostReport
    PS> Get-VortexCostReport -Project my_project -Since 7
    PS> Get-VortexCostReport -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [string] $Project,
        [int] $Since = 0,
        [string] $Agent,
        [switch] $AsJson
    )
    $args = @('--cost-report')
    if ($Project) { $args += @('--project', $Project) }
    if ($Since   -gt 0) { $args += @('--since', $Since) }
    if ($Agent)  { $args += @('--agent',  $Agent) }
    if ($AsJson) { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Get-VortexProjectBudget {
<#
.SYNOPSIS
    Show a project's budget (tokens_total, usd_total, so_far, % used).
.PARAMETER Project
    The project slug (e.g. "trial_of_echoes"). Defaults to the current
    $env:VORTEX_PROJECT if set.
.PARAMETER AsJson
    (Added v0.3.10) Return as a single-line
    {"project":..,"tokens_total":..,"usd_total":..,"so_far":{..},"percent_used":..}
    JSON object per docs/cli-json-contract.md. In JSON mode, missing
    -Project produces {"error":"..."} instead of a Usage message.
.EXAMPLE
    PS> Get-VortexProjectBudget -Project my_project
    PS> Get-VortexProjectBudget -Project my_project -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [string] $Project = $env:VORTEX_PROJECT,
        [switch] $AsJson
    )
    $args = @('--budget-show')
    if ($Project) { $args += @('--project', $Project) }
    if ($AsJson)  { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Set-VortexProjectBudget {
<#
.SYNOPSIS
    Write a per-project budget to deliverables\<project>\_meta.json.
.DESCRIPTION
    A budget alert at 80% yields a PENDING_HUMAN gate (severity MEDIUM);
    at 100% the gate is CRITICAL. Alerts are rate-limited to once per
    day per project.
.PARAMETER Project
    The project slug.
.PARAMETER TokensTotal
    Token budget (optional).
.PARAMETER UsdTotal
    USD budget (optional).
.EXAMPLE
    PS> Set-VortexProjectBudget -Project my_project -UsdTotal 5.00
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $Project,
        [long]   $TokensTotal = 0,
        [double] $UsdTotal    = 0
    )
    $args = @('--budget-set', '--project', $Project)
    if ($TokensTotal -gt 0) { $args += @('--tokens-total', $TokensTotal) }
    if ($UsdTotal    -gt 0) { $args += @('--usd-total',    $UsdTotal) }
    Invoke-Skill -Arguments $args
}

function Get-VortexAgentGraph {
<#
.SYNOPSIS
    Print the agent dependency graph (T0 -> T1 -> T2 -> T3 + T4).
.PARAMETER Format
    ascii (default) or mermaid. The mermaid format is suitable for
    pasting into a Markdown document.
.EXAMPLE
    PS> Get-VortexAgentGraph
    PS> Get-VortexAgentGraph -Format mermaid
#>
    [CmdletBinding()]
    param(
        [ValidateSet('ascii', 'mermaid')] [string] $Format = 'ascii'
    )
    Invoke-Skill -Arguments @('--agents-graph', '--format', $Format)
}

function Test-VortexAgent {
<#
.SYNOPSIS
    Lint one or all agents. Returns $true if all pass, $false otherwise.
.PARAMETER Name
    The agent name. If omitted, lints all agents.
.EXAMPLE
    PS> Test-VortexAgent
    PS> Test-VortexAgent -Name supervisor.store
#>
    [CmdletBinding()]
    param(
        [string] $Name
    )
    $target = if ($Name) { $Name } else { '--all' }
    $lint = Invoke-Skill -Arguments @('--agents-lint', $target)
    return ($lint -match 'LINT_OK') -and ($lint -notmatch 'LINT_FAIL')
}

function Start-VortexPackage {
<#
.SYNOPSIS
    Package one swarm's intermediate deliverables into the project's
    durable deliverables/ directory.
.DESCRIPTION
    Writes a .manifest.json next to the deliverables and refuses to
    overwrite any file that already exists at the target (per ADR-015).
    Use -DryRun to preview what would be copied.
.PARAMETER SwarmId
    The swarm id (looks for swarms\active_<swarmId>\deliverables\).
.PARAMETER DryRun
    Print what would happen, don't actually copy.
.EXAMPLE
    PS> Start-VortexPackage -SwarmId my_swarm_001
    PS> Start-VortexPackage -SwarmId my_swarm_001 -DryRun
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $SwarmId,
        [switch] $DryRun
    )
    $args = @('--package', $SwarmId)
    if ($DryRun) { $args += '--dry-run' }
    Invoke-Skill -Arguments $args
}

function Get-VortexStream {
<#
.SYNOPSIS
    List the in-progress dispatches (those with a .started manifest in
    state\<user>\in_progress\<id>).
.DESCRIPTION
    v0.2.2: per-user sharding means the InProgressDir is
    state\<user>\in_progress\ when team_mode is on, or state\in_progress\
    otherwise. The output table includes a 'started' timestamp + the
    partials count, and a final 'in_progress: <path>' line.
.PARAMETER AsJson
    (Added v0.3.10) Return as a single-line
    {"streams":[<obj>,...],"total":N,"in_progress":"<path>"}
    JSON object per docs/cli-json-contract.md. The "in_progress"
    key is the absolute path of the in-progress root so an
    interactive streamer UI can locate the .partial files directly.
.EXAMPLE
    PS> Get-VortexStream
    PS> Get-VortexStream -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [switch] $AsJson
    )
    $args = @('--stream-list')
    if ($AsJson) { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Send-VortexStreamHint {
<#
.SYNOPSIS
    Append an operator hint to a streaming dispatch's .hints.jsonl file.
.DESCRIPTION
    The next dispatch in the chain reads this file and injects the
    hints as "operator notes" in the prompt. Use this to steer an
    in-progress dispatch without waiting for it to finish.
.PARAMETER TaskId
    The in-progress task id.
.PARAMETER Text
    The hint text (e.g. "Tone is too dark, lighten the next scene").
.EXAMPLE
    PS> Send-VortexStreamHint -TaskId ep2_smoke -Text "Tone is too dark"
#>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $TaskId,
        [Parameter(Mandatory)] [string] $Text
    )
    Invoke-Skill -Arguments @('--hint', $TaskId, '--text', $Text)
}

function Get-VortexTeamConfig {
<#
.SYNOPSIS
    Print the loaded team-mode config + the resolved per-user shard paths.
.DESCRIPTION
    v0.2.2: reads $VORTEX_HOME\.vortex\config.json and dumps it as JSON
    plus the resolved Paths. Useful for verifying that team_mode is
    on (or off) and that the per-user shards point at the right place.
.PARAMETER AsJson
    (Added v0.3.10) Return as a single-line
    {"config":<obj-or-null>,"paths":{...}} JSON object per
    docs/cli-json-contract.md. The "config" key is null when team
    mode is off (no .vortex/config.json) so consumers can detect
    this state without an empty object.
.EXAMPLE
    PS> Get-VortexTeamConfig
    PS> Get-VortexTeamConfig -AsJson | ConvertFrom-Json
#>
    [CmdletBinding()]
    param(
        [switch] $AsJson
    )
    $args = @('--team-config')
    if ($AsJson) { $args += '--json' }
    Invoke-Skill -Arguments $args
}

function Invoke-VortexVectorHydrate {
<#
.SYNOPSIS
    Initialize the vector store at memory\vectors.db (or the JSON
    sidecar if sqlite3 is not on PATH).
.DESCRIPTION
    v0.2.3: applies lib\vector_schema.sql when sqlite3 is available;
    otherwise writes a memory\vectors.json sidecar so downstream readers
    have a durable artifact.
.EXAMPLE
    PS> Invoke-VortexVectorHydrate
#>
    [CmdletBinding()]
    param()
    Invoke-Skill -Arguments @('--vector-hydrate')
}

# --- Module export ----------------------------------------------------------
Export-ModuleMember -Function @(
    'Invoke-Vortex'
    'Get-VortexAgent'
    'Get-VortexAgentGraph'
    'Test-VortexAgent'
    'Get-VortexAuditTrail'
    'Get-VortexHitlPending'
    'Approve-VortexHitl'
    'Deny-VortexHitl'
    'Get-VortexDecision'
    'Send-VortexDecision'
    'Get-VortexCostReport'
    'Get-VortexProjectBudget'
    'Set-VortexProjectBudget'
    'Start-VortexPackage'
    'Get-VortexStream'
    'Send-VortexStreamHint'
    'Get-VortexTeamConfig'
    'Invoke-VortexVectorHydrate'
    'Get-VortexMemory'
    'Test-VortexPackage'
    'Get-VortexLastExitCode'
    'Get-VortexVersion'
    'Get-VortexPlugin'
)
