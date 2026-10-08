param(
    [string]$Python = 'python',
    [string]$Emacs = 'emacs',
    [switch]$Gui
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & ./build/native-test.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Native specifications failed.'
    }
    & ./build/vterm-reflow-test.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Terminal reflow specifications failed.'
    }

    & $Python -X utf8 -m unittest discover -s tests -p test_host.py
    if ($LASTEXITCODE -ne 0) {
        throw 'CLI integration specifications failed.'
    }

    & $Emacs -Q --batch -L . --eval '(setq byte-compile-error-on-warn t)' -f batch-byte-compile neo-term.el
    if ($LASTEXITCODE -ne 0) {
        throw 'Emacs byte compilation failed.'
    }

    & $Emacs -Q --batch -l tests/neo-term-test.el -f ert-run-tests-batch-and-exit
    if ($LASTEXITCODE -ne 0) {
        throw 'Emacs specifications failed.'
    }

    if ($Gui) {
        $emacsExecutable = (Get-Command $Emacs -ErrorAction Stop).Source
        $guiArguments = @('-Q', '-l', (Join-Path $PSScriptRoot 'tests/check-gui.el'))
        $guiOptions = @{
            FilePath = $emacsExecutable
            ArgumentList = $guiArguments
            WindowStyle = 'Hidden'
            PassThru = $true
        }
        $guiProcess = Start-Process @guiOptions

        if (-not $guiProcess.WaitForExit(60000)) {
            $guiProcess.Kill()
            throw 'GUI verification timed out.'
        }

        if ($guiProcess.ExitCode -ne 0) {
            throw 'GUI verification failed.'
        }

        $guiLog = Get-Content -LiteralPath 'build/gui-check.log' -Raw
        if (-not $guiLog.StartsWith('GUI_TESTS=PASS')) {
            throw $guiLog
        }

        Write-Output $guiLog
    }
} finally {
    Pop-Location
}
