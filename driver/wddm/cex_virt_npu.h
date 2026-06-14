/*
 * CEX Virtual NPU -- WDDM Kernel Mode Driver Header
 * cex_virt_npu.h
 *
 * Implements a WDDM 3.2 compute-accelerator device that reports
 * D3DKMT_NODETYPE_ML nodes. This causes Windows Task Manager to
 * show an "NPU" tab with utilization data sourced from the
 * CEX NPU Proxy service (cex_npu_proxy.py) running on the same machine.
 *
 * Build requirements:
 *   Windows Driver Kit (WDK) 11 (for Windows 11 23H2 target)
 *   Visual Studio 2022 with C++ Desktop + WDK extension
 *   SDK: Windows 11 SDK 10.0.22621.0 or later
 *
 * Key references:
 *   WDDM spec: https://docs.microsoft.com/en-us/windows-hardware/drivers/display/wddm-architecture
 *   D3DKMT_NODETYPE: d3dkmdt.h in WDK
 *   Compute-only adapters: WDDM 2.7+ D3DKMT_NODETYPE_ML
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdm.h>
#include <dispmprt.h>
#include <d3dkmthk.h>
#include <d3dkmdt.h>

/* ---------------------------------------------------------------
 * Driver identity
 * --------------------------------------------------------------- */
#define CEX_NPU_POOL_TAG       'PNXC'   /* CXNP reversed */
#define CEX_NPU_VENDOR_ID      0xCEA1   /* synthetic vendor */
#define CEX_NPU_DEVICE_ID      0x4E50   /* 'NP' */
#define CEX_NPU_SUBSYSTEM_ID   0x0001
#define CEX_NPU_REVISION       0x01
#define CEX_NPU_DRIVER_VERSION 0x00010000  /* 1.0.0.0 */

/* Shared memory name for utilization exchange with proxy service */
#define CEX_NPU_SHARED_MEM_NAME  L"CexNpuUtil"
#define CEX_NPU_SHARED_MEM_SIZE  4096UL    /* 4 KB -- holds float32 util + stats */

/* Named event signaled by proxy when new utilization data is ready */
#define CEX_NPU_UPDATE_EVENT_NAME L"\\BaseNamedObjects\\CexNpuUtilUpdate"

/* Named pipe to proxy service (for inference dispatch from DXG kernel) */
#define CEX_NPU_PROXY_PIPE_NAME  L"\\Device\\NamedPipe\\CexNpuProxy"

/* Maximum number of ML nodes reported to DXGKRNL */
#define CEX_NPU_NODE_COUNT       1

/* Node index for the ML/NPU node */
#define CEX_NPU_ML_NODE_INDEX    0

/* ---------------------------------------------------------------
 * Shared memory layout (between kernel driver and user-mode proxy)
 * --------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _CEX_NPU_SHARED_DATA {
    FLOAT  npu_utilization;       /* 0.0 - 100.0 -- written by proxy */
    FLOAT  network_latency_ms;    /* last inference latency */
    UINT32 queue_depth;           /* pending inference requests */
    UINT32 requests_total;        /* cumulative since boot */
    UINT32 requests_ok;
    UINT32 requests_err;
    UINT64 last_update_tick;      /* GetTickCount64() from proxy */
    CHAR   server_host[64];       /* ARM server IP (null-terminated) */
    UINT16 server_port;           /* ARM server port (default 7474) */
    BYTE   padding[122];          /* align to 256 bytes */
} CEX_NPU_SHARED_DATA, *PCEX_NPU_SHARED_DATA;
#pragma pack(pop)

static_assert(sizeof(CEX_NPU_SHARED_DATA) == 256, "shared data size mismatch");

/* ---------------------------------------------------------------
 * Driver extension (device context)
 * --------------------------------------------------------------- */
typedef struct _CEX_NPU_DEVICE_CONTEXT {
    /* WDF handles */
    WDFDEVICE          WdfDevice;
    WDFQUEUE           DefaultQueue;

    /* Shared memory with user-mode proxy */
    HANDLE             SharedMemHandle;
    PCEX_NPU_SHARED_DATA SharedData;

    /* Update event (proxy signals this when new utilization data arrives) */
    HANDLE             UpdateEvent;
    PKTHREAD           UtilPollThread;
    BOOLEAN            StopPollThread;

    /* DXGKRNL interface (obtained via IoGetDeviceProperty) */
    DXGKRNL_INTERFACE  DxgkInterface;
    BOOLEAN            DxgkRegistered;

    /* Adapter LUID assigned by DXGKRNL */
    LUID               AdapterLuid;

    /* Current utilization (cached from shared memory) */
    volatile LONG      UtilizationPermille;  /* 0-1000 (per-mille of 100%) */

    /* Statistics */
    ULONGLONG          TotalInferences;
    ULONGLONG          TotalBytesTransferred;

} CEX_NPU_DEVICE_CONTEXT, *PCEX_NPU_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CEX_NPU_DEVICE_CONTEXT, CexNpuGetDeviceContext)

/* ---------------------------------------------------------------
 * DXGKRNL callback function prototypes
 * (implementations in cex_virt_npu.c -- DXGKDDI entry points)
 * --------------------------------------------------------------- */

DXGKDDI_ADD_DEVICE             CexNpuDdiAddDevice;
DXGKDDI_START_DEVICE           CexNpuDdiStartDevice;
DXGKDDI_STOP_DEVICE            CexNpuDdiStopDevice;
DXGKDDI_REMOVE_DEVICE          CexNpuDdiRemoveDevice;
DXGKDDI_DISPATCH_IO_REQUEST    CexNpuDdiDispatchIoRequest;
DXGKDDI_QUERY_CHILD_RELATIONS  CexNpuDdiQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS     CexNpuDdiQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR CexNpuDdiQueryDeviceDescriptor;
DXGKDDI_SET_POWER_STATE        CexNpuDdiSetPowerState;
DXGKDDI_GET_NODE_METADATA      CexNpuDdiGetNodeMetadata;
DXGKDDI_QUERYADAPTERINFO       CexNpuDdiQueryAdapterInfo;
DXGKDDI_SUBMITCOMMAND          CexNpuDdiSubmitCommand;

/* ---------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuInitSharedMemory(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
);

VOID
CexNpuCleanupSharedMemory(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
);

VOID
CexNpuUtilizationPollThread(
    _In_ PVOID Context
);

NTSTATUS
CexNpuRegisterWithDxgkrnl(
    _In_  WDFDEVICE Device,
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
);

VOID
CexNpuUpdateUtilization(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
);
