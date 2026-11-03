# VinceAIO.ps1 -- installer for the Voodoo Vince Community Patch, the Voodoo Vince Remastered AIO patch
# (Windows PowerShell 5.1, no modules). Player-facing text names the patch by manifest.json's `name` ($Product);
# the namespace, VinceAIO.ini/.backup/.receipt.json and this file's name stay VinceAIO.
#
#   VinceAIO.ps1 status                          [-GameDir <folder>]
#   VinceAIO.ps1 install   [-Options <ids|none>] [-GameDir <folder>]
#   VinceAIO.ps1 change     -Options <ids|none>  [-GameDir <folder>]
#   VinceAIO.ps1 uninstall                       [-GameDir <folder>]
#   VinceAIO.ps1                                 text menu
#
# Options: comma list of option ids from manifest.json (voodoopowerfix, firebug, retexture) or `none` (base set).
# `install` without -Options resumes an unfinished run, repairs the installed set, or asks y/n per option.
# Exit codes: 0 ok, 1 refused, 2 bad input. Package root = the folder above this script.
#
# Every action plans current state -> target set (uninstall = stock). All checks run before any write:
# every target hashes to its stock sha, a sha the receipt records as ours, or (unfinished run) the plan target.
# Then: receipt written `pending` with the plan, stock backups (once, to VinceAIO.backup\<game path>), per-file
# <target>.tmp by stream + flush + hash + File.Replace + re-hash, receipt `done`. Uninstall back to stock
# removes the receipt and the backup folder. Deltas (VVD1, aio/delta.py) apply to the stock backup only.
# Writes stay inside the game folder; no elevation, no network, no registry writes.
#
# Earlier releases (known_releases.json in the package root): a file found at a `replace` sha with no receipt record
# is a prior: backed up once to VinceAIO.backup\prior\<game path>, recorded (prior_sha256, prior_backup), and
# uninstall restores it instead of stock. A Vince.exe prior with `requires` (VinceAudio) is allowed only while that
# file holds the stock sha; the stock backup is copied from it. `keep` files are never written. VinceAIO.ini is
# created only when absent; then each `migrate` file present (old inis) gives its values to the template (guarded
# line patches), is backed up to prior\ and removed. A VinceAIO.ini not written by VinceAIO is left untouched.
#
# Test seams: -SteamPath <dir> replaces the HKCU SteamPath read; -StopAfterWrites <n> exits 9 right after
# the n-th completed write step (backup, tmp written, replace or delete, receipt).
param(
    [Parameter(Position = 0)][string]$Action,
    [string]$GameDir,
    [string]$Options,
    [string]$SteamPath,
    [int]$StopAfterWrites = 0,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$Namespace = 'VinceAIO'
$Product = ''  # manifest.json `name`, set by Load-Manifest before any text that shows it
$AppId = '545980'
$RootDlls = @('X3DAudio1_7.dll', 'd3d11.dll')
$IniName = 'VinceAIO.ini'
$ReceiptName = 'VinceAIO.receipt.json'
$BackupDir = 'VinceAIO.backup'
$Package = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))
$Buf = New-Object byte[] 1048576
$EmptySha = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
$script:Steps = 0
$script:Interactive = $false

# ---------- errors, prompts ----------
function Fail([int]$Code, [string]$Message) {
    $e = New-Object InvalidOperationException $Message
    $e.Data['code'] = $Code
    throw $e
}
function Refuse([string]$Message) { Fail 1 $Message }
function BadInput([string]$Message) { Fail 2 $Message }
function Ask([string]$Prompt) {
    Write-Host -NoNewline "$Prompt "
    $line = [Console]::In.ReadLine()
    if ($null -eq $line) { Write-Host ''; return '' }
    return $line.Trim()
}
function Step([string]$What) {
    $script:Steps++
    if ($StopAfterWrites -gt 0 -and $script:Steps -ge $StopAfterWrites) {
        Write-Host "Stopped after write step $($script:Steps) ($What) [test seam]."
        exit 9
    }
}
function Prop($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $p = $Object.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}

# ---------- hashing and streams ----------
function Hex([byte[]]$Bytes) { return [BitConverter]::ToString($Bytes).Replace('-', '').ToLowerInvariant() }
function New-Sha { return New-Object Security.Cryptography.SHA256Cng }
function Exists([string]$Path) { return [IO.File]::Exists($Path) }
function Open-Read([string]$Path) {
    return New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576)
}
function Hash([string]$Path) {
    if (-not (Exists $Path)) { return $null }
    $stream = Open-Read $Path
    $sha = New-Sha
    try {
        while (($n = $stream.Read($Buf, 0, $Buf.Length)) -gt 0) { [void]$sha.TransformBlock($Buf, 0, $n, $null, 0) }
        [void]$sha.TransformFinalBlock($Buf, 0, 0)
        return Hex $sha.Hash
    } finally { $sha.Dispose(); $stream.Dispose() }
}
function New-Writer([string]$Path) {
    $parent = [IO.Path]::GetDirectoryName($Path)
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None, 1048576)
    return @{ fs = $fs; sha = (New-Sha); path = $Path }
}
function Put($Writer, [byte[]]$Bytes, [int]$Count) {
    $Writer.fs.Write($Bytes, 0, $Count)
    [void]$Writer.sha.TransformBlock($Bytes, 0, $Count, $null, 0)
}
function Close-Writer($Writer) {
    $Writer.fs.Flush($true)
    $Writer.fs.Dispose()
    [void]$Writer.sha.TransformFinalBlock($Buf, 0, 0)
    $h = Hex $Writer.sha.Hash
    $Writer.sha.Dispose()
    return $h
}
function Drop-Writer($Writer) {
    try { $Writer.fs.Dispose(); $Writer.sha.Dispose() } catch { }
    if (Exists $Writer.path) { [IO.File]::Delete($Writer.path) }
}
function Copy-N($In, $Writer, [uint64]$Count, [string]$Name) {
    while ($Count -gt 0) {
        $want = [int][Math]::Min([uint64]$Buf.Length, $Count)
        $got = $In.Read($Buf, 0, $want)
        if ($got -le 0) { Refuse "${Name}: unexpected end of data" }
        Put $Writer $Buf $got
        $Count -= [uint64]$got
    }
}
# Stream $Source into $Tmp; returns the written sha. A sha other than $Expect deletes the tmp and refuses.
function Copy-ToTmp([string]$Source, [string]$Tmp, [string]$Expect, [string]$Name) {
    $in = Open-Read $Source
    $w = New-Writer $Tmp
    $ok = $false
    try {
        Copy-N $in $w ([uint64]$in.Length) $Name
        $h = Close-Writer $w
        if ($h -ne $Expect) { Refuse "${Name}: copied data has sha256 $h, expected $Expect" }
        $ok = $true
        return $h
    } finally { $in.Dispose(); if (-not $ok) { Drop-Writer $w } }
}
# Write $Bytes into $Tmp; a sha other than $Expect deletes the tmp and refuses.
function Bytes-ToTmp([byte[]]$Bytes, [string]$Tmp, [string]$Expect, [string]$Name) {
    $w = New-Writer $Tmp
    $ok = $false
    try {
        Put $w $Bytes $Bytes.Length
        $h = Close-Writer $w
        if ($h -ne $Expect) { Refuse "${Name}: written data has sha256 $h, expected $Expect" }
        $ok = $true
        return $h
    } finally { if (-not $ok) { Drop-Writer $w } }
}
function Hash-Bytes([byte[]]$Bytes) {
    $sha = New-Sha
    try { return Hex $sha.ComputeHash($Bytes) } finally { $sha.Dispose() }
}

