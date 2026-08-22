# VORTEX-OS (.NET 10 engine)

**The .NET 10 C++/CLI class library that powers the [VORTEX-OS skill](https://github.com/Cloudmeru/vortex-os-skill).**

This repo is the upstream source. It produces a single `Vortex.dll`
(C++/CLI, .NET 10) that exposes the engine as a managed API
(`Vortex.Skill`, `Vortex.Verify`). A thin PowerShell 7+ module
(`Vortex.psm1` + `Vortex.psd1`) loads the DLL via `Add-Type` and exposes
it as a set of cmdlets.

## Repository layout

```
vortex-os-dotnet/
├── Vortex.psd1               ← PowerShell module manifest (PSGallery entry point)
├── Vortex.psm1               ← PowerShell module (loads Vortex.dll, exports cmdlets)
├── en-US/
│   └── about_Vortex.help.txt ← PowerShell Get-Help content
├── src/                      ← C++/CLI source (compiled into Vortex.dll)
│   ├── VortexCommon.h
│   ├── VortexPublic.h
│   ├── skill.cpp              (Vortex::Skill implementation)
│   ├── verify.cpp             (Vortex::Verify implementation)
│   ├── lib/                   (engine modules — Commands, Swarm, Hitl, …)
│   └── build.ps1              (cl.exe /clr:netcore build, no VS prompt needed)
├── .github/workflows/        (CI: build on every push; release: publish to PSGallery on tag)
├── Vortex.dll                (built artifact — also published)
├── ijwhost.dll               (built artifact — runtime dep, deployed alongside)
├── LICENSE
├── CHANGELOG.md
└── README.md
```

## Build

The engine compiles with **MSVC v143** (Visual Studio 2022 17.10+)
targeting **.NET 10** via `/clr:netcore`. PowerShell 7+ hosts the CLR —
no apphost / `runtimeconfig.json` / separate launcher is required.

```powershell
# From a normal PowerShell prompt (the script invokes vcvars64.bat itself):
pwsh -NoProfile -File src\build.ps1

# Or from a "x64 Native Tools Command Prompt for VS 2022":
src\build.ps1
```

Build output:
- `Vortex.dll` — the C++/CLI engine (drop into the skill package, or
  `Install-Module Vortex` after publishing to PSGallery)
- `ijwhost.dll` — the .NET 10 IJW host stub (deployed next to `Vortex.dll`)

## Install from PSGallery (when published)

```powershell
Install-Module -Name Vortex -Scope CurrentUser
Import-Module Vortex
Get-VortexAgent
```

## Install from source (this repo)

```powershell
# 1. Build the engine
pwsh -NoProfile -File src\build.ps1

# 2. Stage the module artifacts
Copy-Item src\..\Vortex.dll, src\..\ijwhost.dll, Vortex.psd1, Vortex.psm1, en-US `
          -Destination $env:USERPROFILE\Documents\PowerShell\Modules\Vortex\

# 3. Import
Import-Module Vortex
```

## Versioning & releases

We follow [Semantic Versioning](https://semver.org/). Tags of the form
`vMAJOR.MINOR.PATCH` (e.g. `v0.1.0`) trigger the **release** workflow
which:

1. Builds `Vortex.dll` on a clean Windows runner.
2. Packages `Vortex.psd1` + `Vortex.psm1` + `Vortex.dll` + `ijwhost.dll`
   + `en-US/` + `LICENSE` into a `.nupkg`.
3. Publishes the package to the PowerShell Gallery via `Publish-Module`.
4. Creates a GitHub Release with the build artifacts attached.

Required secret: `PSGALLERY_API_KEY` (the user's NuGetApiKey from
[PowerShell Gallery](https://www.powershellgallery.com)).

## License

[MIT](./LICENSE)

## See also

- **[Cloudmeru/vortex-os-skill](https://github.com/Cloudmeru/vortex-os-skill)** —
  the agent skill that bundles this engine and ships the LLM-facing
  surface (`SKILL.md`, `INSTRUCTIONS.md`, `_meta.json`, `agents/`).
- **[/r/dotnet/fortex-os-dotnet]** — older local working copy
