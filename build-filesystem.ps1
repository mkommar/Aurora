# Included by build.ps1; compile the pinned library with ext2-only features.
$ext4 = 'third_party/lwext4-58bcf89a121b72d4fb66334f1693d3b30e4cb9c5'
$fsFlags = @('-Oz','-DCONFIG_USE_DEFAULT_CFG=1','-DCONFIG_EXT_FEATURE_SET_LVL=2','-DCONFIG_JOURNALING_ENABLE=0','-DCONFIG_XATTR_ENABLE=0','-DCONFIG_EXTENTS_ENABLE=0','-DCONFIG_HAVE_OWN_ERRNO=1','-DCONFIG_DEBUG_PRINTF=0','-DCONFIG_DEBUG_ASSERT=0',"-I$ext4/include",'-Isrc/fs_platform')
$fsObjects = @()
foreach ($source in (Get-ChildItem "$ext4/src/*.c" | Where-Object { $_.BaseName -notin @('ext4_mkfs','ext4_mbr','ext4_xattr','ext4_extent') })) {
    $object = "$output/$($source.BaseName).o"
    Invoke-Checked "$LlvmBin/clang.exe" (($flags | Where-Object { $_ -ne '-Werror' }) + $fsFlags + @('-Wno-unused-function','-Wno-unused-parameter','-ffunction-sections','-fdata-sections','-c',$source.FullName,'-o',$object))
    $fsObjects += $object
}
Invoke-Checked "$LlvmBin/clang.exe" ($flags + $fsFlags + @('-c','src/fs_platform/runtime.c','-o',"$output/fs-runtime.o"))
$fsObjects += "$output/fs-runtime.o"
$fsFlags += @('-Ithird_party/fatfs-r016/source','-DFF_FS_NORTC=0')  # the kernel supplies get_fattime()
foreach ($name in @('ff','ffunicode')) {
    Invoke-Checked "$LlvmBin/clang.exe" (($flags | Where-Object { $_ -ne '-Werror' }) + $fsFlags + @('-ffunction-sections','-fdata-sections','-c',"third_party/fatfs-r016/source/$name.c",'-o',"$output/$name.o"))
    $fsObjects += "$output/$name.o"
}
