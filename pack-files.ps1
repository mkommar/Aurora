param([Parameter(Mandatory=$true)][string]$Image,[Parameter(Mandatory=$true)][hashtable]$Files,[switch]$SelfTest)
$ErrorActionPreference = 'Stop'
# Open exclusively before reading: refuse to modify a disk used by QEMU.
$path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Image)
$stream = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    $imageBytes = if ($SelfTest) { 32 * 1024 * 1024 } else { 16 * 1024 * 1024 }
    $fsLba = if ($SelfTest) { 1024 } else { 512 }
    if ($stream.Length -ne $imageBytes) { throw "Expected a $($imageBytes / 1MB) MiB Aurora disk image." }
    if ($SelfTest -and $fsLba -le 600) { throw 'Filesystem metadata must follow the loader boundary.' }
    $dataEnd = ($fsLba + 8 + 32 * 128) * 512
    if ($dataEnd -gt $imageBytes) { throw 'Filesystem extents exceed the image boundary.' }
    $bytes = New-Object byte[] $imageBytes
    $offset=0
    while ($offset -lt $bytes.Length) { $count=$stream.Read($bytes,$offset,$bytes.Length-$offset); if (!$count) { throw 'Short disk image read' }; $offset += $count }
    $magic = [Text.Encoding]::ASCII.GetBytes("AURFS01`0")
    $base = $fsLba*512
    $existing = [Text.Encoding]::ASCII.GetString($bytes,$base,8)
    if ($existing -ne "AURFS01`0") {
        for ($i=$base;$i -lt $dataEnd;$i++) { if ($bytes[$i] -ne 0) { throw 'Unknown filesystem; refusing to overwrite it.' } }
        $magic.CopyTo($bytes,$base)
    }
    foreach ($name in $Files.Keys) {
        if ($name -notmatch '^[A-Za-z0-9_.-]{1,31}$') { throw "Invalid filename: $name" }
        $data=[IO.File]::ReadAllBytes($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Files[$name]))
        if ($data.Length -gt 65536) { throw "File exceeds 64 KiB: $name" }
        $slot=-1; $empty=-1
        for ($i=0;$i -lt 32;$i++) {
            $entry=($fsLba+1)*512+$i*64
            $used=[BitConverter]::ToUInt32($bytes,$entry+36)
            $size=[BitConverter]::ToUInt32($bytes,$entry+32)
            if ($used -gt 1 -or $size -gt 65536) { throw 'Invalid filesystem metadata' }
            if (!$used -and $empty -lt 0) { $empty=$i }
            $stored=[Text.Encoding]::ASCII.GetString($bytes,$entry,32).Split([char]0)[0]
            if ($used -and $stored -ceq $name) { $slot=$i }
        }
        if ($slot -lt 0) { $slot=$empty }
        if ($slot -lt 0) { throw 'Filesystem directory is full.' }
        $entry=($fsLba+1)*512+$slot*64
        [Array]::Clear($bytes,$entry,64)
        [Text.Encoding]::ASCII.GetBytes($name).CopyTo($bytes,$entry)
        [BitConverter]::GetBytes([uint32]$data.Length).CopyTo($bytes,$entry+32)
        [BitConverter]::GetBytes([uint32]1).CopyTo($bytes,$entry+36)
        $dataOffset=($fsLba+8+$slot*128)*512
        if ($dataOffset + 65536 -gt $imageBytes) { throw 'Filesystem file extent exceeds image boundary.' }
        [Array]::Clear($bytes,$dataOffset,65536)
        $data.CopyTo($bytes,$dataOffset)
    }
    $stream.Position=$base; $stream.Write($bytes,$base,$bytes.Length-$base); $stream.Flush($true)
} finally { $stream.Dispose() }
