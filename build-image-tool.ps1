$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$llvm = 'C:\Program Files\Unity\Hub\Editor\6000.4.0f1\Editor\Data\PlaybackEngines\AndroidPlayer\NDK\toolchains\llvm\prebuilt\windows-x86_64\bin'
$ext4 = 'third_party/lwext4-58bcf89a121b72d4fb66334f1693d3b30e4cb9c5'
$out = 'build/image-tool'
New-Item -ItemType Directory -Force $out | Out-Null
@'
LIBRARY msvcrt.dll
EXPORTS
malloc
calloc
realloc
free
memcpy
memset
memcmp
memmove
strlen
strcmp
strncmp
strcpy
strncpy
strchr
qsort
printf
'@ | Set-Content "$out/msvcrt.def"
& "$llvm/llvm-dlltool.exe" -m i386:x86-64 -d "$out/msvcrt.def" -l "$out/msvcrt.lib"
if ($LASTEXITCODE) { throw 'Import library failed' }
$flags = @('--target=x86_64-pc-windows-msvc','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-O2','-DCONFIG_USE_DEFAULT_CFG=1','-DCONFIG_EXT_FEATURE_SET_LVL=2','-DCONFIG_JOURNALING_ENABLE=0','-DCONFIG_XATTR_ENABLE=1','-DCONFIG_EXTENTS_ENABLE=0','-DCONFIG_HAVE_OWN_ERRNO=1','-DCONFIG_DEBUG_PRINTF=0','-DCONFIG_DEBUG_ASSERT=0',"-I$ext4/include",'-Isrc/fs_platform')
$objects = @()
$flags += @('-Ithird_party/fatfs-r016/source','-U_WIN32','-U_WIN64')
$sources = @(Get-ChildItem "$ext4/src/*.c" | Where-Object { $_.BaseName -notin @('ext4_extent','ext4_mbr') }) + @(Get-Item 'tools-source/ext2-image.c','tools-source/fat-image.c','third_party/fatfs-r016/source/ff.c','third_party/fatfs-r016/source/ffunicode.c')
foreach ($source in $sources) {
    $object = "$out/$($source.BaseName).obj"
    & "$llvm/clang.exe" @flags -ffunction-sections -fdata-sections -c $source.FullName -o $object
    if ($LASTEXITCODE) { throw "Failed $source" }
    $objects += $object
}
& "$llvm/ld.lld.exe" -flavor link /dll /noentry /nodefaultlib /opt:ref "/out:$out/ext2-image.dll" @objects "$out/msvcrt.lib"
if ($LASTEXITCODE) { throw 'Image library link failed' }

