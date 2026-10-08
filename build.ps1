param(
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$output = Join-Path $root 'build'

New-Item -ItemType Directory -Force -Path $output | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'

    if (-not (Test-Path -LiteralPath $locator)) {
        throw 'Visual Studio C++ build tools are required.'
    }

    $locatorArguments = @(
        '-latest'
        '-products', '*'
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
        '-property', 'installationPath'
    )
    $installation = & $locator @locatorArguments

    if (-not $installation) {
        throw 'Install the Visual Studio Desktop development with C++ workload.'
    }

    $setup = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $environmentScript = Join-Path $output 'build-env.cmd'
    $environmentCommands = @(
        "@call `"$setup`" -arch=x64 -host_arch=x64 >nul"
        '@if errorlevel 1 exit /b 1'
        '@set'
    )
    $environmentCommands | Set-Content -LiteralPath $environmentScript -Encoding ascii

    & $env:ComSpec /d /c $environmentScript | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

Push-Location $output

try {
    $flags = @(
        '/nologo'
        '/O2'
        '/utf-8'
        '/MT'
        '/DUNICODE'
        '/D_UNICODE'
        '/DNOMINMAX'
        '/DWIN32_LEAN_AND_MEAN'
        '/D_CRT_SECURE_NO_WARNINGS'
        '/D_WIN32_WINNT=0x0A00'
    )

    & cl.exe @flags /std:c++17 /EHsc /W4 /WX /I"$root/native" "$root/tests/native-test.cpp" /Fe:native-test.exe

    if ($LASTEXITCODE -ne 0) {
        throw 'Native test compilation failed.'
    }

    if (Test-Path -LiteralPath "$root/native/host.cpp") {
        $include = "/I$root/vendor/libvterm/include"
        $sources = Get-ChildItem -LiteralPath "$root/vendor/libvterm/src" -Filter '*.c' |
            ForEach-Object FullName

        & cl.exe @flags /std:c11 $include /c @sources

        if ($LASTEXITCODE -ne 0) {
            throw 'libvterm compilation failed.'
        }

        $objects = $sources | ForEach-Object {
            $objectName = [IO.Path]::GetFileNameWithoutExtension($_) + '.obj'
            Join-Path $output $objectName
        }
        $hostSources = @(
            "$root/native/host.cpp"
            "$root/native/conpty.cpp"
            "$root/native/classic.cpp"
        )

        & cl.exe @flags /std:c++17 /EHsc /W4 /WX $include "$root/tests/vterm-reflow-test.cpp" @objects /Fe:vterm-reflow-test.exe
        if ($LASTEXITCODE -ne 0) {
            throw 'Terminal reflow test compilation failed.'
        }
        if ($Test) {
            & ./vterm-reflow-test.exe
            if ($LASTEXITCODE -ne 0) {
                throw 'Terminal reflow specifications failed.'
            }
        }

        & cl.exe @flags /std:c++17 /EHsc /W4 /WX $include @hostSources @objects /Fe:neo-term-host.exe user32.lib

        if ($LASTEXITCODE -ne 0) {
            throw 'Host compilation failed.'
        }

        & cl.exe @flags /std:c++17 /EHsc /W4 /WX "$root/tests/console-fixture.cpp" /Fe:console-fixture.exe user32.lib

        if ($LASTEXITCODE -ne 0) {
            throw 'Console fixture compilation failed.'
        }
    }

    if ($Test) {
        & ./native-test.exe

        if ($LASTEXITCODE -ne 0) {
            throw 'Native tests failed.'
        }
    }
} finally {
    Pop-Location
}
