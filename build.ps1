param(
    [ValidateSet('x86','x64')][string]$Arch='x86',
    [string]$OutputDirectory='',
    [switch]$Tests,
    [switch]$RegenerateIcons,
    [string]$SigningCertificateThumbprint='',
    [string]$TimestampServer='http://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Requires Visual Studio C++ Build Tools.' }
$vc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'Include') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object Name -Descending | Select-Object -First 1
if (!$sdk) { throw 'Requires Windows SDK.' }
$env:INCLUDE = @((Join-Path $vc.FullName 'include'), (Join-Path $sdk.FullName 'ucrt'), (Join-Path $sdk.FullName 'shared'), (Join-Path $sdk.FullName 'um'), (Join-Path $sdk.FullName 'winrt')) -join ';'
$env:LIB = @((Join-Path $vc.FullName "lib\$Arch"), (Join-Path $kitRoot "Lib\$($sdk.Name)\ucrt\$Arch"), (Join-Path $kitRoot "Lib\$($sdk.Name)\um\$Arch")) -join ';'
$bin = Join-Path $vc.FullName "bin\Hostx64\$Arch"
$env:PATH = "$bin;$env:PATH"
$out = if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $root 'dist'}
$obj = Join-Path $root "build\$Arch"
$product = if($Tests){'XiaomaBrightnessTest.exe'}else{'Xiaoma Brightness.exe'}
New-Item -ItemType Directory -Path $out,$obj -Force | Out-Null
Push-Location $root
try {
    $iconSources=@('src\app-icon.svg','src\tray-mark.svg') | ForEach-Object {Join-Path $root $_}
    $iconOutputs=@('src\app.ico','src\tray-dark.ico','src\tray-light.ico','src\app-icon.png') | ForEach-Object {Join-Path $root $_}
    $iconScript=Join-Path $root 'scripts\make-icons.py'
    $iconSourceTime=@(($iconSources | ForEach-Object {(Get-Item -LiteralPath $_).LastWriteTimeUtc}),(Get-Item -LiteralPath $iconScript).LastWriteTimeUtc) | ForEach-Object {$_} | Sort-Object -Descending | Select-Object -First 1
    $iconsStale=@($iconOutputs | Where-Object {!(Test-Path -LiteralPath $_) -or (Get-Item -LiteralPath $_).LastWriteTimeUtc -lt $iconSourceTime}).Count -gt 0
    if($RegenerateIcons -or $iconsStale){
        & python $iconScript
        if($LASTEXITCODE -ne 0){throw 'Icon export failed; installPillowandresvg-py.'}
    }
    & (Join-Path $kitRoot "bin\$($sdk.Name)\x64\rc.exe") /nologo /fo (Join-Path $obj 'app.res') 'src\app.rc'
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    $args = @('/nologo','/std:c++17','/utf-8','/W4','/O1','/MT','/EHsc','/GS','/GL','/DNDEBUG','/DUNICODE','/D_UNICODE','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX',"/Fo$obj\", "src\popup.cpp","src\display.cpp","src\channel.cpp")
    if($Tests){$args+=@('/DXIAOMA_TEST_BUILD','src\stress.cpp')}
    $args+=@((Join-Path $obj 'app.res'),"/Fe$out\$product",'/link','/SUBSYSTEM:WINDOWS','/LTCG','/OPT:REF','/OPT:ICF','/DYNAMICBASE','/NXCOMPAT','user32.lib','gdi32.lib','shell32.lib','shlwapi.lib','comctl32.lib','dwmapi.lib','d2d1.lib','dwrite.lib','dxva2.lib','ole32.lib','oleaut32.lib','wbemuuid.lib','advapi32.lib','uuid.lib','imm32.lib')
    if($Arch -eq 'x64'){$args+='/HIGHENTROPYVA'}
    if($Tests){$args+='psapi.lib'}
    & (Join-Path $bin 'cl.exe') @args
    if ($LASTEXITCODE -ne 0) { throw 'Application compilation failed' }
    if($SigningCertificateThumbprint){
        $thumbprint=$SigningCertificateThumbprint -replace '\s',''
        if($thumbprint -notmatch '^[0-9A-Fa-f]{40}$'){throw 'Invalid signing certificate thumbprint.'}
        $certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$thumbprint" -ErrorAction Stop
        if(!$certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date) -or !(@($certificate.EnhancedKeyUsageList.ObjectId) -contains '1.3.6.1.5.5.7.3.3')){throw 'A valid code-signing certificate and private key are required for the current user.'}
        $signTool=Join-Path $kitRoot "bin\$($sdk.Name)\x64\signtool.exe"
        & $signTool sign /sha1 $thumbprint /fd SHA256 /tr $TimestampServer /td SHA256 /d 'Xiaoma Brightness' (Join-Path $out $product)
        if($LASTEXITCODE -ne 0){throw 'Code signing failed.'}
        & $signTool verify /pa (Join-Path $out $product)
        if($LASTEXITCODE -ne 0 -or (Get-AuthenticodeSignature -LiteralPath (Join-Path $out $product)).Status -ne 'Valid'){throw 'Signature trust verification failed.'}
    }
    Get-Item -LiteralPath (Join-Path $out $product) | Select-Object FullName,Length
} finally { Pop-Location }
