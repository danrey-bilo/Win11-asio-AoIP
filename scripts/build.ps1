param([Parameter(Mandatory)][string]$AsioSdkDir, [string]$ToolchainBin = $env:PIAOIP_TOOLCHAIN, [string]$AoipSourceDir, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $repo 'build/windows' }
$cmakeArgs = @('-S',$repo,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DASIO_SDK_DIR=$AsioSdkDir")
if ($ToolchainBin) {
  $ToolchainBin = (Resolve-Path $ToolchainBin).Path.Replace('\','/')
  $cmakeArgs += "-DCMAKE_CXX_COMPILER=$ToolchainBin/x86_64-w64-mingw32-clang++.exe"
  $cmakeArgs += "-DCMAKE_RC_COMPILER=$ToolchainBin/x86_64-w64-mingw32-windres.exe"
} else { throw 'Set -ToolchainBin or PIAOIP_TOOLCHAIN to the LLVM-MinGW x64/UCRT bin directory.' }
if ($AoipSourceDir) { $cmakeArgs += "-DAOIP_SOURCE_DIR=$AoipSourceDir" }
& cmake @cmakeArgs
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $build --parallel 2
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $build --output-on-failure --timeout 30
if ($LASTEXITCODE) { throw 'Tests failed' }
Write-Output "Built in $build/bin"
