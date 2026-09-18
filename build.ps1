param([string]$LlvmBin = $env:AURORA_LLVM, [switch]$SelfTest)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (!$LlvmBin) {
    $compiler = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($compiler) { $LlvmBin = Split-Path $compiler.Source }
    else { $LlvmBin = 'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin' }
}
$nasm = Join-Path $PSScriptRoot 'tools/nasm-2.16.03/nasm.exe'
if (!(Test-Path $nasm)) { $nasm = (Get-Command nasm.exe -ErrorAction Stop).Source }
function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $LASTEXITCODE" }
}
$output = if ($SelfTest) { 'build/selftest' } else { 'build' }
New-Item -ItemType Directory -Force $output | Out-Null
$flags = @('--target=x86_64-none-elf','-std=c11','-ffreestanding','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-fno-builtin','-O2','-Wall','-Wextra','-Werror')
Invoke-Checked $nasm @('-f','bin','src/boot.asm','-o',"$output/boot.bin")
Invoke-Checked $nasm @('-f','bin','src/loader.asm','-o',"$output/loader.bin")
Invoke-Checked $nasm @('-f','elf64','src/entry.asm','-o',"$output/entry.o")
Invoke-Checked $nasm @('-f','elf64','src/traps.asm','-o',"$output/traps.o")
Invoke-Checked $nasm @('-f','elf64','src/user/start.asm','-o',"$output/user-start.o")
Invoke-Checked "$LlvmBin/clang.exe" ($flags + @('-c','src/user/lib.c','-o',"$output/user-lib.o"))
$services = @('desktop','input','display')
if ($SelfTest) { $services += 'probe' }
$header = @('/* Generated from the separately linked user binaries. */')
$bundle = @('bits 64','section .rodata.images')
foreach ($service in $services) {
    Invoke-Checked "$LlvmBin/clang.exe" ($flags + @('-c',"src/user/$service.c",'-o',"$output/$service.o"))
    Invoke-Checked "$LlvmBin/ld.lld.exe" @('-nostdlib','-T','src/user/linker.ld',"$output/user-start.o", "$output/$service.o", "$output/user-lib.o",'-o',"$output/$service.elf")
    Invoke-Checked "$LlvmBin/llvm-objcopy.exe" @('-O','binary',"$output/$service.elf", "$output/$service.bin")
    $symbols = & "$LlvmBin/llvm-nm.exe" -n "$output/$service.elf"
    if ($LASTEXITCODE -ne 0) { throw 'Failed to read user image symbols' }
    foreach ($boundary in @('text_end','ro_end')) {
        $match = $symbols | Where-Object { $_ -match " __$boundary`$" }
        if (!$match) { throw "Missing boundary: $boundary" }
        $address = ($match -split '\s+')[0]
        $header += "#define $($service.ToUpper())_$($boundary.ToUpper()) 0x${address}ULL"
    }
    $length = (Get-Item "$output/$service.bin").Length
    $header += "extern const u8 ${service}_image[];"
    $header += "#define ${service}_image_size ${length}ULL"
    $bundle += @('align 16',"global ${service}_image", "${service}_image:", "incbin `"$output/$service.bin`"")
}
[IO.File]::WriteAllLines("$PSScriptRoot/$output/images.h",$header)
[IO.File]::WriteAllLines("$PSScriptRoot/$output/images.asm",$bundle)
Invoke-Checked $nasm @('-f','elf64',"$output/images.asm",'-o',"$output/images.o")
. "$PSScriptRoot/build-filesystem.ps1"
$kernelFlags = $flags + $fsFlags + @('-I',$output)
if ($SelfTest) { $kernelFlags += '-DAURORA_SELF_TEST=1' }
Invoke-Checked "$LlvmBin/clang.exe" ($kernelFlags + @('-c','src/kernel.c','-o',"$output/kernel.o"))
Invoke-Checked "$LlvmBin/ld.lld.exe" (@('-nostdlib','--gc-sections','-T','src/linker.ld',"$output/entry.o", "$output/traps.o", "$output/kernel.o", "$output/images.o") + $fsObjects + @('-o',"$output/kernel.elf"))
Invoke-Checked "$LlvmBin/llvm-objcopy.exe" @('-O','binary',"$output/kernel.elf", "$output/kernel.bin")
$kernel = [IO.File]::ReadAllBytes("$PSScriptRoot/$output/kernel.bin")
if ($kernel.Length -gt 245760) { throw 'Kernel + service bundle exceeds loader limit of 480 sectors.' }
$imagePath = "$PSScriptRoot/$output/aurora.img"
# Preserve filesystem contents across kernel builds. File sharing rejects a
# running QEMU instance rather than corrupting its disk.
$image = if (Test-Path $imagePath) { [IO.File]::ReadAllBytes($imagePath) } else { New-Object byte[] (16 * 1024 * 1024) }
if ($image.Length -ne 16777216) { throw 'Existing disk image is not 16 MiB.' }
[Array]::Clear($image,0,512*512)
[IO.File]::ReadAllBytes("$PSScriptRoot/$output/boot.bin").CopyTo($image, 0)
[IO.File]::ReadAllBytes("$PSScriptRoot/$output/loader.bin").CopyTo($image, 512)
$kernel.CopyTo($image, 4608)
[IO.File]::WriteAllBytes("$PSScriptRoot/$output/aurora.img", $image)
$appFiles = @{}
foreach ($appName in @('hello','calc','filedemo')) {
    & "$PSScriptRoot/build-app.ps1" -Source "apps/$appName.c" -Name $appName -LlvmBin $LlvmBin -OutputDirectory "$output/apps"
    $appFiles[$appName] = "$output/apps/$appName.elf"
}
$appFiles['readme.txt'] = 'apps/readme.txt'
& "$PSScriptRoot/pack-files.ps1" -Image "$output/aurora.img" -Files $appFiles
Write-Host "Built Aurora microkernel + user services: $($kernel.Length) bytes, $output/aurora.img"
