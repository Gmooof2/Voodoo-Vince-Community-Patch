# Builds X3DAudio1_7.dll (VinceFix) and its test hosts with the installed LLVM tools, then checks the DLL: built a second
# time in another folder it must be byte-identical (a reproducible build), and it may import only what
# imports-allowed.txt lists. No Windows SDK or MSVC needed: import libraries come from the .def files. See BUILDING.md.
# Usage: pwsh -File build.ps1 [-Test]
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$llvm = 'C:\Program Files\LLVM\bin'
$pinnedLlvm = '23.1.3' # the LLVM version the official DLL is built with; another version may give a different DLL
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
function Run([string]$tool, [string[]]$arguments) {
    & (Join-Path $llvm $tool) @arguments
    if ($LASTEXITCODE) { throw "$tool failed ($LASTEXITCODE)" }
}
$cflags = @('--target=i686-pc-windows-msvc', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe', '-Wall', '-Wextra', '-c')
# The DLL into folder $out: the import libraries from the .def files, its C files (what each does: vincefix.h) and the
# two export thunks. /Brepro makes lld-link write a hash of the contents where the PE timestamp would go, so the same
# sources and tools always give the same bytes (there is no debug info, so no paths end up in the DLL).
function Build-Dll([string]$out) {
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    foreach ($lib in 'kernel32', 'user32', 'ole32') { Run 'llvm-dlltool.exe' @('-m', 'i386', '-k', '-d', "$lib.def", '-l', "$out\$lib.lib") } # ole32: test host only
    $objects = @()
    foreach ($source in 'dllmain.c', 'common.c', 'hooks.c', 'audio.c', 'settings.c', 'present.c', 'setup_info.c') {
        $object = "$out\$([IO.Path]::GetFileNameWithoutExtension($source)).obj"
        Run 'clang.exe' ($cflags + @($source, '-o', $object))
        $objects += $object
    }
    Run 'clang.exe' @('--target=i686-pc-windows-msvc', '-c', 'thunks.s', '-o', "$out\thunks.obj")
    Run 'lld-link.exe' (@('/dll', '/nodefaultlib', '/entry:DllMain', '/machine:x86', '/subsystem:windows', '/safeseh:no', '/Brepro',
        '/def:exports.def', "/out:$out\X3DAudio1_7.dll") + $objects + @("$out\thunks.obj", "$out\kernel32.lib", "$out\user32.lib"))
}
# The DLL's imports (dll!function, from llvm-objdump -p) against imports-allowed.txt: anything else fails the build, and
# so does a listed one the DLL no longer imports (the list is published as what VinceFix may call).
function Test-Imports([string]$dll) {
    $imports = @(); $dllName = $null; $inImports = $false
    $dump = & (Join-Path $llvm 'llvm-objdump.exe') -p $dll
    if ($LASTEXITCODE) { throw "llvm-objdump could not read $dll (exit $LASTEXITCODE), so its imports were not checked" }
    foreach ($line in $dump) {
        if ($line -cmatch '^Entry d\s+[0-9a-f]+\s+([0-9a-f]+)\s+Delay Import Directory' -and $Matches[1] -notmatch '^0+$') {
            throw "$dll has delay-load imports; imports-allowed.txt does not cover them"
        }
        if ($line -cmatch '^The Import Tables:') { $inImports = $true; continue }
        if ($line -cmatch '^\S') { $inImports = $false; $dllName = $null; continue } # the next part of the dump
        if (!$inImports) { continue }
        if ($line -cmatch '^\s*DLL Name:\s*(\S+)$') { $dllName = $Matches[1].ToLowerInvariant(); continue }
        if (!$dllName -or $line -match '^\s*$' -or $line -cmatch '^\s+Hint/Ord\s+Name$' -or $line -cmatch '^\s+lookup ') { continue }
        if ($line -cmatch '^\s+\d+\s+([A-Za-z_][A-Za-z0-9_]*)$') { $imports += "$dllName!$($Matches[1])" }
        else { throw "unrecognized import line in $dll`: '$line' (an import by ordinal?)" }
    }
    if (!$imports) { throw "llvm-objdump's dump of $dll shows no import table, so its imports were not checked" }
    $allowed = @(Get-Content (Join-Path $PSScriptRoot 'imports-allowed.txt') | ForEach-Object { ($_ -replace '#.*$', '').Trim() } | Where-Object { $_ })
    $notAllowed = @($imports | Where-Object { $allowed -notcontains $_ })
    $unused = @($allowed | Where-Object { $imports -notcontains $_ })
    if ($notAllowed) { throw "X3DAudio1_7.dll imports what imports-allowed.txt does not list: $($notAllowed -join ', ')" }
    if ($unused) { throw "imports-allowed.txt lists what X3DAudio1_7.dll no longer imports (remove it): $($unused -join ', ')" }
    "Import check: the DLL imports exactly the $($imports.Count) functions imports-allowed.txt lists."
}
Push-Location $PSScriptRoot
try {
    $version = "$(& (Join-Path $llvm 'clang.exe') --version | Select-Object -First 1)"
    if ($version -notmatch "^clang version $([regex]::Escape($pinnedLlvm))( |$)") {
        Write-Warning "LLVM is '$version', but the official DLL is built with LLVM ${pinnedLlvm}: this build may not match the published SHA256."
    }
    Build-Dll $build
    Build-Dll "$build\repro-check"
    $hash = (Get-FileHash "$build\X3DAudio1_7.dll").Hash
    $again = (Get-FileHash "$build\repro-check\X3DAudio1_7.dll").Hash
    if ($hash -ne $again) { throw "not reproducible: two builds of X3DAudio1_7.dll differ ($hash, $again)" }
    "Reproducible build: built twice, both SHA256 $hash"
    Test-Imports "$build\X3DAudio1_7.dll"
    Run 'clang.exe' ($cflags + @('test.c', '-o', "$build\test.obj"))
    Run 'lld-link.exe' @('/nodefaultlib', '/entry:start', '/machine:x86', '/subsystem:console', '/safeseh:no',
        "/out:$build\vincefix-test.exe", "$build\test.obj", "$build\kernel32.lib")
    # Startup-path test: imports the DLL statically, like Vince.exe (import lib written by lld-link next to the DLL).
    Run 'clang.exe' ($cflags + @('test_static.c', '-o', "$build\test_static.obj"))
    Run 'lld-link.exe' @('/nodefaultlib', '/entry:start', '/machine:x86', '/subsystem:console', '/safeseh:no',
        "/out:$build\vincefix-test-static.exe", "$build\test_static.obj", "$build\X3DAudio1_7.lib", "$build\kernel32.lib")
    # XAudio2 cache test: imports ole32!CoCreateInstance and the DLL at load time, like Vince.exe.
    Run 'clang.exe' ($cflags + @('test_xaudio.c', '-o', "$build\test_xaudio.obj"))
    Run 'lld-link.exe' @('/nodefaultlib', '/entry:start', '/machine:x86', '/subsystem:console', '/safeseh:no',
        "/out:$build\vincefix-test-xaudio.exe", "$build\test_xaudio.obj", "$build\X3DAudio1_7.lib", "$build\ole32.lib", "$build\kernel32.lib")
    # Present-hold / frame-cap test: a fake game Present call site (byte-for-byte the game's) behind a fake swap chain (60 FPS limiter,
    # or none for the FrameCap scenarios); imports the DLL at load time.
    Run 'clang.exe' ($cflags + @('test_present.c', '-o', "$build\test_present.obj"))
    Run 'lld-link.exe' @('/nodefaultlib', '/entry:start', '/machine:x86', '/subsystem:console', '/safeseh:no',
        "/out:$build\vincefix-test-present.exe", "$build\test_present.obj", "$build\X3DAudio1_7.lib", "$build\kernel32.lib")
    Get-Item "$build\X3DAudio1_7.dll", "$build\vincefix-test.exe", "$build\vincefix-test-static.exe", "$build\vincefix-test-xaudio.exe", "$build\vincefix-test-present.exe" | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String
    if ($Test) {
        $failed = 0
        $ini = "$build\VinceFix.ini"
        # One run per ini: invalid PresentHold (with HoldFlush=1, which then has no effect), pattern absent from the exe (with an
        # invalid HoldFlush), the shipped VinceFix.ini (FrameCap=60, PresentHold=auto, HoldFlush=auto, LiveReload=0) saved as UTF-8
        # with BOM (as Notepad can), then the patched call site (the cap+auto run uses the shipped ini as it is). The present host picks its scenario from the ini it finds next to it, by the same rules
        # as the DLL: PresentHold=5 (the fixed hold), PresentHold=auto, and FrameCap=60 with a 4 ms hold / no hold / auto (the fake
        # Present has no limiter then, so only VinceFix paces the frames); HoldFlush=auto (no line) flushes only with FrameCap, so
        # cap+hold sets HoldFlush=0 for a run without; then HoldFlush=1 with the fixed hold, a failing GetDevice, no immediate
        # context, and auto. [VinceFixTest] Limiter=60 adds a second 60 FPS limiter inside the fake Present for the first half of
        # the run (FrameCap steps aside, then comes back), Limiter=gpu a "GPU-bound" Present (FrameCap must not step aside),
        # Limiter=join one that joins mid-run at another phase and catches up after late frames (FrameCap steps aside once and
        # stays aside), Limiter=edge a GPU that allows 61.5 FPS uncapped (FrameCap may step aside once, must come back for good).
        # Recheck=1 changes the display mid-run: a new swap chain on a 60 Hz display, a move to another monitor
        # at 280 Hz, a new swap chain at the old address (each must re-run the display rules).
        # [VinceFixTest] RefreshHz fakes the display's refresh rate: 60 Hz turns the hold and the cap off, 75 Hz
        # turns FrameCap=75 off (and with it HoldFlush=auto), 0 is unknown (nothing changes). LiveReload: invalid (2) in the first
        # run, 1 where the Present call can't be patched (static host), 0 in the shipped ini, missing elsewhere; the live run
        # starts with everything off and the host rewrites the ini three times while it runs.
        $runs = @(
            @{ Host = 'vincefix-test.exe';         Ini = "PresentHold=99`r`nHoldFlush=1`r`nLiveReload=2" },
            @{ Host = 'vincefix-test-static.exe';  Ini = "PresentHold=5`r`nHoldFlush=2`r`nLiveReload=1" },
            @{ Host = 'vincefix-test-xaudio.exe';  Ini = $null },
            @{ Host = 'vincefix-test-present.exe'; Ini = 'PresentHold=5';                  Mode = 'fixed' },
            @{ Host = 'vincefix-test-present.exe'; Ini = 'PresentHold=auto';               Mode = 'auto' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60`r`nHoldFlush=0`r`n[VinceFixTest]`r`nHybrid=1"; Mode = 'cap+hold' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=0`r`nFrameCap=60";    Mode = 'cap' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60`r`n[VinceFixTest]`r`nLimiter=60"; Mode = 'cap+aside' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=0`r`nFrameCap=60`r`n[VinceFixTest]`r`nLimiter=gpu"; Mode = 'cap+gpu' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60`r`n[VinceFixTest]`r`nLimiter=join"; Mode = 'cap+join' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=0`r`nFrameCap=60`r`n[VinceFixTest]`r`nLimiter=edge"; Mode = 'cap+edge' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60`r`n[VinceFixTest]`r`nRefreshHz=280`r`nRecheck=1"; Mode = 'recheck' },
            @{ Host = 'vincefix-test-present.exe'; Ini = 'the shipped VinceFix.ini (FrameCap=60, PresentHold=auto, HoldFlush=auto, LiveReload=0)'; Shipped = $true; Mode = 'cap+auto' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=5`r`nHoldFlush=1";    Mode = 'fixed+flush' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60";    Mode = 'cap+hold+flush' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=5`r`nHoldFlush=1`r`n[VinceFixTest]`r`nGetDevice=fail`r`nRefreshHz=0"; Mode = 'fixed+flush-fail' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=5`r`nHoldFlush=1`r`n[VinceFixTest]`r`nGetContext=null`r`nOutput=fail"; Mode = 'fixed+flush-nocontext' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=auto`r`nHoldFlush=1`r`n[VinceFixTest]`r`nFlushUs=800"; Mode = 'auto+flush' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=5`r`n[VinceFixTest]`r`nRefreshHz=60"; Mode = 'fixed@60Hz' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=4`r`nFrameCap=60`r`n[VinceFixTest]`r`nRefreshHz=60"; Mode = 'cap+hold@60Hz' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "PresentHold=5`r`nFrameCap=75`r`n[VinceFixTest]`r`nRefreshHz=75"; Mode = 'fixed+cap75@75Hz' },
            @{ Host = 'vincefix-test-present.exe'; Ini = "LiveReload=1`r`nPresentHold=0`r`nFrameCap=0"; Mode = 'live' })
        try {
            foreach ($run in $runs) {
                $host_ = $run.Host
                Remove-Item "$build\VinceFix.log", $ini -ErrorAction SilentlyContinue
                $shipped = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'VinceFix.ini'))
                if ($run.Shipped) { [IO.File]::WriteAllText($ini, $shipped) } # as shipped (ASCII, no BOM)
                elseif ($run.Ini) { [IO.File]::WriteAllText($ini, "[VinceFix]`r`n$($run.Ini)`r`n") }
                else { [IO.File]::WriteAllText($ini, $shipped, [Text.UTF8Encoding]::new($true)) }
                if ($run.Mode) { "=== $host_ [$($run.Mode): $($run.Ini -replace "`r`n", ', ')]" } else { "=== $host_" }
                & "$build\$host_" | Tee-Object -Variable hostOut
                $failed += $LASTEXITCODE
                "--- VinceFix.log:"; $log = @(Get-Content "$build\VinceFix.log" -ErrorAction SilentlyContinue); $log
                $count = { param($text) @($log | Where-Object { $_.Contains($text) }).Count }
                $countBoth = { param($a, $b) @($log | Where-Object { $_.Contains($a) -and $_.Contains($b) }).Count }
                $firstLine = { param($text) for ($i = 0; $i -lt $log.Count; $i++) { if ($log[$i].Contains($text)) { return $i } }; -1 }
                $logChecks = [ordered]@{}
                if ($host_ -eq 'vincefix-test.exe') {
                    $logChecks['log: PresentHold=99 rejected (not a number from 1 to 12)'] = (& $count 'not a number from 1 to 12') -ge 1
                    $logChecks['log: HoldFlush=1 with PresentHold off -> "has no effect while PresentHold is off" once'] = (& $count 'hold flush: HoldFlush=1 has no effect while PresentHold is off') -eq 1 -and (& $count 'hold flush: on') -eq 0
                    $logChecks['log: LiveReload=2 rejected ("is not 0 or 1; off") once, nothing patched'] = (& $count 'live reload: LiveReload=2 in VinceFix.ini is not 0 or 1; off') -eq 1 -and (& $count 'live reload:') -eq 1 -and (& $count 'patched Present call') -eq 0
                }
                if ($host_ -eq 'vincefix-test-static.exe') {
                    $logChecks['log: PresentHold=5 but no Present call in the exe (not found)'] = (& $count 'Present call was not found') -ge 1
                    $logChecks['log: HoldFlush=2 rejected ("is not 0, 1 or auto; using auto") once, no "hold flush: on"'] = (& $count 'hold flush: HoldFlush=2 in VinceFix.ini is not 0, 1 or auto; using auto') -eq 1 -and (& $count 'hold flush: on') -eq 0
                    $logChecks['log: LiveReload=1 without a patchable Present call -> "live reload: off (the Present call could not be patched)" once, no watcher'] = (& $count 'live reload: off (the Present call could not be patched)') -eq 1 -and (& $count 'live reload:') -eq 1
                }
                if ($host_ -eq 'vincefix-test-xaudio.exe') {
                    # This host has no Present call to patch, so the hold and the cap stop there; a value that is missing, 0 or
                    # invalid would have its own "off" or "is not" line.
                    $logChecks = [ordered]@{
                        'log: shipped VinceFix.ini with a BOM is read (PresentHold=auto: "present hold: the game''s Present call was not found" once)' = (& $count 'present hold: the game''s Present call was not found') -eq 1 -and (& $count 'present hold:') -eq 1
                        'log: shipped VinceFix.ini with a BOM is read (FrameCap=60 accepted: no "frame cap:" line)' = (& $count 'frame cap:') -eq 0
                        'log: shipped VinceFix.ini: HoldFlush=auto accepted, nothing to say without a patched Present (no "hold flush:" line)' = (& $count 'hold flush:') -eq 0
                        'log: shipped VinceFix.ini: LiveReload=0 -> "live reload: off (LiveReload=0 or empty)" once' = (& $count 'live reload: off (LiveReload=0 or empty)') -eq 1 -and (& $count 'live reload:') -eq 1
                        'log: CoCreateInstance import hooked once'                 = (& $count 'import CoCreateInstance at') -eq 1
                        'log: XAudio2 vtable patched once (engines B and C share it)' = (& $count 'slot 4 (GetDeviceDetails') -eq 1
                        'log: 2-9 refreshes (B first call; C after its creation, racing threads)' = ($r = & $count 'queried XAudio2') -ge 2 -and $r -le 9
                        'log: no failures or skipped patches'                      = (& $count 'FAILED') + (& $count 'uncached') + (& $count 'VirtualProtect') -eq 0
                    }
                }
                if ($host_ -ne 'vincefix-test-present.exe') {
                    $logChecks['log: Present not patched, so no setup report (gpu:, display:, power:, os:)'] = (& $count '  gpu: ') + (& $count '  display: ') + (& $count '  power: ') + (& $count '  os: ') -eq 0
                }
                if ($host_ -eq 'vincefix-test-present.exe') {
                    $clean =(& $count 'WARNING') + (& $count 'not found') + (& $count 'VirtualProtect') + (& $count 'FAILED') -eq 0
                    $capLine = 'frame cap: 60 FPS (frame starts 16.66 ms apart)'
                    $capOn = (& $count 'frame cap: the game''s display runs at') -eq 1 -and (& $count '; cap on') -eq 1
                    $capFirst = (& $count $capLine) -eq 1 -and (& $firstLine $capLine) -ge 0 -and (& $firstLine $capLine) -lt (& $firstLine 'patched Present call at +0x')
                    # HoldFlush: auto (no line) without FrameCap logs one "off (HoldFlush=auto flushes only with FrameCap" line
                    # and no flush figures; with FrameCap, or HoldFlush=1, one "on (" line after the patch line. Then the first
                    # stats line's held and flushed counts must equal the Flush calls the host saw in frames 0-599, the 6
                    # long-Flush frames among them must show up as "ran past the hold" (a few more if Windows paused the host),
                    # and the host's 3.5 ms fake Flush must be what the DLL measured as the longest.
                    $flushLines = & $count 'hold flush:'
                    $noFlushStats = (& $count ' flushed (') -eq 0
                    $autoNoCap = (& $count 'hold flush: off (HoldFlush=auto flushes only with FrameCap') -eq 1 -and $flushLines -eq 1 -and $noFlushStats
                    $flushOn = (& $count 'hold flush: on (') -eq 1 -and (& $firstLine 'hold flush: on (') -gt (& $firstLine 'patched Present call at +0x')
                    $autoOn = $flushOn -and (& $count 'hold flush: on (HoldFlush=auto and FrameCap is on') -eq 1
                    $oneOn = $flushOn -and (& $count 'hold flush: on (HoldFlush=1:') -eq 1
                    $holdOff60 = (& $count 'present hold: the game''s display runs at 60 Hz; at 61 Hz or less frames are shown on its refresh ticks anyway, so the hold would only add latency; hold off') -eq 1
                    $stats0 = "$(@($log | Where-Object { $_.Contains('present hold stats:') }) | Select-Object -First 1)"
                    $hostLine = "$(@($hostOut | Where-Object { "$_" -match 'flush: frames 0-599: \d+ Flush calls' }) | Select-Object -First 1)"
                    $num = { param($text, $pattern) if ($text -match $pattern) { [double]$Matches[1] } else { -1 } }
                    $held = & $num $stats0 ' (\d+) held,'
                    $flushed = & $num $stats0 ' (\d+) flushed \('
                    $hostFlushes = & $num $hostLine 'frames 0-599: (\d+) Flush calls'
                    $past = & $num $stats0 '; (\d+) ran past the hold'
                    $flushAvg = & $num $stats0 'Flush total [\d.]+ ms, avg ([\d.]+) us'
                    $flushMax = & $num $stats0 'Flush total [\d.]+ ms, avg [\d.]+ us, max ([\d.]+) us'
                    $counted = $held -gt 0 -and $held -eq $flushed -and $flushed -eq $hostFlushes
                    # Time inside Present: with the fake 60 FPS limiter and a 5 ms hold the limiter waits ~11.4 ms per frame on
                    # average (11.7 ms after a held frame, 9.7 after a 7 ms one); with FrameCap and no limiter, about nothing.
                    $insideAvg = & $num $stats0 '; time inside Present: avg ([\d.]+) ms, max [\d.]+ ms \(last 600 frames\)'
                    $insideMax = & $num $stats0 '; time inside Present: avg [\d.]+ ms, max ([\d.]+) ms \(last 600 frames\)'
                    $timed = $past -ge 6 -and $past -le 18 -and $flushMax -ge 3000 -and $flushMax -le 5000 -and $flushAvg -ge 150
                    if ($run.Mode -like '*flush*') { "--- parsed: held $held, flushed $flushed, host saw $hostFlushes Flush calls in frames 0-599, ran past the hold $past, Flush avg $flushAvg us, max $flushMax us" }
                    # Latency figures at the end of every stats line, after the unchanged "(last N frames)": frame start to real
                    # Present always, the hold's wait when a hold runs (the line then has " held, "), FrameCap's wait when it runs
                    # (", frame cap "). The first line's values are checked per run below.
                    $tailRe = '; time inside Present: avg [\d.]+ ms, max [\d.]+ ms \(last \d+ frames\); frame start to real Present avg (?<start>[\d.]+) ms, p99 (?<startp>[\d.]+|over 40) ms(?<hold>; hold wait avg (?<holdavg>[\d.]+) ms, p99 (?:[\d.]+|over 40) ms \((?<held>\d+) held frames\))?(?<cap>; cap wait avg (?<capavg>[\d.]+) ms over (?<capn>\d+) frames)?$'
                    $statsLines = @($log | Where-Object { $_.Contains('present hold stats:') })
                    $tailsOk = $true
                    foreach ($s in $statsLines) {
                        if ($s -notmatch $tailRe -or [bool]$Matches['hold'] -ne $s.Contains(' held, ') -or [bool]$Matches['cap'] -ne $s.Contains(', frame cap ')) { $tailsOk = $false }
                    }
                    $lat = @{}; if ($stats0 -match $tailRe) { $lat = @{} + $Matches }
                    $latNum = { param($key) if ($lat[$key]) { [double]$lat[$key] } else { -1 } }
                    $logChecks['log: every stats line ends with the latency figures (frame start to real Present; hold wait with a hold; cap wait with FrameCap)'] = $tailsOk
                    if ($statsLines.Count) { "--- parsed latency: start->Present avg $($lat['start']) p99 $($lat['startp']); hold wait avg $($lat['holdavg']) ($($lat['held']) held); cap wait avg $($lat['capavg']) over $($lat['capn']) frames" }
                    # FrameCap steps aside only in the cap+aside run (one line each way) and the cap+join run (once, no way
                    # back); the old once-only NOTE is gone.
                    $asideLine = 'frame cap: another frame limiter (a driver or RTSS cap, or VSync) is waiting inside Present, so VinceFix''s cap steps aside; it comes back if that limiter stops'
                    $backLine = 'frame cap: no other limiter is pacing the game any more; VinceFix''s cap is back on'
                    $fasterLine = 'frame cap: without VinceFix''s cap the game ran faster than the cap, so what waits inside Present is not a frame cap (a GPU just fast enough, say); VinceFix''s cap is back on and stays on'
                    if ($run.Mode -notin 'cap+aside', 'cap+join', 'cap+edge') {
                        $logChecks['log: FrameCap never steps aside (no step-aside or back-on line), no "frame cap: NOTE"'] = (& $count $asideLine) + (& $count $backLine) + (& $count $fasterLine) + (& $count 'frame cap: NOTE') -eq 0
                    }
                    # Setup report (first frame): exactly one gpu:, display:, power: and os: line each, with the fake adapter's
                    # values, the fake swap chain's description, the output's GPU, and this PC's Windows build.
                    $iniText = "$($run.Ini)"
                    $gpuWant = if ($iniText -match 'GetDevice=fail') { 'gpu: unknown (the swap chain''s GetDevice(ID3D11Device) failed with 0x80004002)' }
                               else { "gpu: `"VinceFix Test GPU`" (vendor $(if ($iniText -match 'Hybrid=1') { '0x1002 AMD' } else { '0x10DE NVIDIA' }), device 0x2684), 1536 MB dedicated video memory, driver 31.0.15.5222" }
                    $refreshWant = if ($iniText -match 'RefreshHz=(\d+)') { if ([int]$Matches[1] -gt 1) { "$($Matches[1]) Hz" } else { 'refresh rate unknown' } } else { '\d+ Hz' }
                    $outputWant = if ($iniText -match 'Output=fail') { 'output''s GPU unknown \(IDXGISwapChain::GetContainingOutput failed with 0x887A0002\)' }
                                  elseif ($iniText -match 'GetDevice=fail') { 'output on "VinceFix Test GPU" \(render GPU unknown\)' }
                                  elseif ($iniText -match 'Hybrid=1') { 'output on a different GPU \(hybrid\): "VinceFix Test iGPU"' }
                                  else { 'output on the same GPU as rendering' }
                    $displayRe = "  display: $refreshWant; swap chain: discard \(blt model\), 2 buffers of 1920x1080, windowed, flags 0x2; $outputWant$"
                    $build_ = [Environment]::OSVersion.Version.Build
                    $osWant = "  os: Windows 10.0 build $build_ (Windows $(if ($build_ -ge 22000) { '11' } else { '10' }))"
                    $swapLine = 'display: the game''s swap chain changed; display rules checked again'
                    $moveLine = 'display: the game window moved to another monitor; display rules checked again'
                    if ($run.Mode -ne 'recheck') { # the recheck run expects more of them (below)
                        $logChecks['log: the display never changed, so it was never checked again (no "display rules checked again" line)'] = (& $count 'display rules checked again') -eq 0
                        $logChecks["log: setup report: one gpu: line, $(if ($iniText -match 'GetDevice=fail') { 'GetDevice failure named' } else { 'with the fake adapter''s values' })"] = (& $count '  gpu: ') -eq 1 -and (& $count "  $gpuWant") -eq 1
                        $logChecks['log: setup report: one display: line (refresh, the fake swap chain, the output''s GPU)'] = (& $count '  display: ') -eq 1 -and @($log | Where-Object { $_ -match $displayRe }).Count -eq 1
                    }
                    $logChecks['log: setup report: one power: line'] = (& $count '  power: ') -eq 1 -and @($log | Where-Object { $_ -match '  power: (on AC power( \(no battery\))?|on battery( \(\d+ %\))?|power source unknown), battery saver (on|off)$' }).Count -eq 1
                    $logChecks['log: setup report: one os: line with this PC''s Windows build'] = (& $count '  os: ') -eq 1 -and (& $count $osWant) -eq 1
                    if ($run.Shipped) {
                        $logChecks['log: shipped ini: LiveReload=0 -> "live reload: off (LiveReload=0 or empty)" once, no "settings reloaded:" line'] = (& $count 'live reload: off (LiveReload=0 or empty)') -eq 1 -and (& $count 'live reload:') -eq 1 -and (& $count 'settings reloaded:') -eq 0
                    } elseif ($run.Mode -ne 'live') {
                        $logChecks['log: no LiveReload line -> "live reload: off (no LiveReload line)" once, no "settings reloaded:" line'] = (& $count 'live reload: off (no LiveReload line)') -eq 1 -and (& $count 'live reload:') -eq 1 -and (& $count 'settings reloaded:') -eq 0
                    }
                    switch ($run.Mode) {
                        'fixed' {
                            $logChecks['log: present call patched exactly once']                          = (& $count 'patched Present call at +0x') -eq 1
                            $logChecks['log: patch line "present hold: 5.00 ms; patched Present call at +0x"'] = (& $count 'present hold: 5.00 ms; patched Present call at +0x') -eq 1
                            $logChecks['log: no FrameCap line -> "frame cap: off (no FrameCap line)"']    = (& $count 'frame cap: off (no FrameCap line)') -eq 1
                            $logChecks['log: present hold stats written once (600 frames), no frame left Present within 1 ms'] = (& $count 'present hold stats:') -eq 1 -and (& $count ' 0 held ones left Present within 1 ms') -eq 1
                            $logChecks['log: no missing-cap WARNING (the fake limiter waits inside Present)'] = (& $count 'WARNING') -eq 0
                            $logChecks['log: stats line "time inside Present": avg 10-12.5 ms, max <= 17 ms over the last 600 frames (the fake limiter''s wait)'] = $insideAvg -ge 10 -and $insideAvg -le 12.5 -and $insideMax -le 17
                            $logChecks['log: no not-found, VirtualProtect or FAILED lines']               = (& $count 'not found') + (& $count 'VirtualProtect') + (& $count 'FAILED') -eq 0
                            $logChecks['log: no HoldFlush line, no FrameCap -> "hold flush: off (HoldFlush=auto flushes only with FrameCap" only, no flush figures'] = $autoNoCap
                            # Frames of 1-4.6 ms work present 5 ms after their start, the 7 ms ones at once: avg (7 x 5 + 7) / 8 = 5.25;
                            # the hold waits 5 ms minus the work: avg 2.34 ms over the 7 held frames of each 8.
                            $logChecks['log: latency: frame start to real Present avg 5.0-5.6 ms, hold wait avg 2.0-2.8 ms over 500-530 held frames'] = (& $latNum 'start') -ge 5.0 -and (& $latNum 'start') -le 5.6 -and (& $latNum 'holdavg') -ge 2.0 -and (& $latNum 'holdavg') -le 2.8 -and (& $latNum 'held') -ge 500 -and (& $latNum 'held') -le 530
                        }
                        'auto' {
                            $logChecks['log: auto: patch line "present hold: auto; patched Present call at +0x" exactly once'] = (& $count 'present hold: auto; patched Present call at +0x') -eq 1
                            $logChecks['log: auto: no FrameCap line -> "frame cap: off (no FrameCap line)"'] = (& $count 'frame cap: off (no FrameCap line)') -eq 1
                            $logChecks['log: auto: at least 3 "present hold auto: target" lines']         = (& $count 'present hold auto: target') -ge 3
                            $logChecks['log: auto: "no frame-rate cap detected" exactly once (phase C)']   = (& $count 'no frame-rate cap detected') -eq 1
                            $logChecks['log: auto: "present hold auto: frame-rate cap detected" exactly once (phase D)'] = (& $count 'present hold auto: frame-rate cap detected') -eq 1
                            $logChecks['log: auto: stats line has "hold now"']                              = (& $countBoth 'present hold stats:' 'hold now') -ge 1
                            $logChecks['log: auto: no WARNING, not found, VirtualProtect or FAILED']       = $clean
                            $logChecks['log: auto: no HoldFlush line, no FrameCap -> "hold flush: off (HoldFlush=auto flushes only with FrameCap" only, no flush figures'] = $autoNoCap
                        }
                        'cap+hold' {
                            $logChecks['log: cap+hold: "frame cap: 60 FPS (frame starts 16.66 ms apart)" once, before the patch line'] = $capFirst
                            $logChecks['log: cap+hold: patch line "present hold: 4.00 ms; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count 'present hold: 4.00 ms; patched Present call at +0x') -eq 1
                            $logChecks['log: cap+hold: display check "frame cap: the game''s display runs at ... Hz; cap on" once'] = $capOn
                            $logChecks['log: cap+hold: stats line has ", frame cap 60 FPS"']             = (& $countBoth 'present hold stats:' ', frame cap 60 FPS') -eq 1
                            $logChecks['log: cap+hold: no WARNING, not found, VirtualProtect or FAILED']  = $clean
                            $logChecks['log: cap+hold: HoldFlush=0 -> "hold flush: off (HoldFlush=0 or empty)" only, no flush figures'] = (& $count 'hold flush: off (HoldFlush=0 or empty)') -eq 1 -and $flushLines -eq 1 -and $noFlushStats
                            # Every frame presents 4 ms after its start; FrameCap waits the rest of the 16.67 ms (minus Present) in all 599 frames.
                            $logChecks['log: cap+hold: latency: frame start to real Present avg 3.9-4.3 ms, cap wait avg 12-13 ms over 599 frames'] = (& $latNum 'start') -ge 3.9 -and (& $latNum 'start') -le 4.3 -and (& $latNum 'capavg') -ge 12 -and (& $latNum 'capavg') -le 13 -and (& $latNum 'capn') -eq 599
                        }
                        'cap' {
                            $logChecks['log: cap: patch line "frame cap: 60 FPS; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count 'frame cap: 60 FPS; patched Present call at +0x') -eq 1
                            $logChecks['log: cap: hold off (PresentHold=0) and no "frame starts ... ms apart" line'] = (& $count 'present hold: off (PresentHold=0)') -eq 1 -and (& $count 'ms apart') -eq 0
                            $logChecks['log: cap: display check "frame cap: the game''s display runs at ... Hz; cap on" once'] = $capOn
                            $logChecks['log: cap: stats line has ", frame cap 60 FPS"']                  = (& $countBoth 'present hold stats:' ', frame cap 60 FPS') -eq 1
                            $logChecks['log: cap: stats line "time inside Present": avg < 0.2 ms over the last 600 frames (nothing waits inside it)'] = $insideAvg -ge 0 -and $insideAvg -lt 0.2
                            $logChecks['log: cap: no WARNING, not found, VirtualProtect or FAILED']      = $clean
                            $logChecks['log: cap: no hold, so no "hold flush:" line and no flush figures'] = $flushLines -eq 0 -and $noFlushStats
                            # Work 1-3.4 ms (avg 2.1) plus five 25 ms frames; FrameCap waits the rest of each 16.67 ms (none after a 25 ms frame).
                            $logChecks['log: cap: latency: frame start to real Present avg 1.9-2.7 ms, cap wait avg 13.5-15 ms over 599 frames'] = (& $latNum 'start') -ge 1.9 -and (& $latNum 'start') -le 2.7 -and (& $latNum 'capavg') -ge 13.5 -and (& $latNum 'capavg') -le 15 -and (& $latNum 'capn') -eq 599
                        }
                        'cap+aside' {
                            # The host checked the timing; here the two lines and the stats line. FrameCap ran the first window only
                            # (stats count frames 1-599: the cap wait figure covers just the frames before it stepped aside), and
                            # waited nothing there (late behind the slightly slower fake limiter).
                            $logChecks['log: cap+aside: one step-aside line (HoldFlush=auto stops flushing), then one back-on line (flushes again)'] = (& $count $asideLine) -eq 1 -and (& $count "$asideLine (HoldFlush=auto stops flushing meanwhile)") -eq 1 -and (& $count $backLine) -eq 1 -and (& $count "$backLine (HoldFlush=auto flushes again)") -eq 1 -and (& $firstLine $asideLine) -lt (& $firstLine $backLine)
                            $logChecks['log: cap+aside: no "frame cap: NOTE", WARNING, not found, VirtualProtect or FAILED'] = $clean -and (& $count 'frame cap: NOTE') -eq 0
                            $logChecks['log: cap+aside: stats: cap wait avg < 0.2 ms over 30-120 frames (FrameCap ran until it stepped aside), frame start to real Present avg 3.9-4.3 ms'] = (& $latNum 'capavg') -ge 0 -and (& $latNum 'capavg') -lt 0.2 -and (& $latNum 'capn') -ge 30 -and (& $latNum 'capn') -le 120 -and (& $latNum 'start') -ge 3.9 -and (& $latNum 'start') -le 4.3
                            $logChecks['log: cap+aside: stats: hold wait avg 1.5-2.5 ms (4 ms minus 1-3.4 ms of work and the Flush)'] = (& $latNum 'holdavg') -ge 1.5 -and (& $latNum 'holdavg') -le 2.5
                        }
                        'cap+gpu' {
                            $logChecks['log: cap+gpu: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                            $logChecks['log: cap+gpu: FrameCap ran all 599 frames of the first stats line (it never stepped aside), waiting about nothing behind the 58 FPS Present (avg < 0.5 ms)'] = (& $latNum 'capn') -eq 599 -and (& $latNum 'capavg') -ge 0 -and (& $latNum 'capavg') -lt 0.5
                        }
                        'cap+join' {
                            # The host checked the switch's timing and that no Flush came back; here the lines: one step-aside
                            # line after the display check, and FrameCap never back on (the late frames' catch-ups don't count
                            # as a faster game).
                            $logChecks['log: cap+join: one step-aside line (HoldFlush=auto stops flushing), after the display check; no back-on line'] = (& $count $asideLine) -eq 1 -and (& $count "$asideLine (HoldFlush=auto stops flushing meanwhile)") -eq 1 -and (& $count $backLine) -eq 0 -and (& $count $fasterLine) -eq 0 -and (& $firstLine $asideLine) -gt (& $firstLine '; cap on')
                            $logChecks['log: cap+join: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'cap+edge' {
                            # A GPU just faster than the cap: at most one step-aside, then back for good with the "ran faster" line.
                            $logChecks['log: cap+edge: at most one step-aside line, then exactly one "ran faster than the cap ... back on and stays on" line after it; no other back-on line'] = (& $count $asideLine) -le 1 -and (& $count $fasterLine) -eq (& $count $asideLine) -and (& $count $backLine) -eq 0 -and ((& $count $asideLine) -eq 0 -or (& $firstLine $asideLine) -lt (& $firstLine $fasterLine))
                            $logChecks['log: cap+edge: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'recheck' {
                            # Frame 0: the full report at 280 Hz. Frame 300 (new swap chain, 60 Hz): why, the display: line, the
                            # cap and the hold off. Frame 660 (window on the other monitor, 280 Hz): why, display:, cap on. Frame
                            # 960 (new swap chain at the old address, 2560x1440, another adapter): why, gpu: again, display:, cap on.
                            $shown = @($log | Where-Object { $_ -match '  (display|gpu|frame cap|present hold): ' -and $_ -notmatch 'patched Present call|ms apart' } |
                                ForEach-Object { $_ -replace '^.*?  pid \d+  ', '' }) # without the time and pid
                            $want = @(
                                "gpu: `"VinceFix Test GPU`" (vendor 0x10DE NVIDIA, device 0x2684), 1536 MB dedicated video memory, driver 31.0.15.5222",
                                'display: 280 Hz; swap chain: discard (blt model), 2 buffers of 1920x1080, windowed, flags 0x2; output on the same GPU as rendering',
                                'frame cap: the game''s display runs at 280 Hz; cap on',
                                $swapLine,
                                'display: 60 Hz; swap chain: discard (blt model), 2 buffers of 1920x1080, windowed, flags 0x2; output on the same GPU as rendering',
                                'frame cap: the game''s display runs at 60 Hz, so VSync already limits the game to about that; cap off',
                                'present hold: the game''s display runs at 60 Hz; at 61 Hz or less frames are shown on its refresh ticks anyway, so the hold would only add latency; hold off',
                                $moveLine,
                                'display: 280 Hz; swap chain: discard (blt model), 2 buffers of 1920x1080, windowed, flags 0x2; output on the same GPU as rendering',
                                'frame cap: the game''s display runs at 280 Hz; cap on',
                                $swapLine,
                                "gpu: `"VinceFix Test GPU`" (vendor 0x10DE NVIDIA, device 0x2684), 1536 MB dedicated video memory, driver 31.0.15.5222",
                                'display: 280 Hz; swap chain: discard (blt model), 2 buffers of 2560x1440, windowed, flags 0x2; output on the same GPU as rendering',
                                'frame cap: the game''s display runs at 280 Hz; cap on')
                            if (($shown -join "`n") -cne ($want -join "`n")) { '--- recheck: display lines found:'; $shown }
                            $logChecks['log: recheck: in order: the first report (gpu:, display: 280 Hz, cap on); swap chain changed, display: 60 Hz, cap off, hold off; moved to another monitor, display: 280 Hz, cap on; swap chain changed, gpu: (another adapter), display: 2560x1440, cap on'] = ($shown -join "`n") -ceq ($want -join "`n")
                            $logChecks['log: recheck: power: and os: only once (at the first frame)'] = (& $count '  power: ') -eq 1 -and (& $count '  os: ') -eq 1
                            $logChecks['log: recheck: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'cap+auto' {
                            $logChecks['log: cap+auto: patch line "present hold: auto; patched Present call at +0x" exactly once'] = (& $count 'present hold: auto; patched Present call at +0x') -eq 1
                            $logChecks['log: cap+auto: "frame cap: 60 FPS (frame starts 16.66 ms apart)" once, before the patch line'] = $capFirst
                            $logChecks['log: cap+auto: display check "frame cap: the game''s display runs at ... Hz; cap on" once'] = $capOn
                            $logChecks['log: cap+auto: at least 3 "present hold auto: target" lines']     = (& $count 'present hold auto: target') -ge 3
                            $logChecks['log: cap+auto: no "frame-rate cap detected" line of either kind (every window counts as capped)'] = (& $count 'frame-rate cap detected') -eq 0
                            $logChecks['log: cap+auto: stats line has "hold now" and ", frame cap 60 FPS"'] = (& $countBoth 'present hold stats:' 'hold now') -ge 1 -and (& $countBoth 'present hold stats:' ', frame cap 60 FPS') -ge 1
                            $logChecks['log: cap+auto: no WARNING, not found, VirtualProtect or FAILED']  = $clean
                            $logChecks['log: cap+auto: no HoldFlush line, FrameCap on -> "hold flush: on (HoldFlush=auto and FrameCap is on" once, after the patch line'] = $autoOn
                            $logChecks['log: cap+auto: stats held = flushed = the Flush calls the host saw in frames 0-599'] = $counted
                        }
                        'fixed+flush' {
                            $logChecks['log: fixed+flush: patch line "present hold: 5.00 ms; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count 'present hold: 5.00 ms; patched Present call at +0x') -eq 1
                            $logChecks['log: fixed+flush: "hold flush: on (HoldFlush=1:" once, after the patch line'] = $oneOn
                            $logChecks['log: fixed+flush: stats written once (600 frames), no frame left Present within 1 ms'] = (& $count 'present hold stats:') -eq 1 -and (& $count ' 0 held ones left Present within 1 ms') -eq 1
                            $logChecks['log: fixed+flush: stats held = flushed = the Flush calls the host saw in frames 0-599'] = $counted
                            $logChecks['log: fixed+flush: 6-18 ran past the hold (the 6 long Flushes), Flush max 3-5 ms (the 3.5 ms one), avg >= 150 us'] = $timed
                            $logChecks['log: fixed+flush: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'cap+hold+flush' {
                            $logChecks['log: cap+hold+flush: "frame cap: 60 FPS (frame starts 16.66 ms apart)" once, before the patch line'] = $capFirst
                            $logChecks['log: cap+hold+flush: patch line "present hold: 4.00 ms; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count 'present hold: 4.00 ms; patched Present call at +0x') -eq 1
                            $logChecks['log: cap+hold+flush: no HoldFlush line, FrameCap on -> "hold flush: on (HoldFlush=auto and FrameCap is on" once, after the patch line'] = $autoOn
                            $logChecks['log: cap+hold+flush: display check "frame cap: the game''s display runs at ... Hz; cap on" once'] = $capOn
                            $logChecks['log: cap+hold+flush: stats line has ", frame cap 60 FPS"'] = (& $countBoth 'present hold stats:' ', frame cap 60 FPS') -eq 1
                            $logChecks['log: cap+hold+flush: stats held = flushed = the Flush calls the host saw in frames 0-599'] = $counted
                            $logChecks['log: cap+hold+flush: 6-18 ran past the hold (the 6 long Flushes), Flush max 3-5 ms (the 3.5 ms one), avg >= 150 us'] = $timed
                            $logChecks['log: cap+hold+flush: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'fixed+flush-fail' {
                            $logChecks['log: fixed+flush-fail: "hold flush: on (HoldFlush=1:" once, after the patch line'] = $oneOn
                            $logChecks['log: fixed+flush-fail: refresh 0 (unknown) -> "present hold: the game''s display refresh rate is unknown; hold left on as set" once'] = (& $count 'present hold: the game''s display refresh rate is unknown; hold left on as set') -eq 1
                            $logChecks['log: fixed+flush-fail: "GetDevice(ID3D11Device) failed with 0x80004002; HoldFlush off for this session" once'] = (& $count 'hold flush: the swap chain''s GetDevice(ID3D11Device) failed with 0x80004002; HoldFlush off for this session') -eq 1
                            $logChecks['log: fixed+flush-fail: stats line has " 0 flushed (" and "stopped: no D3D11 device"'] = (& $countBoth 'present hold stats:' ' 0 flushed (') -eq 1 -and (& $countBoth 'present hold stats:' 'stopped: no D3D11 device') -eq 1
                            $logChecks['log: fixed+flush-fail: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'fixed+flush-nocontext' {
                            $logChecks['log: fixed+flush-nocontext: "hold flush: on (HoldFlush=1:" once, after the patch line'] = $oneOn
                            $logChecks['log: fixed+flush-nocontext: "GetImmediateContext gave no context; HoldFlush off for this session" once'] = (& $count 'hold flush: ID3D11Device::GetImmediateContext gave no context; HoldFlush off for this session') -eq 1
                            $logChecks['log: fixed+flush-nocontext: stats line has " 0 flushed (" and "stopped: no D3D11 immediate context"'] = (& $countBoth 'present hold stats:' ' 0 flushed (') -eq 1 -and (& $countBoth 'present hold stats:' 'stopped: no D3D11 immediate context') -eq 1
                            $logChecks['log: fixed+flush-nocontext: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'auto+flush' {
                            $logChecks['log: auto+flush: patch line "present hold: auto; patched Present call at +0x" exactly once'] = (& $count 'present hold: auto; patched Present call at +0x') -eq 1
                            $logChecks['log: auto+flush: "hold flush: on (HoldFlush=1:" once, after the patch line'] = $oneOn
                            $logChecks['log: auto+flush: at least 2 "present hold auto: target" lines, no "frame-rate cap detected" line'] = (& $count 'present hold auto: target') -ge 2 -and (& $count 'frame-rate cap detected') -eq 0
                            $logChecks['log: auto+flush: stats held = flushed = the Flush calls the host saw in frames 0-599'] = $counted
                            $logChecks['log: auto+flush: at least 2 ran past the hold (the 3.5 ms Flushes), Flush max 3-5 ms, avg >= 600 us (the 0.8 ms Flush)'] = $past -ge 2 -and $flushMax -ge 3000 -and $flushMax -le 5000 -and $flushAvg -ge 600
                            $logChecks['log: auto+flush: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'fixed@60Hz' {
                            $logChecks['log: fixed@60Hz: patch line "present hold: 5.00 ms; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count 'present hold: 5.00 ms; patched Present call at +0x') -eq 1
                            $logChecks['log: fixed@60Hz: "present hold: the game''s display runs at 60 Hz; ... hold off" once'] = $holdOff60
                            $logChecks['log: fixed@60Hz: nothing runs, so no stats line'] = (& $count 'present hold stats:') -eq 0
                            $logChecks['log: fixed@60Hz: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'cap+hold@60Hz' {
                            $logChecks['log: cap+hold@60Hz: "frame cap: the game''s display runs at 60 Hz, so VSync already limits the game to about that; cap off" once'] = (& $count 'frame cap: the game''s display runs at 60 Hz, so VSync already limits the game to about that; cap off') -eq 1 -and (& $count '; cap on') -eq 0
                            $logChecks['log: cap+hold@60Hz: "present hold: the game''s display runs at 60 Hz; ... hold off" once'] = $holdOff60
                            $logChecks['log: cap+hold@60Hz: nothing runs, so no stats line'] = (& $count 'present hold stats:') -eq 0
                            $logChecks['log: cap+hold@60Hz: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'fixed+cap75@75Hz' {
                            $logChecks['log: fixed+cap75@75Hz: "frame cap: the game''s display runs at 75 Hz, so VSync ...; cap off" once, no hold-off line'] = (& $count 'frame cap: the game''s display runs at 75 Hz, so VSync already limits the game to about that; cap off') -eq 1 -and (& $count 'present hold: the game''s display') -eq 0
                            $logChecks['log: fixed+cap75@75Hz: "hold flush: on (HoldFlush=auto and FrameCap is on" at load, then "hold flush: off (FrameCap turned itself off at this display" once'] = $autoOn -and (& $count 'hold flush: off (FrameCap turned itself off at this display, and HoldFlush=auto flushes only with FrameCap)') -eq 1 -and (& $firstLine 'hold flush: off (FrameCap turned') -gt (& $firstLine 'hold flush: on (')
                            $logChecks['log: fixed+cap75@75Hz: the hold runs alone: one stats line, no flush figures, no ", frame cap"'] = (& $count 'present hold stats:') -eq 1 -and $noFlushStats -and (& $countBoth 'present hold stats:' ', frame cap') -eq 0
                            $logChecks['log: fixed+cap75@75Hz: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                        'live' {
                            # Load: everything off, patched for live reload, watcher on. Then the host's three rewrites: each logs the
                            # settings as read (as at load), the HoldFlush decision, the display check when FrameCap is asked, and one
                            # "settings reloaded:" line describing what runs. The fourth save changes nothing: only its summary line.
                            $reloads = @($log | Where-Object { $_.Contains('settings reloaded: ') } | ForEach-Object { $_.Substring($_.IndexOf('settings reloaded: ')) })
                            $reloadsWant = @(
                                'settings reloaded: FrameCap=0, PresentHold=5.00 ms, HoldFlush=auto (now running: hold 5.00 ms)',
                                'settings reloaded: FrameCap=60, PresentHold=4.00 ms, HoldFlush=auto (now running: frame cap 60 FPS, hold 4.00 ms, hold flush)',
                                'settings reloaded: FrameCap=0, PresentHold=off, HoldFlush=auto (now running: nothing; Present passes straight through)',
                                'settings reloaded: FrameCap=0, PresentHold=off, HoldFlush=auto (unchanged; now running: nothing; Present passes straight through)')
                            $patchLive = 'live reload: FrameCap and PresentHold are off for now; patched Present call at +0x'
                            $liveOn = 'live reload: on (changes to FrameCap, PresentHold and HoldFlush in VinceFix.ini apply while the game runs)'
                            $logChecks['log: live: patch line "live reload: FrameCap and PresentHold are off for now; patched Present call at +0x" exactly once'] = (& $count 'patched Present call at +0x') -eq 1 -and (& $count $patchLive) -eq 1
                            $logChecks['log: live: "live reload: on (changes to FrameCap, PresentHold and HoldFlush ...)" once, after the patch line'] = (& $count 'live reload:') -eq 2 -and (& $count $liveOn) -eq 1 -and (& $firstLine $liveOn) -gt (& $firstLine $patchLive)
                            $logChecks['log: live: exactly four "settings reloaded:" lines, in order: hold 5.00 ms / frame cap 60 FPS, hold 4.00 ms, hold flush / nothing / unchanged, nothing'] = $reloads.Count -eq 4 -and (($reloads -join "`n") -ceq ($reloadsWant -join "`n"))
                            $logChecks['log: live: the settings as read at load and after each changing rewrite, not after the unchanged save ("present hold: off (PresentHold=0)" twice, "frame cap: off (FrameCap=0 or empty)" three times)'] = (& $count 'present hold: off (PresentHold=0)') -eq 2 -and (& $count 'frame cap: off (FrameCap=0 or empty)') -eq 3
                            $logChecks['log: live: rewrite 1 -> "hold flush: off (HoldFlush=auto flushes only with FrameCap" once; rewrite 2 -> "hold flush: on (HoldFlush=auto and FrameCap is on" once'] = (& $count 'hold flush: off (HoldFlush=auto flushes only with FrameCap') -eq 1 -and (& $count 'hold flush: on (HoldFlush=auto and FrameCap is on') -eq 1 -and (& $count 'hold flush:') -eq 2
                            $logChecks['log: live: rewrite 2 -> "frame cap: 60 FPS (frame starts 16.66 ms apart)" and the display check "...; cap on" once each, before its "settings reloaded:" line'] = (& $count $capLine) -eq 1 -and $capOn -and (& $firstLine '; cap on') -lt (& $firstLine $reloadsWant[1])
                            $logChecks['log: live: no WARNING, not found, VirtualProtect or FAILED'] = $clean
                        }
                    }
                }
                foreach ($c in $logChecks.GetEnumerator()) { "$(if ($c.Value) { 'PASS' } else { 'FAIL' })  $($c.Key)"; if (!$c.Value) { $failed++ } }
            }
        } finally { Remove-Item $ini -ErrorAction SilentlyContinue }
        if ($failed) { throw "test hosts reported $failed failed check(s)" }
    }
} finally { Pop-Location }
