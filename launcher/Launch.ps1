param(
    [switch]$Dx,
    [switch]$Windowed,
    [switch]$HomePage,
    [string]$SharedFolder,
    [string]$SpiceUri
)
$ErrorActionPreference = 'Stop'
$clientRoot = $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms
try {
    if (-not $HomePage -and -not $SpiceUri) { throw 'Specify -SpiceUri or use -HomePage' }
    if ($SharedFolder -and -not (Test-Path -LiteralPath $SharedFolder -PathType Container)) {
        throw "Shared folder does not exist: $SharedFolder"
    }
    $env:PATH = "$clientRoot\bin;$env:SystemRoot\System32;$env:SystemRoot"
    $env:GSETTINGS_SCHEMA_DIR = "$clientRoot\share\glib-2.0\schemas"
    $env:XDG_DATA_DIRS = "$clientRoot\share"
    $env:GIO_EXTRA_MODULES = "$clientRoot\lib\gio\modules"
    $env:GST_PLUGIN_SYSTEM_PATH_1_0 = "$clientRoot\lib\gstreamer-1.0"
    $env:GST_PLUGIN_PATH_1_0 = ''
    $env:GST_PLUGIN_SCANNER_1_0 = "$clientRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe"
    $env:GDK_PIXBUF_MODULEDIR = "$clientRoot\lib\gdk-pixbuf-2.0\2.10.0\loaders"
    $env:GDK_PIXBUF_MODULE_FILE = "$clientRoot\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache"
    $logDir = Join-Path $clientRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $env:FLEXVDI_LOG_FILE = Join-Path $logDir "client-$stamp.log"
    if ($Dx) {
        & "$clientRoot\bin\gst-inspect-1.0.exe" d3d11h264dec *> (Join-Path $logDir "decoder-$stamp.log")
        if ($LASTEXITCODE -ne 0) {
            throw 'D3D11 H.264 hardware decoder unavailable. DX mode was not started.'
        }
        $env:SPICE_DX_H264 = '1'
    } else {
        Remove-Item Env:SPICE_DX_H264 -ErrorAction SilentlyContinue
    }
    & "$clientRoot\bin\gdk-pixbuf-query-loaders.exe" --update-cache
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare image loader cache' }
    $viewerArgs = @()
    if ($SharedFolder) { $viewerArgs += @('--shared-folder', ('"' + $SharedFolder + '"')) }
    if (-not $HomePage) { $viewerArgs = @($SpiceUri) + $viewerArgs }
    if ($Windowed) { $viewerArgs += '--no-fullscreen' } else { $viewerArgs += '--fullscreen' }
    if ($Dx) { $viewerArgs += '--no-resize-guest' }
    Start-Process -FilePath "$clientRoot\bin\flexvdi-client.exe" -ArgumentList $viewerArgs -WorkingDirectory $clientRoot -WindowStyle Normal
} catch {
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'SPICE startup failed') | Out-Null
    exit 1
}
