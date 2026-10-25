param([string]$OutputDirectory = "dist")
$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    $destination = [IO.Path]::GetFullPath($OutputDirectory)
} else {
    $destination = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
}
function Invoke-Checked {
    param([string]$Program, [string[]]$CommandArgs)
    & $Program @CommandArgs
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location $projectRoot
try {
    Invoke-Checked cmake @("-S", ".", "-B", "build", "-G", "Visual Studio 17 2022", "-A", "x64")
    Invoke-Checked cmake @("--build", "build", "--config", "Release", "--parallel")
    Invoke-Checked cmake @("--install", "build", "--config", "Release", "--prefix", $destination)
    Invoke-Checked dotnet @("publish", "GUI/ImageBatchProcessorGui.csproj", "-c", "Release",
        "-r", "win-x64", "--self-contained", "true", "-p:PublishSingleFile=false",
        "-o", (Join-Path $destination "gui"))
    # CMake installs VC++/OpenMP redistribution files into bin.
    Get-ChildItem (Join-Path $destination "bin") -Filter "*.dll" | Copy-Item -Destination (Join-Path $destination "gui") -Force
    $shaderDestination = Join-Path $destination "gui/shaders"
    New-Item -ItemType Directory -Force -Path $shaderDestination | Out-Null
    Get-ChildItem (Join-Path $destination "bin/shaders") -Filter "*.spv" | Copy-Item -Destination $shaderDestination -Force

    # Preserve notices from the exact packages resolved by dotnet publish.
    $assets = Get-Content (Join-Path $projectRoot "GUI/obj/project.assets.json") -Raw | ConvertFrom-Json
    $packageRoots = @($assets.packageFolders.PSObject.Properties.Name)
    $runtimeConfig = Get-Content (Join-Path $destination "gui/ImageBatchProcessorGui.runtimeconfig.json") -Raw | ConvertFrom-Json
    $runtimePackages = @($runtimeConfig.runtimeOptions.includedFrameworks | ForEach-Object {
        $_.name + ".Runtime.win-x64/" + $_.version
    })
    if ($runtimePackages.Count -ne 2) { throw "Expected both .NET runtime package identities" }
    foreach ($identity in $runtimePackages) {
        $packagePath = $null
        foreach ($packageRoot in $packageRoots) {
            $candidate = Join-Path $packageRoot $identity.ToLowerInvariant()
            if (Test-Path -LiteralPath $candidate) { $packagePath = $candidate; break }
        }
        if (-not $packagePath) { throw "Runtime package missing: $identity" }
        $noticeDestination = Join-Path $destination ("licenses/dotnet/" + $identity.Replace("/", "-"))
        New-Item -ItemType Directory -Force -Path $noticeDestination | Out-Null
        $notices = @(Get-ChildItem -LiteralPath $packagePath -File | Where-Object {
            $_.Name -match 'LICENSE|THIRD.PARTY.NOTICES'
        })
        if ($notices.Count -eq 0) { throw "Runtime notices missing: $identity" }
        $notices | Copy-Item -Destination $noticeDestination -Force
    }
    Write-Host "Distribution ready: $destination"
} finally {
    Pop-Location
}
