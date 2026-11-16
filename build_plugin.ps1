param(
    [string]$MetaHookDir = "",
    [string]$Configuration = ""
)

$ErrorActionPreference = "Stop"
$projectDir = $PSScriptRoot

# Resolve MetaHook SDK directory
if (-not $MetaHookDir) {
    if ($env:METAHOOK_SOURCE_PATH) {
        $MetaHookDir = $env:METAHOOK_SOURCE_PATH
    } elseif (Test-Path "$projectDir\..\..\MetaHook\include\metahook.h") {
        $MetaHookDir = (Resolve-Path "$projectDir\..\..\MetaHook").Path
    } elseif (Test-Path "$projectDir\..\..\include\metahook.h") {
        $MetaHookDir = (Resolve-Path "$projectDir\..\..").Path
    } else {
        $resolved = Resolve-Path "$projectDir\..\..\MetaHook" -ErrorAction SilentlyContinue
        if ($resolved) {
            $MetaHookDir = $resolved.Path
        } else {
            $MetaHookDir = "$projectDir\..\..\MetaHook"
        }
    }
} else {
    $MetaHookDir = (Resolve-Path $MetaHookDir).Path
}

# Find MSBuild via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) not found at: $vswhere"
}
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild.exe not found via vswhere"
}

Write-Host "Using MSBuild:   $msbuild" -ForegroundColor Cyan
Write-Host "ProjectDir:      $projectDir" -ForegroundColor Cyan
Write-Host "MetaHookDir:     $MetaHookDir" -ForegroundColor Cyan

$configs = if ($Configuration) { @($Configuration) } else { @("Release") }

foreach ($config in $configs) {
    Write-Host "`n>>> Building VoiceExtension [$config | Win32]..." -ForegroundColor Yellow
    
    & $msbuild "$projectDir\VoiceExtension.vcxproj" /t:Rebuild "/p:Configuration=$config" /p:Platform=Win32 "/p:MetaHookDir=$MetaHookDir" "/p:SolutionDir=$projectDir\" /m /v:minimal /nologo
    
    if ($LASTEXITCODE -ne 0) {
        throw "VoiceExtension build failed for configuration: $config"
    }
    
    $outDll = Join-Path $projectDir "$config\VoiceExtension.dll"
    
    if (Test-Path $outDll) {
        $item = Get-Item $outDll
        Write-Host ">>> Built: $($item.FullName) ($($item.Length) bytes, $($item.LastWriteTime))" -ForegroundColor Green
    }
}

Write-Host "`nAll requested configurations built successfully!" -ForegroundColor Green
