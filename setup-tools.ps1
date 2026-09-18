# Download pinned portable tools into this workspace; no system installation.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force tools | Out-Null
function Download-Checked($Url, $Path, $Hash) {
    if (!(Test-Path $Path)) { Invoke-WebRequest $Url -OutFile $Path }
    if ((Get-FileHash $Path -Algorithm SHA256).Hash -ne $Hash) { throw "Checksum mismatch: $Path" }
}
Download-Checked 'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip' 'tools/nasm.zip' '3EE4782247BCB874378D02F7EAB4E294A84D3D15F3F6EE2DE2F47A46AA7226E6'
Download-Checked 'https://qemu.weilnetz.de/w64/2026/qemu-w64-setup-20260422.exe' 'tools/qemu.exe' 'DA95183368398B651ACDB9236F033A76175C9D1298DD90E6C7836F528A63B27D'
Expand-Archive tools/nasm.zip tools -Force
$sevenzip = 'C:\Program Files\7-Zip\7z.exe'
if (!(Test-Path $sevenzip)) { $sevenzip = (Get-Command 7z.exe -ErrorAction Stop).Source }
& $sevenzip x tools/qemu.exe '-otools/qemu' -y
if ($LASTEXITCODE -ne 0) { throw 'QEMU extraction failed' }
Write-Host 'Portable NASM and QEMU are ready. Building also requires LLVM (clang, ld.lld, llvm-objcopy).'
