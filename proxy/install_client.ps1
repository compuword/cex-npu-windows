# CEX Virtual NPU -- Client Installer (Windows i5-7500)
# install_client.ps1
#
# 1. Enables test-signing mode (required for unsigned WDDM driver)
# 2. Installs the WDDM kernel driver (cex_virt_npu.sys)
# 3. Installs the proxy Windows service (cex_npu_proxy.py)
# 4. Registers the ONNX Runtime Execution Provider (cex_npu_ep.dll)
#
# Run as Administrator:
#   Set-ExecutionPolicy Bypass -Scope Process
#   .\install_client.ps1 [-ServerHost 192.168.1.100] [-SkipDriver] [-TestOnly]

param(
    [string]$ServerHost = $env:CEX_NPU_SERVER,
    [int]   $ServerPort = 7474,
    [switch]$SkipDriver,
    [switch]$TestOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT_DIR = Split-Path -Parent $SCRIPT_DIR
$DRIVER_DIR  = Join-Path $PROJECT_DIR "driver\wddm"
$SERVICE_NAME = "CexNpuProxy"
$VENV_DIR    = Join-Path $PROJECT_DIR "venv"

function Write-Info  { param($m) Write-Host "[INFO]  $m" -ForegroundColor Cyan }
function Write-Ok    { param($m) Write-Host "[OK]    $m" -ForegroundColor Green }
function Write-Warn  { param($m) Write-Host "[WARN]  $m" -ForegroundColor Yellow }
function Write-Err   { param($m) Write-Host "[ERR]   $m" -ForegroundColor Red }

Write-Info "CEX Virtual NPU Client Installer"
Write-Info "Project: $PROJECT_DIR"
Write-Info "Driver:  $DRIVER_DIR"
Write-Info "Server:  ${ServerHost}:${ServerPort}"
""

# ---------------------------------------------------------------
# Admin check
# ---------------------------------------------------------------
$adminPrincipal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $adminPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Err "Must run as Administrator."
    Write-Err "Right-click PowerShell -> Run as Administrator"
    exit 1
}

# ---------------------------------------------------------------
# Test-only mode: verify connectivity
# ---------------------------------------------------------------
if ($TestOnly) {
    Write-Info "Test mode: checking connectivity to $ServerHost`:$ServerPort..."
    $tcp = New-Object System.Net.Sockets.TcpClient
    try {
        $tcp.ConnectAsync($ServerHost, 7476).Wait(2000) | Out-Null
        if ($tcp.Connected) {
            $health = Invoke-RestMethod "http://${ServerHost}:7476/health" -TimeoutSec 3
            Write-Ok "Server reachable. NPU available: $($health.npu_available)"
            Write-Ok "Providers: $($health.providers -join ', ')"
        } else {
            Write-Err "Cannot connect to $ServerHost`:7476 -- is install_server.sh done?"
        }
    } catch {
        Write-Err "Connection test failed: $_"
    } finally {
        $tcp.Close()
    }
    exit 0
}

# ---------------------------------------------------------------
# 1. Python venv + dependencies
# ---------------------------------------------------------------
Write-Info "Setting up Python environment..."
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Err "python not found. Install Python 3.9+ from python.org"
    exit 1
}

python -m venv $VENV_DIR | Out-Null
$pip = Join-Path $VENV_DIR "Scripts\pip.exe"
& $pip install --quiet --upgrade pip
& $pip install --quiet numpy pywin32 onnxruntime onnx psutil
Write-Ok "Python environment ready: $VENV_DIR"

