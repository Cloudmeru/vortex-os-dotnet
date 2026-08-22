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
# which PS5 / Windows PowerShell cannot load.
if ($PSVersionTable.PSEdition -ne 'Core' -or $PSVersionTable.PSVersion.Major -lt 7) {
    throw "VORTEX-OS requires PowerShell 7+ (Core). Detected: $($PSVersionTable.PSVersion) ($($PSVersionTable.PSEdition))."
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
function script:Invoke-Skill {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)]
        [string[]] $Arguments
    )
    # Convert PowerShell string array to .NET String[] (PowerShell unwraps
    # single-element arrays in some contexts, so be explicit).
    $netArgs = [string[]] $Arguments
    [int] $rc = [Vortex.Skill]::Run($dllPath, $netArgs)
    return $rc
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

# --- Module export ----------------------------------------------------------
Export-ModuleMember -Function @(
    'Invoke-Vortex'
    'Get-VortexAgent'
    'Get-VortexAuditTrail'
    'Get-VortexHitlPending'
    'Approve-VortexHitl'
    'Deny-VortexHitl'
    'Test-VortexPackage'
)
