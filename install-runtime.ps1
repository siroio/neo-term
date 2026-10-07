param()
$ErrorActionPreference = 'Stop'
$version = '1.25.260930003'
$expectedHash = '02B07B349AF66D801159BDF9E440D4A1CE78BB951F37FC8609731665AFDAE7EE'
$destination = Join-Path $PSScriptRoot 'build/runtime'
$archivePath = Join-Path $PSScriptRoot "build/conpty.$version.nupkg"
New-Item -ItemType Directory -Force -Path $destination | Out-Null
if (-not (Test-Path -LiteralPath $archivePath)) {
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/$version/microsoft.windows.console.conpty.$version.nupkg"
    Invoke-WebRequest -Uri $url -OutFile $archivePath
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'The ConPTY package SHA256 does not match the pinned package.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $files = @{
        'runtimes/win-x64/native/conpty.dll' = 'conpty.dll'
        'build/native/runtimes/x64/OpenConsole.exe' = 'OpenConsole.exe'
    }
    foreach ($entry in $files.GetEnumerator()) {
        $source = $archive.GetEntry($entry.Key)
        if (-not $source) {
            throw "Missing ConPTY package file: $($entry.Key)"
        }

        $destinationPath = Join-Path $destination $entry.Value
        [IO.Compression.ZipFileExtensions]::ExtractToFile($source, $destinationPath, $true)
    }
} finally {
    $archive.Dispose()
}
Write-Output "CONPTY_RUNTIME=$version ($destination)"
