@{
    # Script module + binary module = the .psm1 loads the C++/CLI .dll
    RootModule        = 'Vortex.psm1'
    ModuleVersion     = '0.3.12'
    GUID              = '7c3a9f1e-1184-4e6b-83b7-5b4a4f4e3b2a'
    Author            = 'MiniMax Agent'
    CompanyName       = 'VORTEX-OS'
    Copyright         = '(c) 2026 MiniMax Agent. MIT License.'
    Description       = 'VORTEX-OS is a Hierarchical Autonomous Orchestration Engine. PowerShell 7+ module backed by a .NET 10 C++/CLI class library (Vortex.dll).'
    PowerShellVersion = '7.0'
    CompatiblePSEditions = @('Core')
    CmdletsToExport   = @()
    FunctionsToExport = @(
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
    VariablesToExport = @()
    AliasesToExport    = @()
    FileList          = @('Vortex.psm1', 'Vortex.dll', 'ijwhost.dll', 'en-US\about_Vortex.help.txt', 'LICENSE', 'README.md', 'CHANGELOG.md')
    PrivateData       = @{
        PSData = @{
            Tags       = @('VORTEX-OS', 'Vortex', 'autonomous', 'orchestration', 'agent', 'HITL', 'continuity', 'MiniMax', 'multimodal', 'swarm', 'clixml', 'audit', 'self-heal', 'plugin', 'extensible', 'install', 'team-mode', 'streaming')
            LicenseUri = 'https://github.com/Cloudmeru/vortex-os-dotnet/blob/main/LICENSE'
            ProjectUri = 'https://github.com/Cloudmeru/vortex-os-dotnet'
            ReleaseNotes = 'https://github.com/Cloudmeru/vortex-os-dotnet/releases/tag/v0.3.0'
        }
    }
}
