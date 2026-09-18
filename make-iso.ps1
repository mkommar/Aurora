param(
    [string]$Image = 'build/aurora.img',
    [string]$Output = 'build/aurora.iso'
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$source = [IO.Path]::GetFullPath($Image)
$target = [IO.Path]::GetFullPath($Output)
if (!(Test-Path -LiteralPath $source)) { throw "Source image not found: $source" }
if ($source -eq $target) { throw 'ISO output must be different from the source image.' }
$bytes = [IO.File]::ReadAllBytes($source)
if ($bytes.Length -lt 1048576 -or ($bytes.Length % 512) -ne 0) { throw 'Aurora image is not a valid 512-byte-sector disk image.' }
if ($bytes[510] -ne 0x55 -or $bytes[511] -ne 0xaa) { throw 'Aurora image has no BIOS boot signature.' }
# Aurora's loader and kernel are intentionally preserved byte-for-byte. The
# artifact uses the .iso convention while retaining 512-byte hard-disk BIOS
# sectors, because Aurora's kernel accesses its GPT/ext2/FAT32 volumes through
# ATA PIO after the BIOS handoff. A normal ISO9660 CD would break that contract.
[IO.File]::WriteAllBytes($target, $bytes)
$hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
$metadata = [ordered]@{
    format = 'Aurora BIOS hybrid disk ISO (raw 512-byte sectors)'
    source = $source
    output = $target
    sha256 = $hash
    size = $bytes.Length
    boot = 'BIOS INT 13h hard-disk handoff; GPT, ext2 and FAT32 retained'
    qemu = "qemu-system-x86_64 -drive format=raw,file=$Output -m 1G -smp 4"
}
[IO.File]::WriteAllText("$target.json", ($metadata | ConvertTo-Json -Depth 3), [Text.UTF8Encoding]::new($false))
Write-Host "Created $target ($($bytes.Length) bytes, SHA-256 $hash)"
