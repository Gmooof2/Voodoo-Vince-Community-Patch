# Installs or removes VinceFix (build\X3DAudio1_7.dll from build.ps1) in the Voodoo Vince Remastered folder.
# Takes effect on the next game launch. Removal only deletes a file that is VinceFix
# (it checks for the "VinceFix" marker), never a different X3DAudio1_7.dll.
#   pwsh -File install.ps1                                   # install / update (default Steam folder)
#   pwsh -File install.ps1 -GameDir 'D:\SteamLibrary\steamapps\common\Voodoo Vince Remastered'
#   pwsh -File install.ps1 -Uninstall                        # remove (also removes VinceFix.log)
param([switch]$Uninstall, [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Voodoo Vince Remastered')
$ErrorActionPreference = 'Stop'
$target = Join-Path $GameDir 'X3DAudio1_7.dll'
$source = Join-Path $PSScriptRoot 'build\X3DAudio1_7.dll'
function Test-VinceFix([string]$path) {
    (Test-Path -LiteralPath $path) -and ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path)) -cmatch 'VinceFix \d+\.\d+ loaded')
}
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'Vince.exe'))) { throw "Vince.exe not found in $GameDir (pass -GameDir with your game folder)" }
if (Get-Process -Name Vince -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
$systemCopy = Join-Path ([Environment]::GetFolderPath('SystemX86')) 'X3DAudio1_7.dll'
if (!$Uninstall -and !(Test-Path -LiteralPath $systemCopy)) { throw "$systemCopy (DirectX June 2010 runtime) is missing; VinceFix forwards audio to it. Install the DirectX runtime first." }
if ($Uninstall) {
    if (!(Test-Path -LiteralPath $target)) { 'Not installed.'; return }
    if (!(Test-VinceFix $target)) { throw "$target is not VinceFix; leaving it alone." }
    Remove-Item -LiteralPath $target
    $log = Join-Path $GameDir 'VinceFix.log'
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log }
    'Removed VinceFix. The game uses the system X3DAudio1_7.dll again from the next launch.'
    return
}
if (!(Test-VinceFix $source)) { throw 'Build output missing: run build.ps1 first.' }
if ((Test-Path -LiteralPath $target) -and !(Test-VinceFix $target)) { throw "$target exists and is not VinceFix; not overwriting." }
Copy-Item -LiteralPath $source -Destination $target -Force
"Installed $target ($((Get-Item -LiteralPath $target).Length) bytes, SHA256 $((Get-FileHash -LiteralPath $target).Hash.Substring(0, 16))...)."
'Takes effect the next time the game starts. Log: ' + (Join-Path $GameDir 'VinceFix.log')
