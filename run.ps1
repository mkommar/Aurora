param([switch]$Headless, [switch]$NoBuild, [switch]$SelfTest, [switch]$NativeGcc, [switch]$Minimal)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (!$NoBuild) { & "$PSScriptRoot/build.ps1" -SelfTest:$SelfTest }
$output = if ($SelfTest) { 'build/selftest' } else { 'build' }
$qemu = Join-Path $PSScriptRoot 'tools/qemu/qemu-system-x86_64.exe'
if (!(Test-Path $qemu)) { $qemu = (Get-Command qemu-system-x86_64.exe -ErrorAction Stop).Source }
$arguments = @('-name','Aurora OS','-machine','pc','-accel','tcg','-cpu','qemu64','-m','128M','-vga','std','-drive',"format=raw,file=$output/aurora.img",'-serial',"file:$output/serial.log",'-net','none','-qmp','tcp:127.0.0.1:4444,server=on,wait=off')
if (!$NativeGcc -and !$Minimal -and !$SelfTest -and (Test-Path 'build/development.img')) {
    $arguments[$arguments.IndexOf('128M')] = '1G'
    $arguments += @('-drive','format=raw,file=build/development.img,if=none,id=development','-device','virtio-blk-pci,drive=development,disable-modern=on')
}
elseif ($NativeGcc -or (!$Minimal -and !$SelfTest -and (Test-Path 'build/toolchain.img'))) {
    if (!(Test-Path 'build/toolchain.img')) { throw 'Run setup-native-gcc.py first to create the native compiler disk.' }
    $arguments[$arguments.IndexOf('128M')] = '1G'
    $arguments += @('-drive','format=raw,file=build/toolchain.img,if=ide,index=1')
}
if ($Headless) { $arguments += @('-display','none') }
else { $arguments += @('-display','gtk') }
& $qemu @arguments
if ($LASTEXITCODE -ne 0) { throw "QEMU exited with code $LASTEXITCODE" }
