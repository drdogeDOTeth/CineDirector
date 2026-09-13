<#
.SYNOPSIS
    Install CineDirector once, instead of copying it into every project.

.DESCRIPTION
    Two ways to stop keeping copies. They solve different problems, so pick by
    what you are doing rather than by which sounds tidier.

    -Engine packages the plugin with its compiled binaries and puts it in
    <Engine>\Engine\Plugins\Marketplace, where every project on that engine sees
    it with no setup at all. This is the right answer once the plugin is stable.
    The cost is that an installed engine cannot compile its own engine plugins,
    so every C++ change means re-running this, and every engine patch invalidates
    the binaries and needs the same.

    -Project <path> junctions this checkout into one project's Plugins folder.
    That is one command per project rather than none, but the project compiles
    the plugin itself the normal way, so editing C++ and hitting Live Coding
    still works. This is the right answer for whichever project you are actually
    developing in.

    The two can coexist across different projects, but never for the same
    project: two plugins with the same name is a startup error.

.PARAMETER Engine
    Build and install into the engine, for every project at once.

.PARAMETER Project
    Path to a .uproject, or to the folder holding one. Links this checkout into
    that project's Plugins folder.

.PARAMETER EngineRoot
    Which engine to build with and install into. Default: C:\Program Files\Epic Games\UE_5.8.

.PARAMETER Uninstall
    Remove instead of install. Combine with -Engine or -Project.

.EXAMPLE
    .\Tools\Install-Plugin.ps1 -Engine
.EXAMPLE
    .\Tools\Install-Plugin.ps1 -Project "C:\Users\Me\Documents\Unreal Projects\MyGame"
.EXAMPLE
    .\Tools\Install-Plugin.ps1 -Engine -Uninstall
#>

[CmdletBinding(DefaultParameterSetName = 'Engine')]
param(
    [Parameter(ParameterSetName = 'Engine')]
    [switch] $Engine,

    [Parameter(ParameterSetName = 'Project', Mandatory = $true)]
    [string] $Project,

    [string] $EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',

    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

$PluginName = 'CineDirector'
$SourceRoot = Split-Path -Parent $PSScriptRoot
$UPlugin    = Join-Path $SourceRoot "$PluginName.uplugin"

if (-not (Test-Path $UPlugin)) {
    throw "$PluginName.uplugin is not in $SourceRoot. Run this script from inside the repo."
}

function Remove-Install([string] $Path) {
    if (-not (Test-Path $Path)) { return $false }
    $item = Get-Item $Path -Force
    if ($item.LinkType) {
        # Deleting a link never touches what it points at.
        $item.Delete()
    } else {
        Remove-Item $Path -Recurse -Force
    }
    return $true
}

# ---------------------------------------------------------------------------
# Into one project: a junction, so there is still only one copy of the source.
# ---------------------------------------------------------------------------

if ($PSCmdlet.ParameterSetName -eq 'Project') {
    if (Test-Path $Project -PathType Leaf) { $Project = Split-Path -Parent $Project }
    if (-not (Test-Path $Project)) { throw "No such project folder: $Project" }
    if (-not (Get-ChildItem $Project -Filter *.uproject)) {
        throw "No .uproject in $Project."
    }

    $pluginsDir = Join-Path $Project 'Plugins'
    $target     = Join-Path $pluginsDir $PluginName

    Write-Host "Project: $Project"
    Write-Host "Target:  $target"

    if (Remove-Install $target) { Write-Host "Removed the existing install." }
    if ($Uninstall) {
        Write-Host "Uninstalled. Regenerate project files and rebuild." -ForegroundColor Green
        exit 0
    }

    New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
    # A junction, not a symlink: junctions need no elevation and no developer mode.
    New-Item -ItemType Junction -Path $target -Value $SourceRoot | Out-Null

    Write-Host "Linked to $SourceRoot" -ForegroundColor Green
    Write-Host ""
    Write-Host "Now right-click the .uproject, Generate Visual Studio project files, and rebuild."
    exit 0
}

# ---------------------------------------------------------------------------
# Into the engine: needs binaries, because an installed engine will not compile
# its own engine plugins. BuildPlugin produces exactly that layout.
# ---------------------------------------------------------------------------

if (-not (Test-Path (Join-Path $EngineRoot 'Engine\Plugins'))) {
    throw "$EngineRoot does not contain Engine\Plugins, so it is not an engine root."
}

$marketplace = Join-Path $EngineRoot 'Engine\Plugins\Marketplace'
$target      = Join-Path $marketplace $PluginName

Write-Host "Engine:  $EngineRoot"
Write-Host "Target:  $target"

if (Remove-Install $target) { Write-Host "Removed the existing install." }
if ($Uninstall) {
    Write-Host "Uninstalled. Restart the editor." -ForegroundColor Green
    exit 0
}

$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path $uat)) { throw "RunUAT.bat not found at $uat." }

