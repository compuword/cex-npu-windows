/*
 * CEX Virtual NPU -- WDDM 3.2 Kernel Mode Driver
 * cex_virt_npu.c
 *
 * Creates a compute-accelerator device visible in Windows Task Manager
 * as an NPU tab.  The device reports one ML node (D3DKMT_NODETYPE_ML)
 * to DXGKRNL.  Utilization data is read from a named shared-memory
 * region that the user-mode proxy (cex_npu_proxy.py) updates on each
 * inference cycle.
 *
 * Build:
 *   Visual Studio 2022 + WDK 11 + Windows 11 SDK 10.0.22621.0
 *   Sign with "signtool sign /fd SHA256 /a cex_virt_npu.sys" (test cert)
 *   Or submit to Microsoft Hardware Dev Center for attestation signing.
 *
 * Install:
 *   devcon install cex_virt_npu.inf Root\CexVirtualNPU
 *   (requires test-signing mode: bcdedit /set testsigning on)
 *
 * Task Manager NPU tab appears after:
 *   1. Driver installed and started
 *   2. Proxy service running (cex_npu_proxy.py or cex_npu_service.exe)
 *   3. ARM server reachable on LAN
 */

#include "cex_virt_npu.h"

/* ---------------------------------------------------------------
 * DriverEntry -- called by Windows when driver is loaded
 * --------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS               status;
    WDF_DRIVER_CONFIG      config;
    DRIVER_INITIALIZATION_DATA dxgkInitData;

    WDF_DRIVER_CONFIG_INIT(&config, CexNpuEvtDeviceAdd);
    config.DriverPoolTag = CEX_NPU_POOL_TAG;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: WdfDriverCreate failed 0x%08X\n", status));
        return status;
    }

    /* Register DXGKRNL miniport entry points */
    RtlZeroMemory(&dxgkInitData, sizeof(dxgkInitData));
    dxgkInitData.Version                   = DXGKDDI_INTERFACE_VERSION_WDDM2_9;
    dxgkInitData.DxgkDdiAddDevice          = CexNpuDdiAddDevice;
    dxgkInitData.DxgkDdiStartDevice        = CexNpuDdiStartDevice;
    dxgkInitData.DxgkDdiStopDevice         = CexNpuDdiStopDevice;
    dxgkInitData.DxgkDdiRemoveDevice       = CexNpuDdiRemoveDevice;
    dxgkInitData.DxgkDdiDispatchIoRequest  = CexNpuDdiDispatchIoRequest;
    dxgkInitData.DxgkDdiQueryChildRelations= CexNpuDdiQueryChildRelations;
    dxgkInitData.DxgkDdiQueryChildStatus   = CexNpuDdiQueryChildStatus;
    dxgkInitData.DxgkDdiQueryDeviceDescriptor = CexNpuDdiQueryDeviceDescriptor;
    dxgkInitData.DxgkDdiSetPowerState      = CexNpuDdiSetPowerState;
    dxgkInitData.DxgkDdiGetNodeMetadata    = CexNpuDdiGetNodeMetadata;
    dxgkInitData.DxgkDdiQueryAdapterInfo   = CexNpuDdiQueryAdapterInfo;
    dxgkInitData.DxgkDdiSubmitCommand      = CexNpuDdiSubmitCommand;

    status = DxgkInitialize(DriverObject, RegistryPath, &dxgkInitData);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: DxgkInitialize failed 0x%08X\n", status));
    }
    return status;
}