# ---------- VVD1 deltas (aio/delta.py) ----------
function Read-DeltaHeader([string]$Path, [string]$Name) {
    $stream = Open-Read $Path
    $file = New-Sha
    $body = New-Sha
    try {
        if ($stream.Length -lt 120) { Refuse "${Name}: delta shorter than its header" }
        $head = New-Object byte[] 120
        if ($stream.Read($head, 0, 120) -ne 120) { Refuse "${Name}: unreadable header" }
        [void]$file.TransformBlock($head, 0, 120, $null, 0)
        while (($n = $stream.Read($Buf, 0, $Buf.Length)) -gt 0) {
            [void]$file.TransformBlock($Buf, 0, $n, $null, 0)
            [void]$body.TransformBlock($Buf, 0, $n, $null, 0)
        }
        [void]$file.TransformFinalBlock($Buf, 0, 0)
        [void]$body.TransformFinalBlock($Buf, 0, 0)
        $magic = [Text.Encoding]::ASCII.GetString($head, 0, 4)
        if ($magic -ne 'VVD1') { Refuse "${Name}: bad magic" }
        $version = [BitConverter]::ToUInt32($head, 4)
        if ($version -ne 1) { Refuse "${Name}: unsupported delta version $version" }
        $d = @{
            SourceSize = [BitConverter]::ToUInt64($head, 8); TargetSize = [BitConverter]::ToUInt64($head, 16)
            SourceSha = Hex $head[24..55]; TargetSha = Hex $head[56..87]; BodySha = Hex $head[88..119]
            FileSha = Hex $file.Hash; Length = $stream.Length
        }
        if ((Hex $body.Hash) -ne $d.BodySha) { Refuse "${Name}: delta body sha256 mismatch (corrupt delta)" }
        return $d
    } finally { $file.Dispose(); $body.Dispose(); $stream.Dispose() }
}
# Apply delta $Delta (header $D, already body-checked) to $Source (stock bytes; $null = empty) into $Tmp.
function Apply-Delta([string]$Source, [string]$Delta, $D, [string]$Tmp, [string]$Name) {
    $ds = Open-Read $Delta
    $br = New-Object IO.BinaryReader($ds)
    $ss = $null
    $w = $null
    $ok = $false
    try {
        if ($Source) { $ss = Open-Read $Source }
        $srcLen = [uint64]0
        if ($ss) { $srcLen = [uint64]$ss.Length }
        if ($srcLen -ne $D.SourceSize) { Refuse "${Name}: source is not the stock file this delta was made from" }
        $w = New-Writer $Tmp
        $ds.Position = 120
        $end = [uint64]$ds.Length
        $total = [uint64]0
        while ([uint64]$ds.Position -lt $end) {
            $op = $br.ReadByte()
            if ($op -eq 1) {
                if ([uint64]$ds.Position + 16 -gt $end) { Refuse "${Name}: truncated COPY" }
                $off = $br.ReadUInt64()
                $n = $br.ReadUInt64()
                if ($n -gt $srcLen -or $off -gt $srcLen - $n) { Refuse "${Name}: COPY out of source bounds" }
                $ss.Position = [long]$off
                Copy-N $ss $w $n $Name
            } elseif ($op -eq 2) {
                if ([uint64]$ds.Position + 8 -gt $end) { Refuse "${Name}: truncated ADD" }
                $n = $br.ReadUInt64()
                if ($n -gt $end - [uint64]$ds.Position) { Refuse "${Name}: ADD out of body bounds" }
                Copy-N $ds $w $n $Name
            } else { Refuse ("${Name}: unknown op 0x{0:x2}" -f $op) }
            $total += $n
            if ($total -gt $D.TargetSize) { Refuse "${Name}: result exceeds target size" }
        }
        $h = Close-Writer $w
        if ($total -ne $D.TargetSize -or $h -ne $D.TargetSha) { Refuse "${Name}: result does not match target sha256" }
        $ok = $true
        return $h
    } finally {
        $br.Dispose(); $ds.Dispose()
        if ($ss) { $ss.Dispose() }
        if (-not $ok -and $w) { Drop-Writer $w }
    }
}

