param(
    [Parameter(Mandatory=$true)][string]$Source,
    [string]$Name = '',
    [string]$LlvmBin = $env:AURORA_LLVM,
    [string]$OutputDirectory = 'build/apps',
    [string]$Image = ''
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (!$Name) { $Name = [IO.Path]::GetFileNameWithoutExtension($Source) }
if ($Name -notmatch '^[A-Za-z0-9_.-]{1,31}$') { throw 'Application name must be 1-31 letters, digits, dots, underscores or hyphens.' }
if (!$LlvmBin) {
    $compiler = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($compiler) { $LlvmBin = Split-Path $compiler.Source }
    else { $LlvmBin = 'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin' }
}
$nasm = Join-Path $PSScriptRoot 'tools/nasm-2.16.03/nasm.exe'
if (!(Test-Path $nasm)) { $nasm = (Get-Command nasm.exe -ErrorAction Stop).Source }
function Invoke-AppTool([string]$exe,[string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $LASTEXITCODE" }
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$flags = @('--target=x86_64-none-elf','-std=c11','-ffreestanding','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-fno-builtin','-O2','-Wall','-Wextra','-Werror','-I','sdk/include')
Invoke-AppTool $nasm @('-f','elf64','sdk/start.asm','-o',"$OutputDirectory/start.o")
Invoke-AppTool "$LlvmBin/clang.exe" ($flags + @('-c','sdk/runtime.c','-o',"$OutputDirectory/runtime.o"))
Invoke-AppTool "$LlvmBin/clang.exe" ($flags + @('-c','src/user/lib.c','-o',"$OutputDirectory/lib.o"))
Invoke-AppTool "$LlvmBin/clang.exe" ($flags + @('-c',$Source,'-o',"$OutputDirectory/$Name.o"))
Invoke-AppTool "$LlvmBin/ld.lld.exe" @('-nostdlib','-z','max-page-size=4096','-T','sdk/linker.ld',"$OutputDirectory/start.o","$OutputDirectory/$Name.o","$OutputDirectory/runtime.o","$OutputDirectory/lib.o",'-o',"$OutputDirectory/$Name.elf")
if ((Get-Item "$OutputDirectory/$Name.elf").Length -gt 65536) { throw 'Executable exceeds the initial 64 KiB file limit.' }
if ($Image) {
    $files = @{}; $files[$Name] = "$OutputDirectory/$Name.elf"
    & "$PSScriptRoot/pack-files.ps1" -Image $Image -Files $files
}
Write-Host "Built C application: $OutputDirectory/$Name.elf"