# ---------------------------------------------------------------
# 2. Test-signing mode (for unsigned kernel driver)
# ---------------------------------------------------------------
if (-not $SkipDriver) {
    Write-Info "Enabling test-signing mode (required for unsigned WDDM driver)..."
    Write-Warn "A system reboot is required after enabling test-signing."
    $confirm = Read-Host "Enable test-signing mode? (y/N)"
    if ($confirm -eq "y" -or $confirm -eq "Y") {
        bcdedit /set testsigning on
        Write-Ok "Test-signing enabled. Reboot required before driver install."
        Write-Warn "After reboot, re-run this script with -SkipDriver:$false"
        $reboot = Read-Host "Reboot now? (y/N)"
        if ($reboot -eq "y" -or $reboot -eq "Y") {
            Restart-Computer -Force
        }
        exit 0
    } else {
        Write-Warn "Skipping test-signing. Driver install may fail if test-signing is not active."
    }

    # ---------------------------------------------------------------
    # 3. Install WDDM kernel driver via devcon
    # ---------------------------------------------------------------
    $infPath = Join-Path $DRIVER_DIR "cex_virt_npu.inf"
    if (-not (Test-Path $infPath)) {
        Write-Warn "cex_virt_npu.inf not found at $infPath"
        Write-Warn "Build the driver with Visual Studio + WDK first."
        Write-Warn "See: cex_virtual_npu/driver/wddm/BUILD.md"
    } else {
        $devconCmd = Get-Command devcon -ErrorAction SilentlyContinue
        $devcon = if ($devconCmd) { $devconCmd.Source } else { $null }
        if (-not $devcon) {
            Write-Warn "devcon.exe not found. Download from WDK Tools."
            Write-Warn "Manual: devcon install `"$infPath`" Root\CexVirtualNPU"
        } else {
            Write-Info "Installing driver: $infPath"
            & $devcon install $infPath "Root\CexVirtualNPU"
            if ($LASTEXITCODE -eq 0) {
                Write-Ok "Driver installed. NPU will appear in Task Manager after proxy starts."
            } else {
                Write-Err "Driver install failed (exit $LASTEXITCODE). Check DevMgr for errors."
            }
        }
    }
}

# ---------------------------------------------------------------
# 4. Install proxy as Windows Service
# ---------------------------------------------------------------
Write-Info "Installing proxy Windows service: $SERVICE_NAME..."

$python = Join-Path $VENV_DIR "Scripts\python.exe"
$proxyScript = Join-Path $SCRIPT_DIR "cex_npu_proxy.py"

# Store server config in registry
$regKey = "HKLM:\SOFTWARE\CexVirtualNPU"
if (-not (Test-Path $regKey)) { New-Item -Path $regKey -Force | Out-Null }
Set-ItemProperty -Path $regKey -Name "ServerHost" -Value $ServerHost
Set-ItemProperty -Path $regKey -Name "ServerPort" -Value $ServerPort

# Remove old service if exists
$existing = Get-Service -Name $SERVICE_NAME -ErrorAction SilentlyContinue
if ($existing) {
    Write-Info "Removing existing service..."
    Stop-Service -Name $SERVICE_NAME -Force -ErrorAction SilentlyContinue
    sc.exe delete $SERVICE_NAME | Out-Null
    Start-Sleep -Seconds 2
}

# Create service using sc.exe (no NSSM required)
$binPath = "`"$python`" `"$proxyScript`" --server $ServerHost --rx-port $ServerPort"
sc.exe create $SERVICE_NAME binPath= $binPath start= auto DisplayName= "CEX Virtual NPU Proxy" | Out-Null
sc.exe description $SERVICE_NAME "Routes NPU inference requests to Qualcomm ARM server via LAN (CEX Virtual NPU)" | Out-Null
sc.exe failure $SERVICE_NAME reset= 30 actions= restart/5000/restart/10000/restart/30000 | Out-Null

Write-Info "Starting service..."
Start-Service -Name $SERVICE_NAME
$svc = Get-Service -Name $SERVICE_NAME
Write-Ok "Service status: $($svc.Status)"

# ---------------------------------------------------------------
# 5. Register ONNX Runtime EP (environment variable)
# ---------------------------------------------------------------
Write-Info "Registering ONNX Runtime Execution Provider..."
$epDll = Join-Path $env:SystemRoot "System32\cex_npu_ep.dll"
if (Test-Path $epDll) {
    [System.Environment]::SetEnvironmentVariable("ORT_CUSTOM_EP_PATHS", $epDll, "Machine")
    Write-Ok "EP DLL registered: $epDll"
} else {
    Write-Warn "cex_npu_ep.dll not found at $epDll"
    Write-Warn "Build cex_npu_ep.dll (ep/CMakeLists.txt) and copy to System32"
    Write-Warn "Apps can still use the Python API directly without the DLL"
}

# ---------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------
""
Write-Ok "Installation complete."
Write-Info ""
Write-Info "Next steps:"
Write-Info "  1. Verify Task Manager > Performance shows 'NPU' tab"
Write-Info "     (requires driver + proxy running + ARM server reachable)"
Write-Info "  2. Test: .\install_client.ps1 -TestOnly -ServerHost $ServerHost"
Write-Info "  3. Use CexNpuExecutionProvider in ONNX Runtime apps:"
Write-Info "     sess = ort.InferenceSession(model, providers=['CexNpuExecutionProvider'])"
""
Write-Info "Service management:"
Write-Info "  Start:  Start-Service $SERVICE_NAME"
Write-Info "  Stop:   Stop-Service  $SERVICE_NAME"
Write-Info "  Logs:   Get-EventLog -LogName Application -Source $SERVICE_NAME -Newest 20"
""
Write-Info "Registry config: HKLM:\SOFTWARE\CexVirtualNPU"
