param([ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$component = if ($Architecture -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$installation = & $vswhere -version '[18.0,19.0)' -products '*' -requires $component -property installationPath | Select-Object -First 1
if (-not $installation) { throw 'Visual Studio 2026 with the requested C++ toolchain is required.' }
$hostArchitecture = if ($Architecture -eq 'arm64') { 'arm64' } else { 'amd64' }
& (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch $hostArchitecture -HostArch $hostArchitecture -SkipAutomaticLocation
if ($env:VisualStudioVersion -notlike '18.*') { throw 'Visual Studio 2026 initialization failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -notmatch '^(GITHUB_|RUNNER_|ACTIONS_|CI$)' } | ForEach-Object {
    if ($_.Value -notmatch "[\r\n]") { "$($_.Name)=$($_.Value)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
}