# ---------- paths ----------
function Game-File([string]$Game, [string]$Rel) {
    $bad = (-not $Rel) -or $Rel.Contains(':') -or $Rel.StartsWith('/') -or $Rel.StartsWith('\') -or
           [IO.Path]::IsPathRooted($Rel) -or $Rel.EndsWith('.before-protoball')
    if (-not $bad) {
        foreach ($seg in $Rel.Split([char[]]@('/', '\'))) { if ($seg -eq '' -or $seg -eq '.' -or $seg -eq '..') { $bad = $true } }
    }
    if ($bad) { Refuse "manifest path '$Rel': not a safe path inside the game folder" }
    $full = [IO.Path]::GetFullPath([IO.Path]::Combine($Game, $Rel.Replace('/', '\')))
    if (-not $full.StartsWith($Game + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Refuse "manifest path '$Rel': resolves outside the game folder"
    }
    return $full
}
function Clear-ReadOnly([string]$Path) {
    if (Exists $Path) {
        $a = [IO.File]::GetAttributes($Path)
        if ($a -band [IO.FileAttributes]::ReadOnly) { [IO.File]::SetAttributes($Path, $a -band (-bnot [IO.FileAttributes]::ReadOnly)) }
    }
}
function Put-InPlace([string]$Tmp, [string]$Target) {
    Clear-ReadOnly $Target
    if (Exists $Target) { [IO.File]::Replace($Tmp, $Target, [NullString]::Value) }
    else { [IO.File]::Move($Tmp, $Target) }
}

# ---------- game folder ----------
function Find-SteamGames {
    $root = $SteamPath
    if (-not $root) {
        $key = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue
        if ($key) { $root = [string]$key.SteamPath }
    }
    if (-not $root) { return @() }
    $libraries = @($root.Replace('/', '\'))
    try {
        $vdf = [IO.Path]::Combine($libraries[0], 'steamapps\libraryfolders.vdf')
        if (Exists $vdf) {
            foreach ($m in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"\r\n]+)"')) {
                $libraries += $m.Groups[1].Value.Replace('\\', '\').Replace('/', '\')
            }
        }
    } catch { }
    $seen = @{}
    $found = @()
    foreach ($lib in $libraries) {
        try {
            $acf = [IO.Path]::Combine($lib, "steamapps\appmanifest_$AppId.acf")
            if (-not (Exists $acf)) { continue }
            $m = [regex]::Match([IO.File]::ReadAllText($acf), '"installdir"\s+"([^"\r\n]+)"')
            if (-not $m.Success) { continue }
            $dir = [IO.Path]::GetFullPath([IO.Path]::Combine($lib, 'steamapps\common', $m.Groups[1].Value)).TrimEnd('\')
            if ((Exists ([IO.Path]::Combine($dir, 'Vince.exe'))) -and -not $seen.ContainsKey($dir)) {
                $seen[$dir] = $true
                $found += $dir
            }
        } catch { continue }
    }
    return $found
}
function Select-Game {
    $dir = $GameDir
    if (-not $dir) {
        $found = @(Find-SteamGames)
        if ($found.Count -eq 1) { $dir = $found[0] }
        elseif (-not $script:Interactive) {
            if ($found.Count -eq 0) { Refuse 'no Steam install of Voodoo Vince Remastered found; pass -GameDir <folder>' }
            Refuse ("several Steam installs found ({0}); pass -GameDir <folder>" -f ($found -join '; '))
        } elseif ($found.Count -gt 1) {
            Write-Host 'Several game installs were found:'
            for ($i = 0; $i -lt $found.Count; ++$i) { Write-Host ("  {0}. {1}" -f ($i + 1), $found[$i]) }
            $c = 0
            if (-not ([int]::TryParse((Ask 'Install number:'), [ref]$c) -and $c -ge 1 -and $c -le $found.Count)) {
                BadInput 'no install selected'
            }
            $dir = $found[$c - 1]
        } else {
            $dir = Ask 'Game folder (the one containing Vince.exe):'
            if (-not $dir) { BadInput 'no game folder given' }
        }
    }
    $dir = [IO.Path]::GetFullPath($dir.Trim('"')).TrimEnd('\')
    if (-not (Exists ([IO.Path]::Combine($dir, 'Vince.exe')))) { BadInput "$dir does not contain Vince.exe" }
    return $dir
}
function Assert-NotRunning {
    $p = @(Get-Process -Name Vince -ErrorAction SilentlyContinue)
    if ($p.Count -gt 0) {
        Refuse ("Vince.exe is running (pid {0}); close the game first" -f (($p | ForEach-Object { $_.Id }) -join ', '))
    }
}

# ---------- package ----------
function Load-Manifest {
    $path = [IO.Path]::Combine($Package, 'manifest.json')
    if (-not (Exists $path)) { Refuse "manifest.json not found in $Package" }
    try { $m = [IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { Refuse "manifest.json: not valid JSON" }
    foreach ($k in @('name', 'version', 'sets', 'stock_sha256', 'aio_files', 'ini_sha256', 'target_exe_sha256')) {
        if ($null -eq (Prop $m $k)) { Refuse "manifest.json: missing $k" }
    }
    if ((Prop $m.stock_sha256 'Vince.exe') -ne $m.target_exe_sha256) { Refuse 'manifest.json: stock Vince.exe sha is not target_exe_sha256' }
    $ids = @()
    foreach ($s in $m.sets) {
        foreach ($o in @($s.options)) { if ($ids -notcontains $o) { $ids += $o } }
        foreach ($f in @($s.files)) {
            [void](Game-File 'C:\x' $f.game_path)
            if (-not $m.stock_sha256.PSObject.Properties[$f.game_path]) { Refuse "manifest.json: $($f.game_path) has no stock record" }
            if ($f.delta -notmatch '^deltas/[0-9a-f]{64}\.vvd$') { Refuse "manifest.json: bad delta name '$($f.delta)'" }
        }
    }
    foreach ($n in $RootDlls + @($IniName)) { if (-not (Prop $m.aio_files $n)) { Refuse "manifest.json: no aio_files entry for $n" } }
    if ($m.aio_files.$IniName -ne $m.ini_sha256) { Refuse 'manifest.json: ini_sha256 differs from aio_files' }
    $script:Product = $m.name
    $m | Add-Member -NotePropertyName OptionIds -NotePropertyValue $ids
    $m | Add-Member -NotePropertyName Known -NotePropertyValue (Load-Known $m)
    return $m
}
# known_releases.json (schema 1): files of earlier releases (replace / keep / migrate) and the ini keys to migrate.
function Load-Known($M) {
    $path = [IO.Path]::Combine($Package, 'known_releases.json')
    if (-not (Exists $path)) { Refuse "known_releases.json not found in $Package" }
    try { $k = [IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { Refuse 'known_releases.json: not valid JSON' }
    if ((Prop $k 'schema') -ne 1) { Refuse 'known_releases.json: not schema 1' }
    foreach ($f in @($k.files)) {
        [void](Game-File 'C:\x' $f.game_path)
        if (@('replace', 'keep', 'migrate') -notcontains $f.action -or $f.sha256 -notmatch '^[0-9a-f]{64}$') {
            Refuse "known_releases.json: bad entry for $($f.game_path)"
        }
        $req = Prop $f 'requires'
        if ($req) {
            [void](Game-File 'C:\x' $req.game_path)
            if ($req.sha256 -ne (Prop $M.stock_sha256 $f.game_path)) { Refuse "known_releases.json: $($f.game_path) requires a file that is not its stock copy" }
        }
    }
    foreach ($g in @($k.migrate)) { [void](Game-File 'C:\x' $g.file) }
    return $k
}
function Find-Set($M, [string[]]$Chosen) {
    $want = @($Chosen | Sort-Object) -join ','
    foreach ($s in $M.sets) { if ((@($s.options | Sort-Object) -join ',') -eq $want) { return $s } }
    BadInput "no release set has options '$want'"
}
function Parse-Options($M, [string]$Text) {
    $t = $Text.Trim()
    if ($t -eq 'none') { return Find-Set $M @() }
    $chosen = @()
    foreach ($o in $t.Split(',')) {
        $o = $o.Trim().ToLowerInvariant()
        if (-not $o) { continue }
        if ($M.OptionIds -notcontains $o) { BadInput "unknown option '$o' (known: $($M.OptionIds -join ', '), or none)" }
        if ($chosen -notcontains $o) { $chosen += $o }
    }
    if ($chosen.Count -eq 0) { BadInput "-Options needs option ids or none" }
    return Find-Set $M $chosen
}
function Ask-Options($M) {
    $chosen = @()
    foreach ($o in $M.OptionIds) { if ((Ask "Enable ${o}? (y/n)") -match '^[yY]') { $chosen += $o } }
    return Find-Set $M $chosen
}

# ---------- ini migration (C0025: one guarded line patch per key) ----------
# Put Key=Value into [Section] of $Text: replace its one Key= line, or add the line at the end of a section that
# holds no settings yet (the [VinceAudio] comment block). Anything else refuses: the template is not the expected one.
function Set-IniKey([string]$Text, [string]$Section, [string]$Key, [string]$Value, [string]$Nl) {
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.AddRange([string[]][regex]::Split($Text, '(?<=\n)'))
    $start = -1
    $end = $lines.Count
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        $t = $lines[$i].Trim()
        if ($t.StartsWith('[')) {
            if ($start -ge 0) { $end = $i; break }
            if ($t -eq "[$Section]") { $start = $i }
        }
    }
    if ($start -lt 0) { Refuse "$IniName template: no [$Section] section" }
    $hits = @()
    $settings = 0
    for ($i = $start + 1; $i -lt $end; ++$i) {
        if ($lines[$i] -cmatch ('^' + [regex]::Escape($Key) + '=')) { $hits += $i }
        if ($lines[$i] -match '^[^;\s][^=]*=') { $settings++ }
    }
    if ($hits.Count -eq 1) {
        $i = $hits[0]
        $body = $lines[$i].TrimEnd([char[]]@("`r", "`n"))
        $lines[$i] = "$Key=$Value" + $lines[$i].Substring($body.Length)
    } elseif ($hits.Count -eq 0 -and $settings -eq 0) {
        $at = $end
        while ($at -gt $start + 1 -and $lines[$at - 1] -eq '') { $at-- }
        $pre = ''
        if (-not $lines[$at - 1].EndsWith("`n")) { $pre = $Nl }
        $lines.Insert($at, "$pre$Key=$Value$Nl")
    } else { Refuse "$IniName template: [$Section] $Key is not on exactly one line" }
    return [string]::Concat($lines)
}
# The new VinceAIO.ini: the package template with each migrate file's values in its section. Values come from the
# file's prior backup when one exists (a rerun after a kill gives the same bytes), else from the game file.
# Returns @{ bytes; notes }.
function Build-Ini([string]$Game, $M) {
    $text = [IO.File]::ReadAllText([IO.Path]::Combine($Package, $IniName))
    $nl = "`n"
    if ($text.Contains("`r`n")) { $nl = "`r`n" }
    $notes = @()
    foreach ($mg in @($M.Known.migrate)) {
        $src = Game-File $Game "$BackupDir/prior/$($mg.file)"
        if (-not (Exists $src)) { $src = Game-File $Game $mg.file }
        if (-not (Exists $src)) { continue }
        $vals = @{}
        $sec = $null
        foreach ($line in [regex]::Split([IO.File]::ReadAllText($src), '\r?\n')) {
            $hm = [regex]::Match($line, '^\s*\[([^\]]*)\]')
            if ($hm.Success) { $sec = $hm.Groups[1].Value.Trim(); continue }
            if ($sec -ne $mg.from) { continue }
            $kv = [regex]::Match($line, '^\s*([^=;#\s][^=]*?)\s*=(.*)$')
            if (-not $kv.Success) { continue }
            foreach ($key in @($mg.keys)) {
                if ($key -eq $kv.Groups[1].Value -and -not $vals.ContainsKey($key)) { $vals[$key] = $kv.Groups[2].Value.Trim() }
            }
        }
        foreach ($key in @($mg.keys)) {
            if (-not $vals.ContainsKey($key)) { continue }
            $v = $vals[$key]
            if ($v -notmatch '^[A-Za-z0-9.]{1,16}$') {
                $notes += "$($mg.file): [$($mg.from)] $key value '$v' not carried over (letters, digits and dots only); the default stays"
                continue
            }
            $text = Set-IniKey $text $mg.to $key $v $nl
        }
    }
    return @{ bytes = (New-Object Text.UTF8Encoding $false).GetBytes($text); notes = $notes }
}

# ---------- receipt ----------
function Read-Receipt([string]$Game) {
    $path = [IO.Path]::Combine($Game, $ReceiptName)
    if (-not (Exists $path)) { return $null }
    try { $r = [IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { Refuse "${ReceiptName}: not valid JSON" }
    if ((Prop $r 'backup_namespace') -ne $Namespace -or @('pending', 'done') -notcontains (Prop $r 'state')) {
        Refuse "${ReceiptName}: not a $Product receipt"
    }
    return $r
}
function Write-Receipt([string]$Game, $Receipt) {
    $path = [IO.Path]::Combine($Game, $ReceiptName)
    $bytes = (New-Object Text.UTF8Encoding $false).GetBytes((ConvertTo-Json -InputObject $Receipt -Depth 8))
    $w = New-Writer "$path.tmp"
    $ok = $false
    try {
        Put $w $bytes $bytes.Length
        $h = Close-Writer $w
        $ok = $true
    } finally { if (-not $ok) { Drop-Writer $w } }
    Put-InPlace "$path.tmp" $path
    if ((Hash $path) -ne $h) { Refuse "${ReceiptName}: re-hash after write failed" }
    Step 'receipt'
}
function Records($Receipt) {
    $map = @{}
    if ($Receipt) { foreach ($f in @($Receipt.files)) { $map[$f.game_path] = $f } }
    return $map
}

# ---------- status ----------
function Show-Status([string]$Game, $M) {
    $r = Read-Receipt $Game
    $exe = Hash ([IO.Path]::Combine($Game, 'Vince.exe'))
    Write-Host "Game folder: $Game"
    Write-Host "Package: $Product $($M.version)"
    $owned = @()
    if ($r) { $f = (Records $r)['Vince.exe']; if ($f) { $owned = @($f.owned) } }
    if ($exe -eq $M.target_exe_sha256) { Write-Host 'Vince.exe: stock Steam build' }
    elseif ($owned -contains $exe) { Write-Host "Vince.exe: patched by $Product" }
    else { Write-Host "Vince.exe: UNKNOWN build (sha256 $exe); install will refuse" }
    # earlier releases: a file at its pinned sha now, or recorded as the prior VinceAIO replaced
    $recs = Records $r
    $hashes = @{}
    $found = [ordered]@{}
    foreach ($f in @($M.Known.files)) {
        $p = $f.game_path
        if (-not $hashes.ContainsKey($p)) { $hashes[$p] = Hash (Game-File $Game $p) }
        $pr = $null; $own = @()
        if ($recs[$p]) { $pr = Prop $recs[$p] 'prior_sha256'; $own = @(Prop $recs[$p] 'owned') }
        # a current sha VinceAIO owns is its own file, not the earlier release that shares its bytes
        if (($hashes[$p] -eq $f.sha256 -and $own -notcontains $hashes[$p]) -or $pr -eq $f.sha256) {
            if (-not $found.Contains($f.release)) { $found[$f.release] = @() }
            $found[$f.release] += $p
        }
    }
    foreach ($k in $found.Keys) { Write-Host "Earlier release found: $k ($($found[$k] -join ', '))" }
    if (-not $r) { Write-Host 'State: not installed' }
    elseif ($r.state -eq 'pending') {
        $to = if ($null -eq $r.target_set) { 'stock (uninstall)' } else { "set $($r.target_set)" }
        Write-Host "State: PENDING - an unfinished run toward $to; run install to finish it or uninstall to roll back"
    } else {
        Write-Host "State: installed set $($r.set) (options: $(if (@($r.options).Count) { @($r.options) -join ', ' } else { 'none' }))"
    }
}

# ---------- plan and run ----------
# $Target = a manifest set, or $null for stock.
function Invoke-Plan([string]$Game, $M, $Target) {
    Assert-NotRunning
    Write-Host 'Working; this takes a moment. Keep this window open until it says it is done.'
    $receipt = Read-Receipt $Game
    $recs = Records $receipt
    $receiptPath = [IO.Path]::Combine($Game, $ReceiptName)
    if (Exists "$receiptPath.tmp") {
        try { $leftover = [IO.File]::ReadAllText("$receiptPath.tmp") | ConvertFrom-Json } catch { $leftover = $null }
        if ((Prop $leftover 'backup_namespace') -ne $Namespace) { Refuse "$ReceiptName.tmp: unknown file; remove it first" }
    }

    $notes = @()
    # earlier releases: replace shas per path, the old inis to migrate
    $priorShas = @{}
    foreach ($f in @($M.Known.files)) {
        if ($f.action -ne 'replace') { continue }
        if (-not $priorShas.ContainsKey($f.game_path)) { $priorShas[$f.game_path] = @() }
        $priorShas[$f.game_path] += $f.sha256
    }
    $migFiles = @(@($M.Known.migrate) | ForEach-Object { $_.file })
    # VinceAIO.ini is created (and the old inis migrated into it) only when absent
    $migrating = ($null -ne $Target) -and -not (Exists (Game-File $Game $IniName))
    $newIni = $null
    if ($migrating) { $newIni = Build-Ini $Game $M; $notes += $newIni.notes }

    # universe: every file any set, the receipt, the root members or the old inis can touch
    $paths = @{}
    foreach ($s in $M.sets) { foreach ($f in @($s.files)) { $paths[$f.game_path] = $true } }
    foreach ($k in $recs.Keys) { $paths[$k] = $true }
    foreach ($n in $RootDlls + @($IniName) + $migFiles) { $paths[$n] = $true }
    [string[]]$order = @($paths.Keys)
    [Array]::Sort($order, [StringComparer]::Ordinal)
    $byPath = @{}
    if ($Target) { foreach ($f in @($Target.files)) { $byPath[$f.game_path] = $f } }

    $items = @()
    $deltas = @{}
    foreach ($rel in $order) {
        $full = Game-File $Game $rel
        $rec = $recs[$rel]
        $isRoot = ($RootDlls + @($IniName)) -contains $rel
        $isMig = $migFiles -contains $rel
        if ($isRoot -or $isMig) {
            $stock = $null
        } else {
            $sp = $M.stock_sha256.PSObject.Properties[$rel]
            if (-not $sp) { Refuse "${rel}: recorded in the receipt but unknown to this package" }
            $stock = $sp.Value
        }
        $owned = @()
        if ($rec) { $owned = @(@($rec.owned) | Where-Object { $_ }) }
        $planned = $null
        if ($rec -and $receipt.state -eq 'pending') { $planned = Prop $rec 'result_sha256' }
        $cur = Hash $full
        $kind = 'game'
        if ($rel -eq $IniName) { $kind = 'ini' } elseif ($isRoot) { $kind = 'root' } elseif ($isMig) { $kind = 'migrate' }

        # prior: the earlier release's file VinceAIO replaces (or migrates) and uninstall restores
        $prior = $null
        if ($rec) { $prior = Prop $rec 'prior_sha256' }
        elseif ($kind -ne 'migrate' -and $null -ne $cur -and $priorShas[$rel] -contains $cur) { $prior = $cur }
        if ($kind -eq 'migrate' -and $null -eq $prior -and $migrating -and $null -ne $cur) { $prior = $cur }

        # what this file must become
        $want = $stock
        if ($null -ne $prior) { $want = $prior }
        $pkgFile = $null
        $entry = $null
        if ($kind -eq 'root') {
            if ($Target) { $want = $M.aio_files.$rel; $pkgFile = [IO.Path]::Combine($Package, $rel) }
        } elseif ($kind -eq 'ini') {
            $pkgFile = [IO.Path]::Combine($Package, $rel)
            $want = $cur
            if ($Target -and $null -eq $cur) { $want = Hash-Bytes $newIni.bytes }
            if (-not $Target -and $null -ne $cur -and $owned -contains $cur) { $want = $null }
        } elseif ($kind -eq 'migrate') {
            $want = $cur
            if ($null -ne $prior) { $want = $(if ($Target) { $null } else { $prior }) }
        } elseif ($byPath.ContainsKey($rel)) {
            $entry = $byPath[$rel]
            $want = $entry.result_sha256
        }

        # C14: the current file must be stock, recorded as ours, or the unfinished plan's target
        $allowed = @($stock) + $owned + @($planned) + @($prior)
        if ($rel -eq 'Vince.exe' -and $allowed -notcontains $cur) {
            Refuse "Vince.exe: not the stock Steam build nor a $Product build (sha256 $cur); nothing was changed"
        }
        if ($kind -eq 'ini') {
            # a VinceAIO.ini not written by VinceAIO stays untouched (want = cur) and is never owned
        } elseif ($kind -eq 'migrate') {
            if ($null -ne $prior -and $null -ne $cur -and $cur -ne $prior) {
                Refuse "${rel}: present again after $Product moved its settings into $IniName (sha256 $cur); remove it first"
            }
        } elseif ($allowed -notcontains $cur) {
            if ($null -eq $cur) { Refuse "${rel}: missing from the game folder (verify the game files in Steam)" }
            if ($null -eq $stock -and -not $rec) { Refuse "${rel}: already present and not installed by $Product; remove it first" }
            Refuse "${rel}: sha256 $cur is neither stock nor recorded by $Product (another mod?); nothing was changed"
        }
        # a prior that needs another file at the stock sha (VinceAudio's Vince.exe backup): the stock backup comes from it
        $stockSrc = $null
        if ($null -ne $stock -and $cur -eq $stock) { $stockSrc = $full }
        if ($null -ne $prior -and $cur -eq $prior) {
            $pe = @(@($M.Known.files) | Where-Object { $_.game_path -eq $rel -and $_.sha256 -eq $prior -and $_.action -eq 'replace' })
            $req = $null
            if ($pe.Count) { $req = Prop $pe[0] 'requires' }
            if ($req) {
                $reqFull = Game-File $Game $req.game_path
                if ($want -ne $cur) {
                    $rs = Hash $reqFull
                    if ($null -eq $rs -or $rs -ne $stock -or $rs -ne $req.sha256) {
                        $rsText = $(if ($null -eq $rs) { 'missing' } else { "sha256 $rs" })
                        Refuse "$($req.game_path): missing or not the stock $rel ($rsText); $rel is $($pe[0].release) and needs it to upgrade; nothing was changed"
                    }
                }
                $stockSrc = $reqFull
            }
        }

        # backup of the stock file
        $bakRel = "$BackupDir/$rel"
        $bak = Game-File $Game $bakRel
        $bakSha = Hash $bak
        if ($null -ne $bakSha) {
            if ($null -eq $stock -or $bakSha -ne $stock) { Refuse "${bakRel}: backup is not the stock file (stale backup); nothing was changed" }
        }
        $needsStock = ($null -ne $stock) -and ($cur -ne $stock) -and ($want -ne $cur) -and ($null -eq $prior -or $want -ne $prior)
        if ($needsStock -and $null -eq $bakSha -and $null -eq $stockSrc) { Refuse "${rel}: no stock backup in $BackupDir to restore from" }

        # backup of the prior file
        $pbakRel = "$BackupDir/prior/$rel"
        $pbak = Game-File $Game $pbakRel
        $pbakSha = Hash $pbak
        if ($null -ne $pbakSha -and $pbakSha -ne $prior) { Refuse "${pbakRel}: backup is not the earlier release's file (stale backup); nothing was changed" }
        if ($null -ne $prior -and $want -eq $prior -and $cur -ne $prior -and $null -eq $pbakSha) {
            Refuse "${rel}: no backup in $BackupDir\prior to restore the earlier release's file from"
        }

        # leftover temp files
        $tmps = @()
        foreach ($t in @("$full.tmp", "$bak.tmp", "$pbak.tmp")) {
            if (Exists $t) {
                $ts = Hash $t
                if ((@($stock) + $owned + @($planned) + @($want) + @($prior)) -notcontains $ts) {
                    Refuse "$([IO.Path]::GetFileName($t)): leftover temp file not written by $Product; remove it first"
                }
                $tmps += $t
            }
        }

        # package data
        $d = $null
        if ($kind -eq 'game' -and $null -ne $want -and $want -ne $stock -and $want -ne $cur -and $want -ne $prior) {
            $dPath = Game-File $Package $entry.delta
            if (-not (Exists $dPath)) { Refuse "$($entry.delta): missing from the package" }
            if (-not $deltas.ContainsKey($entry.delta)) { $deltas[$entry.delta] = Read-DeltaHeader $dPath $entry.delta }
            $d = $deltas[$entry.delta]
            $src = $stock
            if ($null -eq $src) { $src = $EmptySha }
            if ($d.FileSha -ne $entry.delta_sha256) { Refuse "$($entry.delta): sha256 differs from manifest.json" }
            if ($d.SourceSha -ne $src -or $entry.source_sha256 -ne $src) { Refuse "$($entry.delta): made from another source than stock $rel" }
            if ($d.TargetSha -ne $want) { Refuse "$($entry.delta): result sha differs from manifest.json" }
            $d.Path = $dPath
        }
        if (($kind -eq 'root' -or $kind -eq 'ini') -and $null -ne $want -and $want -ne $cur -and $want -ne $prior) {
            $pw = $want
            if ($kind -eq 'ini') { $pw = $M.ini_sha256 }  # the template; the written bytes carry the migrated values
            if ((Hash $pkgFile) -ne $pw) { Refuse "${rel}: package copy does not match manifest.json" }
        }
        $items += @{ rel = $rel; full = $full; kind = $kind; stock = $stock; cur = $cur; target = $want; owned = $owned;
                     planned = $planned; bak = $bak; bakRel = $bakRel; bakSha = $bakSha; tmps = $tmps; delta = $d; pkg = $pkgFile;
                     hasRec = [bool]$rec; prior = $prior; pbak = $pbak; pbakRel = $pbakRel; pbakSha = $pbakSha; stockSrc = $stockSrc }
    }

    $work = @($items | Where-Object { $_.target -ne $_.cur -or $_.tmps.Count -gt 0 }).Count
    if ($work -eq 0 -and (-not $receipt -or $receipt.state -eq 'done')) {
        if (-not $Target -and -not $receipt -and -not [IO.Directory]::Exists([IO.Path]::Combine($Game, $BackupDir))) {
            Write-Host "Nothing to uninstall: $Product is not installed."
            return
        }
        if ($Target -and $receipt -and $receipt.set -eq $Target.set) {
            Write-Host "Set $($Target.set) is already installed; nothing changed."
            return
        }
    }

    # ---- writes start here ----
    $records = @()
    foreach ($it in $items) {
        $own = @($it.owned)
        $mine = @($it.planned, $it.target)
        if ($it.kind -eq 'ini') { $mine = @(); if ($it.target -ne $it.cur) { $mine = @($it.target) } }  # an edited ini never becomes ours
        if ($it.kind -eq 'migrate') { $mine = @() }  # old inis are only moved aside and restored, never ours
        foreach ($s in $mine) {
            if ($null -ne $s -and $s -ne $it.stock -and $s -ne $it.prior -and $own -notcontains $s) { $own += $s }
        }
        $it.own = $own
        $hasBak = ($null -ne $it.bakSha) -or (($null -ne $it.stock) -and ($it.target -ne $it.stock))
        $hasPrior = ($null -ne $it.pbakSha) -or (($null -ne $it.prior) -and ($it.cur -eq $it.prior) -and ($it.target -ne $it.cur))
        $records += [ordered]@{ game_path = $it.rel; stock_sha256 = $it.stock; result_sha256 = $it.target; owned = @($own)
                                backup = $(if ($hasBak) { $it.bakRel } else { $null }); prior_sha256 = $it.prior
                                prior_backup = $(if ($hasPrior) { $it.pbakRel } else { $null }) }
    }
    $before = @{ set = $null; options = @() }
    if ($receipt) { $before = @{ set = $receipt.set; options = @($receipt.options) } }
    $tset = $null; $topt = @()
    if ($Target) { $tset = $Target.set; $topt = @($Target.options) }
    Write-Receipt $Game ([ordered]@{ backup_namespace = $Namespace; version = $M.version; state = 'pending';
        set = $before.set; options = @($before.options); target_set = $tset; target_options = @($topt); files = @($records) })

    foreach ($it in $items) { foreach ($t in $it.tmps) { if (Exists $t) { [IO.File]::Delete($t) } } }
    # progress: [k/n] per file written or deleted; $keep is the loop's own skip rule (an ini left on uninstall)
    $keep = { param($i) $i.kind -eq 'ini' -and -not $Target -and $null -ne $i.cur -and $null -ne $i.target }
    $total = @($items | Where-Object { -not (& $keep $_) -and $_.target -ne $_.cur }).Count
    $done = 0
    foreach ($it in $items) {
        $rel = $it.rel
        if (& $keep $it) {
            if (@($it.owned).Count -eq 0) { $notes += "${rel}: not written by $Product; left in place" }
            else { $notes += "${rel}: edited since install; left in place" }
            continue
        }
        if ($it.kind -eq 'ini' -and $Target -and -not $it.hasRec -and $null -ne $it.cur) {
            $notes += "${rel}: already present and not written by $Product; left untouched (old settings files not migrated)"
        }
        if ($it.target -eq $it.cur) { continue }
        $done++
        Write-Host "  [$done/$total] $rel"
        if ($null -ne $it.stock -and $null -eq $it.bakSha -and $null -ne $it.stockSrc) {
            # first change of a stock file: back it up once (tmp + hash + rename); a VinceAudio exe: from its stock copy
            $h = Copy-ToTmp $it.stockSrc "$($it.bak).tmp" $it.stock $rel
            [IO.File]::Move("$($it.bak).tmp", $it.bak)
            if ((Hash $it.bak) -ne $h) { Refuse "${rel}: backup re-hash failed" }
            $it.bakSha = $h
            Step "backup $rel"
        }
        if ($null -ne $it.prior -and $it.cur -eq $it.prior -and $null -eq $it.pbakSha) {
            # first change of an earlier release's file: back it up once to prior\ (tmp + hash + rename)
            $h = Copy-ToTmp $it.full "$($it.pbak).tmp" $it.prior $rel
            [IO.File]::Move("$($it.pbak).tmp", $it.pbak)
            if ((Hash $it.pbak) -ne $h) { Refuse "${rel}: prior backup re-hash failed" }
            $it.pbakSha = $h
            Step "prior backup $rel"
        }
        if ($null -eq $it.target) {
            Clear-ReadOnly $it.full
            [IO.File]::Delete($it.full)
            Step "delete $rel"
            continue
        }
        $tmp = "$($it.full).tmp"
        if ($null -ne $it.prior -and $it.target -eq $it.prior) { [void](Copy-ToTmp $it.pbak $tmp $it.prior $rel) }
        elseif ($it.kind -eq 'ini') { [void](Bytes-ToTmp $newIni.bytes $tmp $it.target $rel) }
        elseif ($it.kind -ne 'game') { [void](Copy-ToTmp $it.pkg $tmp $it.target $rel) }
        elseif ($it.target -eq $it.stock) { [void](Copy-ToTmp $it.bak $tmp $it.stock $rel) }
        else {
            $src = $null
            if ($null -ne $it.stock) { $src = $it.bak }
            [void](Apply-Delta $src $it.delta.Path $it.delta $tmp $rel)
        }
        Step "tmp $rel"
        Put-InPlace $tmp $it.full
        if ((Hash $it.full) -ne $it.target) { Refuse "${rel}: re-hash after replace failed" }
        Step "replace $rel"
    }

    if ($Target) {
        $final = @()
        foreach ($it in $items) {
            $now = $it.target
            $own = @()
            if ($it.kind -eq 'ini') { $own = @($it.own) }
            elseif ($it.kind -ne 'migrate' -and $null -ne $now -and $now -ne $it.stock) { $own = @($now) }
            $final += [ordered]@{ game_path = $it.rel; stock_sha256 = $it.stock; result_sha256 = $now; owned = $own
                                  backup = $(if (Exists $it.bak) { $it.bakRel } else { $null }); prior_sha256 = $it.prior
                                  prior_backup = $(if (Exists $it.pbak) { $it.pbakRel } else { $null }) }
        }
        Write-Receipt $Game ([ordered]@{ backup_namespace = $Namespace; version = $M.version; state = 'done';
            set = $Target.set; options = @($Target.options); files = @($final) })
        Write-Host "Installed set $($Target.set) (options: $(if (@($Target.options).Count) { @($Target.options) -join ', ' } else { 'none' }))."
    } else {
        # back at stock (or the earlier release): remove the backups (each verified == stock / prior above), the folder,
        # then the receipt
        $priors = 0
        foreach ($it in $items) {
            if (Exists $it.bak) {
                if ((Hash $it.bak) -ne $it.stock) { Refuse "$($it.bakRel): changed during uninstall; left in place" }
                [IO.File]::Delete($it.bak)
            }
            if (Exists $it.pbak) {
                if ((Hash $it.pbak) -ne $it.prior) { Refuse "$($it.pbakRel): changed during uninstall; left in place" }
                [IO.File]::Delete($it.pbak)
            }
            if ($null -ne $it.prior) { $priors++ }
        }
        $root = [IO.Path]::Combine($Game, $BackupDir)
        if ([IO.Directory]::Exists($root)) {
            $dirs = @([IO.Directory]::GetDirectories($root, '*', [IO.SearchOption]::AllDirectories) | Sort-Object -Property Length -Descending)
            foreach ($dir in $dirs + @($root)) {
                if (@([IO.Directory]::GetFileSystemEntries($dir)).Count -eq 0) { [IO.Directory]::Delete($dir) }
            }
            if ([IO.Directory]::Exists($root)) { $notes += "${BackupDir}: holds files $Product did not write; left in place" }
        }
        [IO.File]::Delete($receiptPath)
        Step 'receipt removed'
        if ($priors) { Write-Host "Uninstalled: the game files are back to how they were before $Product (earlier releases restored)." }
        else { Write-Host 'Uninstalled: the game files are back to stock.' }
    }
    foreach ($n in $notes) { Write-Host "Note: $n" }
    Write-Host "Write steps: $($script:Steps)"
}

# ---------- main ----------
function Main {
    if ($Rest) { BadInput "unexpected arguments: $($Rest -join ' ')" }
    $act = ''
    if ($Action) { $act = $Action.ToLowerInvariant() }
    if (@('', 'status', 'install', 'change', 'uninstall') -notcontains $act) { BadInput "unknown action '$Action' (status, install, change, uninstall)" }
    if ($act -eq '') { $script:Interactive = $true }
    $m = Load-Manifest
    $game = Select-Game
    if ($act -eq '') {
        Show-Status $game $m
        $c = Ask 'Choose: [I]nstall, [C]hange options, [U]ninstall, [Q]uit:'
        switch -regex ($c) {
            '^[iIcC]' { Invoke-Plan $game $m (Ask-Options $m); return }
            '^[uU]' { Invoke-Plan $game $m $null; return }
            '^([qQ].*)?$' { Write-Host 'Nothing changed.'; return }
            default { BadInput "unknown choice '$c'" }
        }
    }
    if ($act -eq 'status') { Show-Status $game $m; return }
    if ($act -eq 'uninstall') { Invoke-Plan $game $m $null; return }
    if ($Options) { $target = Parse-Options $m $Options }
    elseif ($act -eq 'change') { BadInput 'change needs -Options <ids|none>' }
    else {
        $r = Read-Receipt $game
        if ($r -and $r.state -eq 'pending' -and $null -ne $r.target_set) {
            $target = @($m.sets | Where-Object { $_.set -eq $r.target_set })[0]
        } elseif ($r -and $null -ne $r.set) {
            $target = @($m.sets | Where-Object { $_.set -eq $r.set })[0]
        } else { $target = Ask-Options $m }
        if (-not $target) { Refuse "${ReceiptName}: names a set this package does not have" }
    }
    Invoke-Plan $game $m $target
}

try {
    Main
    exit 0
} catch {
    $code = $_.Exception.Data['code']
    if ($null -eq $code) { $code = 1; Write-Host "Unexpected error at VinceAIO.ps1 line $($_.InvocationInfo.ScriptLineNumber)." }
    $word = if ($code -eq 2) { 'Bad input' } else { 'Refused' }
    Write-Host "${word}: $($_.Exception.Message)"
    exit $code
}