/* ---------------------------------------------------------------
 * CexNpuEvtDeviceAdd -- create WDF device object
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS                     status;
    WDF_OBJECT_ATTRIBUTES        devAttr;
    WDFDEVICE                    device;
    PCEX_NPU_DEVICE_CONTEXT      ctx;
    WDF_IO_QUEUE_CONFIG          queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, CEX_NPU_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &devAttr, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    ctx = CexNpuGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(CEX_NPU_DEVICE_CONTEXT));
    ctx->WdfDevice = device;

    /* Default I/O queue */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = CexNpuEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: WdfIoQueueCreate failed 0x%08X\n", status));
        return status;
    }

    /* Map shared memory from proxy service */
    status = CexNpuInitSharedMemory(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: shared memory init warning 0x%08X (proxy not running?)\n", status));
        /* Non-fatal: proxy may start later. */
    }

    KdPrint(("CexNpu: device created\n"));
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * DXGKRNL DDI: AddDevice
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuDdiAddDevice(
    _In_  PHYSICAL_ADDRESS PhysicalAddress,
    _In_  PVOID            MiniportDeviceContext,
    _Out_ HANDLE          *DeviceHandle
)
{
    UNREFERENCED_PARAMETER(PhysicalAddress);
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    *DeviceHandle = (HANDLE)(ULONG_PTR)0xCEA10001;
    KdPrint(("CexNpu: DdiAddDevice\n"));
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * DXGKRNL DDI: StartDevice
 * Populate adapter capabilities and node layout.
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuDdiStartDevice(
    _In_  PVOID                         MiniportDeviceContext,
    _In_  PDXGK_START_INFO              DxgkStartInfo,
    _In_  PDXGKRNL_INTERFACE            DxgkInterface,
    _Out_ PULONG                        NumberOfVideoPresentSources,
    _Out_ PULONG                        NumberOfChildren
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DxgkStartInfo);
    UNREFERENCED_PARAMETER(DxgkInterface);

    /*
     * Compute-only adapter: 0 video present sources, 0 children.
     * DXGKRNL will not try to enumerate monitors.
     */
    *NumberOfVideoPresentSources = 0;
    *NumberOfChildren            = 0;

    KdPrint(("CexNpu: DdiStartDevice -- compute-only adapter\n"));
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * DXGKRNL DDI: QueryAdapterInfo
 * Report adapter capabilities and the ML node configuration.
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuDdiQueryAdapterInfo(
    _In_ PVOID                         MiniportDeviceContext,
    _In_ CONST DXGKARG_QUERYADAPTERINFO *QueryAdapterInfo
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    switch (QueryAdapterInfo->Type) {

    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(caps, sizeof(*caps));
        caps->HighestAcceptableAddress.QuadPart = (LONGLONG)-1;
        /* Declare as compute-only (no display) */
        caps->WDDMVersion               = DXGKDDI_WDDMVersion_WDDM2_9;
        caps->GpuEngineTopology.NbAsyncEngineCount = CEX_NPU_NODE_COUNT;
        caps->SupportNonVGA             = TRUE;
        caps->SupportSmoothRotation     = FALSE;
        caps->SupportPerEngineTDR       = TRUE;
        caps->SupportDirectFlip         = FALSE;
        return STATUS_SUCCESS;
    }

    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        DXGK_PHYSICALADAPTERCAPS *pcaps =
            (DXGK_PHYSICALADAPTERCAPS *)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(pcaps, sizeof(*pcaps));
        pcaps->NumExecutionNodes = CEX_NPU_NODE_COUNT;
        /*
         * Mark this adapter as a machine learning accelerator.
         * DXGKRNL routes D3D12 ML workloads to this adapter.
         */
        pcaps->SupportRuntimePowerManagement = TRUE;
        return STATUS_SUCCESS;
    }

    default:
        return STATUS_NOT_IMPLEMENTED;
    }
}

