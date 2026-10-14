# Builds X3DAudio1_7.dll (VinceFix) and its test hosts with LLVM (clang, lld-link, llvm-dlltool).
# No Windows SDK or MSVC needed: import libraries come from the .def files.
# /Brepro makes the output reproducible: the same sources and LLVM version give the same bytes.
# Usage: pwsh -File build.ps1 [-Test] [-LLVM <folder with clang.exe>]
#   -Test runs the three test hosts. The XAudio2 one needs the DirectX June 2010 runtime
#   (xaudio2_7.dll), which the game needs anyway.
param([switch]$Test, [string]$LLVM = 'C:\Program Files\LLVM\bin')
$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
function Run([string]$tool, [string[]]$arguments) {
    & (Join-Path $LLVM $tool) @arguments
    if ($LASTEXITCODE) { throw "$tool failed ($LASTEXITCODE)" }
}
Push-Location $PSScriptRoot
try {
    Run 'llvm-dlltool.exe' @('-m', 'i386', '-k', '-d', 'src\kernel32.def', '-l', "$build\kernel32.lib")
    Run 'llvm-dlltool.exe' @('-m', 'i386', '-k', '-d', 'src\user32.def', '-l', "$build\user32.lib")
    Run 'llvm-dlltool.exe' @('-m', 'i386', '-k', '-d', 'tests\ole32.def', '-l', "$build\ole32.lib") # test host only
    $cflags = @('--target=i686-pc-windows-msvc', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe', '-Wall', '-Wextra', '-c')
    $link = @('/nodefaultlib', '/machine:x86', '/safeseh:no', '/Brepro')
    Run 'clang.exe' ($cflags + @('src\vincefix.c', '-o', "$build\vincefix.obj"))
    Run 'clang.exe' @('--target=i686-pc-windows-msvc', '-c', 'src\thunks.s', '-o', "$build\thunks.obj")
    Run 'lld-link.exe' ($link + @('/dll', '/entry:DllMain', '/subsystem:windows', '/def:src\exports.def', "/out:$build\X3DAudio1_7.dll",
        "$build\vincefix.obj", "$build\thunks.obj", "$build\kernel32.lib", "$build\user32.lib"))
    # Test hosts: dynamic load; load-time import like Vince.exe; load-time import plus ole32!CoCreateInstance and real XAudio2.
    Run 'clang.exe' ($cflags + @('tests\test.c', '-o', "$build\test.obj"))
    Run 'lld-link.exe' ($link + @('/entry:start', '/subsystem:console', "/out:$build\vincefix-test.exe", "$build\test.obj", "$build\kernel32.lib"))
    Run 'clang.exe' ($cflags + @('tests\test_static.c', '-o', "$build\test_static.obj"))
    Run 'lld-link.exe' ($link + @('/entry:start', '/subsystem:console', "/out:$build\vincefix-test-static.exe", "$build\test_static.obj",
        "$build\X3DAudio1_7.lib", "$build\kernel32.lib"))
    Run 'clang.exe' ($cflags + @('tests\test_xaudio.c', '-o', "$build\test_xaudio.obj"))
    Run 'lld-link.exe' ($link + @('/entry:start', '/subsystem:console', "/out:$build\vincefix-test-xaudio.exe", "$build\test_xaudio.obj",
        "$build\X3DAudio1_7.lib", "$build\ole32.lib", "$build\kernel32.lib"))
    $dll = Get-Item "$build\X3DAudio1_7.dll"
    "Built $($dll.FullName) ($($dll.Length) bytes, SHA256 $((Get-FileHash $dll.FullName -Algorithm SHA256).Hash))"
    if ($Test) {
        $failed = 0
        foreach ($host_ in 'vincefix-test.exe', 'vincefix-test-static.exe', 'vincefix-test-xaudio.exe') {
            if (Test-Path "$build\VinceFix.log") { Remove-Item "$build\VinceFix.log" }
            "=== $host_"
            & "$build\$host_"
            $failed += $LASTEXITCODE
            "--- VinceFix.log:"; $log = @(Get-Content "$build\VinceFix.log" -ErrorAction SilentlyContinue); $log
            if ($host_ -eq 'vincefix-test-xaudio.exe') {
                $count = { param($text) @($log | Where-Object { $_.Contains($text) }).Count }
                $logChecks = [ordered]@{
                    'log: CoCreateInstance import hooked once'                                = (& $count 'import CoCreateInstance at') -eq 1
                    'log: XAudio2 vtable patched once (engines B and C share it)'             = (& $count 'slot 4 (GetDeviceDetails') -eq 1
                    'log: 2-9 refreshes (B first call; C after its creation, racing threads)' = ($r = & $count 'queried XAudio2') -ge 2 -and $r -le 9
                    'log: no failures or skipped patches'                                     = (& $count 'FAILED') + (& $count 'uncached') + (& $count 'VirtualProtect') -eq 0
                }
                foreach ($c in $logChecks.GetEnumerator()) { "$(if ($c.Value) { 'PASS' } else { 'FAIL' })  $($c.Key)"; if (!$c.Value) { $failed++ } }
            }
        }
        if ($failed) { throw "test hosts reported $failed failed check(s)" }
        'All test hosts passed.'
    }
} finally { Pop-Location }
