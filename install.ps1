# Installs or removes VinceFix (build\X3DAudio1_7.dll) in the Voodoo Vince Remastered folder.
# Takes effect on the next game launch. Removal only deletes a file that is VinceFix
# (it checks for the "VinceFix" marker), never a different X3DAudio1_7.dll.
# Install also adds VinceFix.ini (the shipped settings: FrameCap=60, PresentHold=auto, HoldFlush=auto)
# if the folder has none; an existing one is kept.
#   pwsh -File install.ps1               # install / update
#   pwsh -File install.ps1 -Uninstall    # remove (also removes VinceFix.ini and VinceFix.log)
param([switch]$Uninstall, [string]$GameDir = 'H:\SteamLibrary\steamapps\common\Voodoo Vince Remastered')
$ErrorActionPreference = 'Stop'
$target = Join-Path $GameDir 'X3DAudio1_7.dll'
$source = Join-Path $PSScriptRoot 'build\X3DAudio1_7.dll'
$settings = Join-Path $GameDir 'VinceFix.ini'
function Test-VinceFix([string]$path) {
    (Test-Path -LiteralPath $path) -and ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)) -cmatch 'VinceFix \d+\.\d+ loaded')
}
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'Vince.exe'))) { throw "Vince.exe not found in $GameDir" }
$systemCopy = Join-Path ([Environment]::GetFolderPath('SystemX86')) 'X3DAudio1_7.dll'
if (!$Uninstall -and !(Test-Path -LiteralPath $systemCopy)) { throw "$systemCopy (DirectX June 2010 runtime) is missing; VinceFix forwards audio to it. Install the DirectX runtime first." }
if ($Uninstall) {
    if (!(Test-Path -LiteralPath $target)) { 'Not installed.'; return }
    if (!(Test-VinceFix $target)) { throw "$target is not VinceFix; leaving it alone." }
    Remove-Item -LiteralPath $target
    Remove-Item -LiteralPath $settings, (Join-Path $GameDir 'VinceFix.log') -ErrorAction SilentlyContinue
    'Removed VinceFix. The game uses the system X3DAudio1_7.dll and pins its threads again from the next launch.'
    return
}
if (!(Test-VinceFix $source)) { throw "Build output missing: run build.ps1 first." }
$template = Join-Path $PSScriptRoot 'VinceFix.ini'
if (!(Test-Path -LiteralPath $settings) -and !(Test-Path -LiteralPath $template)) { throw "$template is missing; nothing was installed." }
if ((Test-Path -LiteralPath $target) -and !(Test-VinceFix $target)) { throw "$target exists and is not VinceFix; not overwriting." }
Copy-Item -LiteralPath $source -Destination $target -Force
"Installed $target ($((Get-Item -LiteralPath $target).Length) bytes, SHA256 $((Get-FileHash -LiteralPath $target).Hash.Substring(0, 16))...)."
if (Test-Path -LiteralPath $settings) { "Kept the existing $settings." }
else { Copy-Item -LiteralPath $template -Destination $settings; "Added $settings (FrameCap=60, PresentHold=auto, HoldFlush=auto; see its comments, e.g. for a fixed-refresh display)." }
'Takes effect the next time the game starts. Log: ' + (Join-Path $GameDir 'VinceFix.log')
