# CEX NPU Windows

Windows client stack for distributed NPU inference over LAN.

Routes ONNX Runtime inference from a machine with no native NPU to a
Qualcomm ARM Linux server (see **cex-npu-linux**) via 1 Gbps LAN.

## What it does

- Installs a WDDM compute-accelerator driver that makes Windows Task Manager
  show a real **NPU tab** with live utilization %
- Registers a custom ONNX Runtime Execution Provider (`CexNpuExecutionProvider`)
  so apps that already use ORT work without code changes
- Routes inference over TCP to the ARM server via a Windows proxy service

## Stack

```
[ONNX App]
  providers=['CexNpuExecutionProvider']
      |
      v (named pipe \\.\pipe\CexNpuProxy)
  cex_npu_proxy.py  (Windows service)
      |  writes util% to shared mem "CexNpuUtil"
      v (TCP port 7474)
  [cex-npu-linux server]
```

## Components

| Path | Description |
|------|-------------|
| `shared/cex_npu_protocol.py` | CXNP binary wire protocol |
| `proxy/cex_npu_proxy.py` | Windows proxy service (named pipe + TCP client) |
| `proxy/install_client.ps1` | Installer: test-signing, driver, service, ORT EP |
| `proxy/requirements.txt` | Python deps: numpy, pywin32, onnxruntime |
| `ep/cex_npu_ep.cpp` | ONNX Runtime custom EP DLL (C++) |
| `ep/CMakeLists.txt` | CMake build for cex_npu_ep.dll |
| `driver/wddm/cex_virt_npu.c` | WDDM kernel driver (D3DKMT_NODETYPE_ML) |
| `driver/wddm/cex_virt_npu.h` | Driver header + shared memory struct |
| `driver/wddm/cex_virt_npu.inf` | INF: ComputeAccelerator class device |

## Requirements

- Windows 10/11 Pro (Build 22621+ for NPU tab in Task Manager)
- Python 3.9+
- ONNX Runtime 1.17+
- Visual Studio 2022 + WDK (to build kernel driver)
- CMake 3.20+ (to build EP DLL)

## Quick Start

```powershell
# 1. Verify ARM server is running
.\proxy\install_client.ps1 -TestOnly -ServerHost 192.168.1.100

# 2. Full install (as Administrator)
Set-ExecutionPolicy Bypass -Scope Process
.\proxy\install_client.ps1 -ServerHost 192.168.1.100
```

## Build (EP DLL)

```powershell
$ORT = "C:\onnxruntime-win-x64-1.18.0"
cmake -B build -DONNXRUNTIME_ROOT=$ORT -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
copy build\Release\cex_npu_ep.dll C:\Windows\System32\
```

## Network Ports

| Port | Role | Protocol |
|------|------|----------|
| 7474 | RX -- inference requests | TCP/CXNP |
| 7476 | Health | HTTP/JSON |

## Related

- [cex-npu-linux](https://github.com/brunodanna/cex-npu-linux) -- ARM Linux NPU server
- [cex-orchestrator](https://github.com/brunodanna/cex-orchestrator) -- Orchestrator + GPU routing
