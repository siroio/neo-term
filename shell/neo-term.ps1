if (-not $global:neoTermPromptInstalled) {
    $global:neoTermOriginalPrompt = (Get-Item Function:\prompt).ScriptBlock
    $global:neoTermPromptInstalled = $true

    function global:prompt {
        $promptResult = & $global:neoTermOriginalPrompt
        $promptText = [string]::Join('', @($promptResult))
        $plainPrompt = [regex]::Replace($promptText, '\x1b\[[0-?]*[ -/]*[@-~]', '')
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
            $bytes = [Text.Encoding]::UTF8.GetBytes($metadata)
            $Host.UI.RawUI.WindowTitle = 'neo-term;' + [Convert]::ToBase64String($bytes)
        } catch {
        }

        $promptText
    }
}
