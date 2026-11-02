param([ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$component = if ($Architecture -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$installation = & $vswhere -version '[18.0,19.0)' -products '*' -requires $component -property installationPath | Select-Object -First 1
if (-not $installation) { throw 'Visual Studio 2026 with the requested C++ toolchain is required.' }
$hostArchitecture = if ($Architecture -eq 'arm64') { 'arm64' } else { 'amd64' }
& (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch $hostArchitecture -HostArch $hostArchitecture -SkipAutomaticLocation
if ($env:VisualStudioVersion -notlike '18.*') { throw 'Visual Studio 2026 initialization failed.' }
# Persist only compiler/SDK settings needed by later steps. GitHub displays
# GITHUB_ENV entries in step logs; unrelated process variables must stay out.
$toolchainEnvironment = @(
    'Path', 'INCLUDE', 'EXTERNAL_INCLUDE', 'LIB', 'LIBPATH',
    'VisualStudioVersion', 'VSINSTALLDIR', 'VS180COMNTOOLS', 'DevEnvDir',
    'VCINSTALLDIR', 'VCIDEInstallDir', 'VCToolsInstallDir', 'VCToolsVersion', 'VCToolsRedistDir',
    'VSCMD_ARG_HOST_ARCH', 'VSCMD_ARG_TGT_ARCH', 'VSCMD_ARG_app_plat', 'VSCMD_VER',
    'WindowsSdkDir', 'WindowsSDKVersion', 'WindowsSDKLibVersion',
    'WindowsSdkBinPath', 'WindowsSdkVerBinPath', 'WindowsLibPath',
    'UniversalCRTSdkDir', 'UCRTVersion', 'ExtensionSdkDir', 'NETFXSDKDir',
    'FrameworkDir', 'FrameworkDir64', 'FrameworkVersion', 'FrameworkVersion64',
    'Framework40Version', 'VCPKG_ROOT', 'CommandPromptType'
)
foreach ($name in $toolchainEnvironment) {
    $value = [Environment]::GetEnvironmentVariable($name, 'Process')
    if ($value -and $value -notmatch "[\r\n]") {
        "$name=$value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
    }
}