# Staged outside the repo: BuildPlugin copies the plugin folder into this
# directory, so packaging into a subfolder of the source would recurse.
$staging = Join-Path ([System.IO.Path]::GetTempPath()) "CineDirectorPackage"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }

Write-Host ""
Write-Host "Building. This compiles the whole plugin and takes a few minutes."
& $uat BuildPlugin -Plugin="$UPlugin" -Package="$staging" -TargetPlatforms=Win64
if ($LASTEXITCODE -ne 0) {
    throw "BuildPlugin failed with exit code $LASTEXITCODE. The log path is printed above."
}

if (-not (Test-Path (Join-Path $staging 'Binaries\Win64'))) {
    throw "BuildPlugin reported success but produced no Binaries\Win64 in $staging."
}

Write-Host ""
Write-Host "Installing..."
New-Item -ItemType Directory -Force -Path $marketplace | Out-Null
Copy-Item $staging $target -Recurse -Force
Remove-Item $staging -Recurse -Force

# BuildPlugin does not copy the descriptor, it regenerates it, and the field it
# drops on the way is EnabledByDefault. Without that an engine plugin is only
# enabled in projects whose .uproject names it, which is exactly the per-project
# step this script exists to remove. Put it back.
$installedUPlugin = Join-Path $target "$PluginName.uplugin"
$descriptor = Get-Content $installedUPlugin -Raw | ConvertFrom-Json
if ($descriptor.PSObject.Properties.Name -contains 'EnabledByDefault') {
    $descriptor.EnabledByDefault = $true
} else {
    $descriptor | Add-Member -MemberType NoteProperty -Name 'EnabledByDefault' -Value $true
}
# -Depth matters: PowerShell 5.1 defaults to 2 and would flatten Modules and
# Plugins into type names, producing a descriptor the engine cannot read.
$json = $descriptor | ConvertTo-Json -Depth 20
# WriteAllText with an explicit BOM-less encoding, because Set-Content -Encoding
# UTF8 on PowerShell 5.1 emits a byte order mark. The engine tolerates one in a
# .uplugin; plenty of other JSON parsers do not, and no descriptor Epic ships
# has one.
[System.IO.File]::WriteAllText($installedUPlugin, $json,
                               (New-Object System.Text.UTF8Encoding($false)))

if (-not (Get-Content $installedUPlugin -Raw | Select-String -Quiet '"Modules"')) {
    throw "Rewriting $installedUPlugin lost the Modules section. Install aborted."
}

Write-Host "Installed to $target" -ForegroundColor Green
Write-Host ""
Write-Host "Restart the editor. Every project on this engine now has the plugin,"
Write-Host "so remove any copy of $PluginName from a project's own Plugins folder."
Write-Host "Re-run this after a C++ change or an engine patch: an installed engine"
Write-Host "cannot rebuild its engine plugins on its own."
