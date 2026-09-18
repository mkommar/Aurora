$lwip = 'third_party/lwip-STABLE-2_2_1_RELEASE'
$netFlags = @("-I$lwip/src/include",'-Isrc/network_platform','-Isrc/fs_platform','-Isrc','-Oz','-ffunction-sections','-fdata-sections')
$netObjects = @()
$netSources = @(Get-ChildItem "$lwip/src/core/*.c") + @(Get-ChildItem "$lwip/src/core/ipv4/*.c") + @(Get-Item "$lwip/src/netif/ethernet.c",'src/network.c')
foreach ($source in $netSources) {
    $object = "$output/net-$($source.BaseName).o"
    Invoke-Checked "$LlvmBin/clang.exe" (($flags | Where-Object { $_ -ne '-Werror' }) + $netFlags + @('-c',$source.FullName,'-o',$object))
    $netObjects += $object
}