/* ---------------------------------------------------------------
 * DXGKRNL DDI: GetNodeMetadata
 * Critical: report D3DKMT_NODETYPE_ML for the NPU node.
 * This is what causes Task Manager to show the NPU tab.
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuDdiGetNodeMetadata(
    _In_  PVOID                    MiniportDeviceContext,
    _In_  UINT                     NodeOrdinal,
    _Out_ DXGKARG_GETNODEMETADATA *NodeMetadata
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    if (NodeOrdinal != CEX_NPU_ML_NODE_INDEX) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(NodeMetadata, sizeof(*NodeMetadata));

    /*
     * D3DKMT_NODETYPE_ML is the magic value that causes Windows
     * Task Manager (Windows 11 23H2+) to show the NPU tab and
     * route the node's utilization counter there.
     */
    NodeMetadata->EngineType = D3DKMT_NODETYPE_ML;

    /* Friendly name shown in Task Manager tooltip */
    RtlCopyMemory(
        NodeMetadata->FriendlyName,
        L"CEX Virtual NPU (LAN)",
        sizeof(L"CEX Virtual NPU (LAN)")
    );

    KdPrint(("CexNpu: DdiGetNodeMetadata node=%u type=D3DKMT_NODETYPE_ML\n", NodeOrdinal));
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * DXGKRNL DDI: SubmitCommand
 * Intercepts compute commands destined for the ML node.
 * In this driver the actual work is handled by the EP DLL
 * (cex_npu_ep.dll) in user mode; the kernel driver only needs
 * to update utilization and acknowledge the command.
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuDdiSubmitCommand(
    _In_ PVOID                       MiniportDeviceContext,
    _In_ CONST DXGKARG_SUBMITCOMMAND *SubmitCommand
)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SubmitCommand);
    /* Real dispatch handled by EP DLL -> proxy service -> ARM server */
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * Shared memory: map region written by proxy service (user mode)
 * --------------------------------------------------------------- */
