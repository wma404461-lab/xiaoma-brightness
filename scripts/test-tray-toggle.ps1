param([ValidateSet('x86','x64')][string]$Arch='x86')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vc=Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kitRoot=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk=Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$env:INCLUDE=@((Join-Path $vc.FullName 'include'),(Join-Path $sdk.FullName 'ucrt'),(Join-Path $sdk.FullName 'shared'),(Join-Path $sdk.FullName 'um')) -join ';'
$env:LIB=@((Join-Path $vc.FullName "lib\$Arch"),(Join-Path $kitRoot "Lib\$($sdk.Name)\ucrt\$Arch"),(Join-Path $kitRoot "Lib\$($sdk.Name)\um\$Arch")) -join ';'
$taskOutput=Join-Path $taskRoot "build\tray-test-$Arch"
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskExe=Join-Path $taskOutput 'tray-toggle-test.exe'
& (Join-Path $vc.FullName "bin\Hostx64\$Arch\cl.exe") /nologo /std:c++17 /W4 /O1 /MT /EHsc (Join-Path $PSScriptRoot 'tray-toggle-test.cpp') "/Fo$taskOutput\" "/Fe$taskExe" /link /SUBSYSTEM:CONSOLE
if($LASTEXITCODE -ne 0){throw 'Tray regression compilation failed.'}
$result=& $taskExe
$result | Set-Content -LiteralPath (Join-Path $taskRoot "build\tray-toggle-$Arch.json") -Encoding utf8
$result
if($LASTEXITCODE -ne 0){throw 'Tray regression failed.'}
