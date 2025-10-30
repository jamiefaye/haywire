# Quick script to get WSL2 IP address for VNC connection
# Just run this to see what IP to use

$wslIP = (wsl bash -c "hostname -I").Trim()

if ($wslIP) {
    Write-Host "WSL2 IP Address: $wslIP" -ForegroundColor Green
    Write-Host ""
    Write-Host "Connect VNC to: $wslIP:5900" -ForegroundColor Cyan
} else {
    Write-Host "Could not get WSL2 IP address" -ForegroundColor Red
    Write-Host "Make sure WSL2 is running" -ForegroundColor Red
}
