if (-not $global:neoTermPromptInstalled) {
    $global:neoTermOriginalPrompt = (Get-Item Function:\prompt).ScriptBlock
    $global:neoTermPromptInstalled = $true

    function global:Send-NeoTermNotification([string] $Message) {
        if (-not $env:NEO_TERM_PIPE) {
            return
        }
        $savedExitCode = $global:LASTEXITCODE
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes($Message)
            & $env:NEO_TERM_HOST --notify encoded ([Convert]::ToBase64String($bytes))
        } finally {
            $global:LASTEXITCODE = $savedExitCode
        }
    }

    function global:neo-open {
        param(
            [Parameter(Mandatory = $true, Position = 0)] [string] $Path,
            [Parameter(Position = 1)] [int] $Line = 1,
            [Parameter(Position = 2)] [int] $Column = 1
        )
        $file = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).ProviderPath
        $request = @{ file = $file; line = $Line; column = $Column } | ConvertTo-Json -Compress
        Send-NeoTermNotification ('51;neo-term;' + $request)
    }

    function global:prompt {
        $promptResult = & $global:neoTermOriginalPrompt
        $promptText = [string]::Join('', @($promptResult))
        $plainPrompt = [regex]::Replace($promptText, '\x1b\[[0-?]*[ -/]*[@-~]', '')
        $plainPrompt = [regex]::Replace($plainPrompt, '\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)', '')
        $directory = ''
        if ($PWD.Provider.Name -eq 'FileSystem') {
            $directory = $PWD.ProviderPath
        }

        $metadata = @{
            directory = $directory
            prompt = $plainPrompt
            title = "PowerShell: $directory"
        } | ConvertTo-Json -Compress

        try {
            $start = $Host.UI.RawUI.CursorPosition
            Send-NeoTermNotification "prompt;$($start.X);$($start.Y);$plainPrompt"
            Send-NeoTermNotification ('meta;' + $metadata)
            $bytes = [Text.Encoding]::UTF8.GetBytes($metadata)
            $Host.UI.RawUI.WindowTitle = 'neo-term;' + [Convert]::ToBase64String($bytes)
        } catch {
        }

        $escape = [char]27
        "$escape]133;A$escape\$promptText$escape]133;B$escape\"
    }
}