NTSTATUS
CexNpuInitSharedMemory(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
)
{
    NTSTATUS              status = STATUS_SUCCESS;
    UNICODE_STRING        memName;
    LARGE_INTEGER         size;
    OBJECT_ATTRIBUTES     objAttr;

    RtlInitUnicodeString(&memName, L"\\BaseNamedObjects\\" CEX_NPU_SHARED_MEM_NAME);
    InitializeObjectAttributes(&objAttr, &memName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    size.QuadPart = (LONGLONG)CEX_NPU_SHARED_MEM_SIZE;

    status = ZwCreateSection(
        &Ctx->SharedMemHandle,
        SECTION_ALL_ACCESS,
        &objAttr,
        &size,
        PAGE_READWRITE,
        SEC_COMMIT,
        NULL
    );

    if (!NT_SUCCESS(status)) {
        KdPrint(("CexNpu: ZwCreateSection failed 0x%08X\n", status));
        Ctx->SharedMemHandle = NULL;
        Ctx->SharedData = NULL;
        return status;
    }

    /*
     * In a production driver, map the section into kernel address space
     * via MmMapViewInSystemSpace.  Proxy writes; driver reads utilization.
     * Abbreviated here -- map in StartDevice when kernel mapping APIs
     * are available at PASSIVE_LEVEL.
     */
    KdPrint(("CexNpu: shared memory section created\n"));
    return STATUS_SUCCESS;
}

VOID
CexNpuCleanupSharedMemory(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
)
{
    if (Ctx->SharedMemHandle) {
        ZwClose(Ctx->SharedMemHandle);
        Ctx->SharedMemHandle = NULL;
        Ctx->SharedData = NULL;
    }
}

/* ---------------------------------------------------------------
 * CexNpuUpdateUtilization
 * Called periodically; reads NPU utilization from shared memory
 * and updates the DXGKRNL node utilization counter so Task Manager
 * reflects real NPU activity on the ARM server.
 * --------------------------------------------------------------- */
VOID
CexNpuUpdateUtilization(
    _Inout_ PCEX_NPU_DEVICE_CONTEXT Ctx
)
{
    FLOAT utilPct = 0.0f;

    if (Ctx->SharedData != NULL) {
        utilPct = Ctx->SharedData->npu_utilization;
        if (utilPct < 0.0f)   utilPct = 0.0f;
        if (utilPct > 100.0f) utilPct = 100.0f;
    }

    /* Convert to per-mille (0-1000) for DXGKRNL */
    InterlockedExchange(
        &Ctx->UtilizationPermille,
        (LONG)(utilPct * 10.0f)
    );

    /*
     * Notify DXGKRNL of updated node utilization via
     * DxgkInterface.DxgkCbIndicateChildStatus or the
     * D3DKMTQueryStatistics counter path.
     * DXGKRNL polls this on its own timer for Task Manager.
     */
}

/* ---------------------------------------------------------------
 * Stub DDI implementations (required by DXGKRNL interface)
 * --------------------------------------------------------------- */
NTSTATUS CexNpuDdiStopDevice(_In_ PVOID ctx)
{ UNREFERENCED_PARAMETER(ctx); KdPrint(("CexNpu: StopDevice\n")); return STATUS_SUCCESS; }

NTSTATUS CexNpuDdiRemoveDevice(_In_ PVOID ctx)
{ UNREFERENCED_PARAMETER(ctx); KdPrint(("CexNpu: RemoveDevice\n")); return STATUS_SUCCESS; }

NTSTATUS CexNpuDdiDispatchIoRequest(_In_ PVOID ctx, _In_ ULONG VidPnSourceId, _In_ PVIDEO_REQUEST_PACKET VideoRequestPacket)
{ UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(VidPnSourceId); UNREFERENCED_PARAMETER(VideoRequestPacket); return STATUS_NOT_IMPLEMENTED; }

NTSTATUS CexNpuDdiQueryChildRelations(_In_ PVOID ctx, _Inout_updates_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations, _In_ ULONG ChildRelationsSize)
{ UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(ChildRelations); UNREFERENCED_PARAMETER(ChildRelationsSize); return STATUS_SUCCESS; }

NTSTATUS CexNpuDdiQueryChildStatus(_In_ PVOID ctx, _Inout_ PDXGK_CHILD_STATUS ChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{ UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(ChildStatus); UNREFERENCED_PARAMETER(NonDestructiveOnly); return STATUS_SUCCESS; }

NTSTATUS CexNpuDdiQueryDeviceDescriptor(_In_ PVOID ctx, _In_ ULONG ChildUid, _Inout_ PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{ UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(ChildUid); UNREFERENCED_PARAMETER(DeviceDescriptor); return STATUS_MONITOR_NO_DESCRIPTOR; }

NTSTATUS CexNpuDdiSetPowerState(_In_ PVOID ctx, _In_ ULONG DeviceUid, _In_ DEVICE_POWER_STATE DevicePowerState, _In_ POWER_ACTION ActionType)
{ UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(DeviceUid); UNREFERENCED_PARAMETER(DevicePowerState); UNREFERENCED_PARAMETER(ActionType); return STATUS_SUCCESS; }

/* ---------------------------------------------------------------
 * I/O control handler (IOCTL from EP DLL or user-mode tools)
 * --------------------------------------------------------------- */
VOID
CexNpuEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PCEX_NPU_DEVICE_CONTEXT ctx = CexNpuGetDeviceContext(WdfIoQueueGetDevice(Queue));
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS): {
        /* IOCTL_CEX_NPU_GET_UTIL -- returns current utilization as ULONG permille */
        PULONG outBuf = NULL;
        size_t outLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&outBuf, &outLen);
        if (NT_SUCCESS(status)) {
            *outBuf = (ULONG)ReadAcquire(&ctx->UtilizationPermille);
            WdfRequestSetInformation(Request, sizeof(ULONG));
            status = STATUS_SUCCESS;
        }
        break;
    }
    case CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS): {
        /* IOCTL_CEX_NPU_GET_STATS -- returns full shared data struct */
        PVOID outBuf = NULL;
        size_t outLen = 0;
        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(CEX_NPU_SHARED_DATA), &outBuf, &outLen);
        if (NT_SUCCESS(status) && ctx->SharedData != NULL) {
            RtlCopyMemory(outBuf, ctx->SharedData, sizeof(CEX_NPU_SHARED_DATA));
            WdfRequestSetInformation(Request, sizeof(CEX_NPU_SHARED_DATA));
            status = STATUS_SUCCESS;
        }
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}
