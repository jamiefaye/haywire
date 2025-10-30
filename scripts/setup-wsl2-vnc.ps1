# WSL2 VNC Port Forwarding Setup
# Run this script to enable VNC access via localhost:5900

Write-Host "=== WSL2 VNC Port Forwarding Setup ===" -ForegroundColor Cyan
Write-Host ""

# Check if running as administrator
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# Get WSL2 IP address
Write-Host "Getting WSL2 IP address..." -ForegroundColor Yellow
$wslIP = (wsl bash -c "hostname -I").Trim()

if (-not $wslIP) {
    Write-Host "ERROR: Could not get WSL2 IP address" -ForegroundColor Red
    Write-Host "Make sure WSL2 is running" -ForegroundColor Red
    exit 1
}

Write-Host "WSL2 IP: $wslIP" -ForegroundColor Green
Write-Host ""

# Display connection info
Write-Host "=== VNC Connection Options ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "Option 1 - Direct WSL2 IP (Works immediately):" -ForegroundColor Yellow
Write-Host "  Connect to: $wslIP:5900" -ForegroundColor White
Write-Host ""

if ($isAdmin) {
    Write-Host "Option 2 - Localhost forwarding (Setting up now):" -ForegroundColor Yellow

    # Remove existing forward if any
    netsh interface portproxy delete v4tov4 listenport=5900 listenaddress=127.0.0.1 2>$null | Out-Null

    # Add new forward
    $result = netsh interface portproxy add v4tov4 listenport=5900 listenaddress=127.0.0.1 connectport=5900 connectaddress=$wslIP

    if ($LASTEXITCODE -eq 0) {
        Write-Host "  ✓ Port forwarding configured!" -ForegroundColor Green
        Write-Host "  Connect to: localhost:5900" -ForegroundColor White
        Write-Host ""

        # Show current forwards
        Write-Host "Active port forwards:" -ForegroundColor Cyan
        netsh interface portproxy show v4tov4
    } else {
        Write-Host "  ✗ Failed to set up port forwarding" -ForegroundColor Red
        Write-Host "  Use Option 1 instead" -ForegroundColor Yellow
    }
} else {
    Write-Host "Option 2 - Localhost forwarding (Run as Admin to enable):" -ForegroundColor Yellow
    Write-Host "  Right-click this script and 'Run as Administrator'" -ForegroundColor Gray
    Write-Host "  Then connect to: localhost:5900" -ForegroundColor Gray
}

Write-Host ""
Write-Host "=== QMP and Monitor Ports ===" -ForegroundColor Cyan
Write-Host "QMP:     localhost:4445" -ForegroundColor White
Write-Host "Monitor: localhost:4444" -ForegroundColor White
Write-Host ""

Write-Host "Press any key to exit..." -ForegroundColor Gray
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
