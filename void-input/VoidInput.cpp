#include "VoidInput.h"
#include "Devices.h"

// Per-instance container id base for input children (project branding). The low
// byte is varied per slot so multiple devices are distinct but stable.
static const GUID kVoidInputContainerBase =
    { 0x61912c91, 0x979a, 0x40e7, { 0xb4, 0x2a, 0xab, 0xf6, 0xd9, 0x17, 0xbf, 0xf3 } };

static BOOLEAN VoidInputIsGamepadType(VOIDINPUT_DEVICE_TYPE type)
{
    return type == VoidInputDeviceXboxOne ||
           type == VoidInputDeviceDS4 ||
           type == VoidInputDeviceDS5;
}

// ---------------------------------------------------------------------------
// Output/feature event channel (device -> host)
//
// VHF invokes our async-operation callback when the OS sends an output report
// (e.g. gamepad rumble) or a feature request. We wrap it as an event, queue it,
// and hand it to a pending IOCTL_VOIDINPUT_GET_EVENT; the host services it and
// calls IOCTL_VOIDINPUT_COMPLETE_EVENT, which completes the VHF operation.
// ---------------------------------------------------------------------------

#define VOIDINPUT_MAX_WAITING_EVENTS 8   // drop-oldest cap if the host never reads

// Satisfy a GET_EVENT request from the head of the waiting queue. Returns TRUE
// if the request was completed (success or error), FALSE if nothing was waiting
// (the caller should then pend the request).
static BOOLEAN VoidInputTryCompleteGetEvent(PVOIDINPUT_FILE_CONTEXT fc, WDFREQUEST Request)
{
    WdfWaitLockAcquire(fc->EventLock, NULL);

    WDFOBJECT op = WdfCollectionGetFirstItem(fc->WaitingEvents);
    if (!op) {
        WdfWaitLockRelease(fc->EventLock);
        return FALSE;
    }
    auto* oc = VoidInputOpGetContext(op);

    VOIDINPUT_EVENT* out = nullptr;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID*)&out, nullptr);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockRelease(fc->EventLock);
        WdfRequestComplete(Request, status);
        return TRUE;
    }

    ULONG len = oc->Packet.reportBufferLen;
    if (len > VOIDINPUT_MAX_REPORT) {
        len = VOIDINPUT_MAX_REPORT;
    }
    RtlZeroMemory(out, sizeof(*out));
    out->Type       = (UINT32)oc->Type;
    out->RequestId  = oc->RequestId;
    out->ReportId   = oc->Packet.reportId;
    out->DataLength = len;
    if (len && oc->Packet.reportBuffer) {
        RtlCopyMemory(out->Data, oc->Packet.reportBuffer, len);
    }

    status = WdfCollectionAdd(fc->OutstandingEvents, op);
    if (NT_SUCCESS(status)) {
        WdfCollectionRemove(fc->WaitingEvents, op);
        WdfWaitLockRelease(fc->EventLock);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*out));
        return TRUE;
    }

    // Could not move it to outstanding; fail the VHF op and drop it.
    WdfCollectionRemove(fc->WaitingEvents, op);
    WdfWaitLockRelease(fc->EventLock);
    if (oc->VhfOp) { VhfAsyncOperationComplete(oc->VhfOp, status); }
    WdfObjectDelete(op);
    WdfRequestComplete(Request, status);
    return TRUE;
}

// Queue a VHF async operation as an event, then try to dispatch it immediately.
static VOID VoidInputDispatchEvent(PVOIDINPUT_FILE_CONTEXT fc, VHFOPERATIONHANDLE vhfOp,
                                   PHID_XFER_PACKET packet, VOIDINPUT_EVENT_TYPE type)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VOIDINPUT_OP_CONTEXT);
    attr.ParentObject = fc->FileObject;

    WDFOBJECT op = nullptr;
    NTSTATUS status = WdfObjectCreate(&attr, &op);
    if (!NT_SUCCESS(status)) {
        if (vhfOp) { VhfAsyncOperationComplete(vhfOp, STATUS_INSUFFICIENT_RESOURCES); }
        return;
    }
    auto* oc = VoidInputOpGetContext(op);
    oc->Type      = type;
    oc->VhfOp     = vhfOp;
    oc->RequestId = (ULONG)InterlockedIncrement(&fc->NextRequestId);
    oc->Packet    = packet ? *packet : HID_XFER_PACKET{};

    WdfWaitLockAcquire(fc->EventLock, NULL);
    if (!fc->VhfHandle) {            // device is tearing down
        WdfWaitLockRelease(fc->EventLock);
        if (vhfOp) { VhfAsyncOperationComplete(vhfOp, STATUS_DEVICE_REMOVED); }
        WdfObjectDelete(op);
        return;
    }
    status = WdfCollectionAdd(fc->WaitingEvents, op);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockRelease(fc->EventLock);
        if (vhfOp) { VhfAsyncOperationComplete(vhfOp, status); }
        WdfObjectDelete(op);
        return;
    }
    // Bound the queue if the host is not reading events: drop the oldest.
    while (WdfCollectionGetCount(fc->WaitingEvents) > VOIDINPUT_MAX_WAITING_EVENTS) {
        WDFOBJECT old = WdfCollectionGetFirstItem(fc->WaitingEvents);
        auto* ooc = VoidInputOpGetContext(old);
        if (ooc->VhfOp) { VhfAsyncOperationComplete(ooc->VhfOp, STATUS_CANCELLED); }
        WdfCollectionRemove(fc->WaitingEvents, old);
        WdfObjectDelete(old);
    }
    WdfWaitLockRelease(fc->EventLock);

    // Service a pending GET_EVENT for this file, if one is waiting.
    WDFREQUEST request = nullptr;
    if (WdfIoQueueRetrieveRequestByFileObject(fc->DeviceContext->GetEventQueue,
                                              fc->FileObject, &request) == STATUS_SUCCESS) {
        if (!VoidInputTryCompleteGetEvent(fc, request)) {
            if (!NT_SUCCESS(WdfRequestRequeue(request))) {
                WdfRequestComplete(request, STATUS_CANCELLED);
            }
        }
    }
}

_Function_class_(EVT_VHF_ASYNC_OPERATION)
static VOID VoidInputEvtVhfWriteReport(PVOID VhfClientContext, VHFOPERATIONHANDLE VhfOperationHandle,
                                       PVOID VhfOperationContext, PHID_XFER_PACKET HidTransferPacket)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);
    VoidInputDispatchEvent((PVOIDINPUT_FILE_CONTEXT)VhfClientContext, VhfOperationHandle,
                           HidTransferPacket, VoidInputEventWriteReport);
}

// Dequeue a handed-out op by request id; the caller completes it.
static WDFOBJECT VoidInputTakeOutstandingById(PVOIDINPUT_FILE_CONTEXT fc, ULONG requestId)
{
    WdfWaitLockAcquire(fc->EventLock, NULL);
    ULONG count = WdfCollectionGetCount(fc->OutstandingEvents);
    for (ULONG i = 0; i < count; ++i) {
        WDFOBJECT op = WdfCollectionGetItem(fc->OutstandingEvents, i);
        if (VoidInputOpGetContext(op)->RequestId == requestId) {
            WdfCollectionRemoveItem(fc->OutstandingEvents, i);
            WdfWaitLockRelease(fc->EventLock);
            return op;
        }
    }
    WdfWaitLockRelease(fc->EventLock);
    return nullptr;
}

// Cancel and free all queued/outstanding events (device teardown).
static VOID VoidInputRundownEvents(PVOIDINPUT_FILE_CONTEXT fc)
{
    WdfWaitLockAcquire(fc->EventLock, NULL);
    WDFOBJECT op;
    while ((op = WdfCollectionGetFirstItem(fc->WaitingEvents)) != nullptr) {
        auto* oc = VoidInputOpGetContext(op);
        if (oc->VhfOp) { VhfAsyncOperationComplete(oc->VhfOp, STATUS_CANCELLED); }
        WdfCollectionRemove(fc->WaitingEvents, op);
        WdfObjectDelete(op);
    }
    while ((op = WdfCollectionGetFirstItem(fc->OutstandingEvents)) != nullptr) {
        auto* oc = VoidInputOpGetContext(op);
        if (oc->VhfOp) { VhfAsyncOperationComplete(oc->VhfOp, STATUS_CANCELLED); }
        WdfCollectionRemove(fc->OutstandingEvents, op);
        WdfObjectDelete(op);
    }
    WdfWaitLockRelease(fc->EventLock);
}

// ---------------------------------------------------------------------------
// Device create / destroy
//
// Create reserves a live slot under the lock (enforcing the capacity rules),
// then builds the VHF config from the type's compiled-in descriptor and calls
// VhfCreate / VhfStart outside the lock (they can be heavy). On failure the
// reservation is rolled back.
// ---------------------------------------------------------------------------
static NTSTATUS VoidInputDoCreate(PVOIDINPUT_DEVICE_CONTEXT dc,
                                  PVOIDINPUT_FILE_CONTEXT fc,
                                  const VOIDINPUT_CREATE* req)
{
    if (fc->VhfHandle) {
        return STATUS_INVALID_DEVICE_STATE;   // one device per handle
    }

    const VOIDINPUT_DEVICE_DESC* desc =
        VoidInputGetDeviceDesc((VOIDINPUT_DEVICE_TYPE)req->Type);
    if (!desc) {
        VOID_LOG("CREATE type=%u: not implemented", req->Type);
        return STATUS_NOT_SUPPORTED;
    }

    LONG slot = -1;
    WdfWaitLockAcquire(dc->Lock, NULL);
    {
        UINT32 total = 0, gamepads = 0;
        LONG   firstFree = -1;
        for (LONG i = 0; i < VOIDINPUT_MAX_DEVICES; ++i) {
            if (dc->Slots[i].InUse) {
                ++total;
                if (VoidInputIsGamepadType(dc->Slots[i].Type)) {
                    ++gamepads;
                }
                if (desc->Singleton && dc->Slots[i].Type == desc->Type) {
                    WdfWaitLockRelease(dc->Lock);
                    VOID_LOG("CREATE type=%u: singleton already present", req->Type);
                    return STATUS_RESOURCE_IN_USE;
                }
            } else if (firstFree < 0) {
                firstFree = i;
            }
        }
        if (total >= VOIDINPUT_MAX_DEVICES || firstFree < 0) {
            WdfWaitLockRelease(dc->Lock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (desc->IsGamepad && gamepads >= 4) {
            WdfWaitLockRelease(dc->Lock);
            VOID_LOG("CREATE type=%u: gamepad cap (4) reached", req->Type);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        slot = firstFree;
        dc->Slots[slot].InUse = TRUE;          // reserve
        dc->Slots[slot].Type  = desc->Type;
        dc->Slots[slot].Vid   = 0;
        dc->Slots[slot].Pid   = 0;
    }
    WdfWaitLockRelease(dc->Lock);

    USHORT vid = req->VendorId      ? req->VendorId      : desc->DefaultVid;
    USHORT pid = req->ProductId     ? req->ProductId     : desc->DefaultPid;
    USHORT ver = req->VersionNumber ? req->VersionNumber : desc->DefaultVersion;

    fc->VhfConfig.VhfClientContext       = fc;
    fc->VhfConfig.VendorID               = vid;
    fc->VhfConfig.ProductID              = pid;
    fc->VhfConfig.VersionNumber          = ver;
    fc->VhfConfig.ReportDescriptor       = (PUCHAR)desc->ReportDescriptor;
    fc->VhfConfig.ReportDescriptorLength = desc->ReportDescriptorLength;

    GUID container = kVoidInputContainerBase;
    container.Data4[7] = (UCHAR)(container.Data4[7] + (UCHAR)slot);
    fc->VhfConfig.ContainerID = container;

    // Register the VHF output/feature callbacks this type opts into. Mouse and
    // keyboard are input-only (none); the Xbox pad registers write-report for
    // rumble. (DS4/DS5 add the feature-report callbacks later.)
    if (desc->Events & VOIDINPUT_EVT_WRITE_REPORT) {
        fc->VhfConfig.EvtVhfAsyncOperationWriteReport = VoidInputEvtVhfWriteReport;
    }

    NTSTATUS status = VhfCreate(&fc->VhfConfig, &fc->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("VhfCreate type=%u failed 0x%08X", req->Type, status);
        fc->VhfHandle = nullptr;
        WdfWaitLockAcquire(dc->Lock, NULL);
        dc->Slots[slot].InUse = FALSE;
        WdfWaitLockRelease(dc->Lock);
        return status;
    }

    status = VhfStart(fc->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("VhfStart type=%u failed 0x%08X", req->Type, status);
        VhfDelete(fc->VhfHandle, TRUE);
        fc->VhfHandle = nullptr;
        WdfWaitLockAcquire(dc->Lock, NULL);
        dc->Slots[slot].InUse = FALSE;
        WdfWaitLockRelease(dc->Lock);
        return status;
    }

    WdfWaitLockAcquire(dc->Lock, NULL);
    dc->Slots[slot].Vid = vid;
    dc->Slots[slot].Pid = pid;
    WdfWaitLockRelease(dc->Lock);

    fc->Type            = desc->Type;
    fc->NumberedReports = desc->NumberedReports;
    fc->LiveIndex       = slot;

    VOID_LOG("Created device type=%u vid=0x%04X pid=0x%04X at slot %ld",
             req->Type, vid, pid, slot);
    return STATUS_SUCCESS;
}

static void VoidInputDoDestroy(PVOIDINPUT_DEVICE_CONTEXT dc, PVOIDINPUT_FILE_CONTEXT fc)
{
    if (!fc->VhfHandle) {
        return;   // idempotent: no device is a no-op
    }
    // Stop new events from queuing, cancel any in flight, then delete the device.
    VHFHANDLE vhf = fc->VhfHandle;
    WdfWaitLockAcquire(fc->EventLock, NULL);
    fc->VhfHandle = nullptr;
    WdfWaitLockRelease(fc->EventLock);
    VoidInputRundownEvents(fc);

    VhfDelete(vhf, TRUE);

    if (fc->LiveIndex >= 0) {
        WdfWaitLockAcquire(dc->Lock, NULL);
        dc->Slots[fc->LiveIndex].InUse = FALSE;
        WdfWaitLockRelease(dc->Lock);
        fc->LiveIndex = -1;
    }
    fc->Type = VoidInputDeviceNone;
}

// ---------------------------------------------------------------------------
// DriverEntry / device add
// ---------------------------------------------------------------------------
EXTERN_C NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT pDriverObject,
                              _In_ PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG     config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, VoidInputDeviceAdd);
    config.DriverPoolTag = VOIDINPUT_POOL_TAG;

    NTSTATUS status = WdfDriverCreate(pDriverObject, pRegistryPath, &attributes,
                                      &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDriverCreate failed 0x%08X", status);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS VoidInputDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    // We sit as an upper filter on the VHF device (which acts as the function
    // driver for the HID children we spawn).
    WdfFdoInitSetFilter(pDeviceInit);

    // Each open handle gets its own file context = one virtual HID device.
    WDF_OBJECT_ATTRIBUTES fileAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttr, VOIDINPUT_FILE_CONTEXT);
    fileAttr.SynchronizationScope = WdfSynchronizationScopeNone;

    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, VoidInputFileCreate, VoidInputFileClose,
                               WDF_NO_EVENT_CALLBACK);
    fileConfig.AutoForwardCleanupClose = WdfFalse;
    WdfDeviceInitSetFileObjectConfig(pDeviceInit, &fileConfig, &fileAttr);

    WDF_OBJECT_ATTRIBUTES deviceAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttr, VOIDINPUT_DEVICE_CONTEXT);

    WDFDEVICE device = nullptr;
    NTSTATUS status = WdfDeviceCreate(&pDeviceInit, &deviceAttr, &device);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    auto* dc = VoidInputDeviceGetContext(device);
    dc->Device = device;
    RtlZeroMemory(dc->Slots, sizeof(dc->Slots));

    WDF_OBJECT_ATTRIBUTES lockAttr;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);
    lockAttr.ParentObject = device;
    status = WdfWaitLockCreate(&lockAttr, &dc->Lock);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfWaitLockCreate failed 0x%08X", status);
        return status;
    }

    // Default queue: control IOCTLs and input-report writes.
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged       = WdfFalse;
    queueConfig.EvtIoDeviceControl = VoidInputIoDeviceControl;
    queueConfig.EvtIoWrite         = VoidInputIoWrite;

    WDFQUEUE queue = nullptr;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfIoQueueCreate failed 0x%08X", status);
        return status;
    }

    // Manual queue holding pended GET_EVENT requests until an output/feature
    // event arrives for that file.
    WDF_IO_QUEUE_CONFIG eventQueueConfig;
    WDF_IO_QUEUE_CONFIG_INIT(&eventQueueConfig, WdfIoQueueDispatchManual);
    eventQueueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &eventQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &dc->GetEventQueue);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfIoQueueCreate (events) failed 0x%08X", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VOIDINPUT, nullptr);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDeviceCreateDeviceInterface failed 0x%08X", status);
        return status;
    }

    VOID_LOG("Device added (VHF enumerator ready)");
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// File object lifetime: open the lower VHF device per handle. The virtual HID
// device itself is created later, on IOCTL_VOIDINPUT_CREATE.
// ---------------------------------------------------------------------------
_Use_decl_annotations_
VOID VoidInputFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    auto* fc = VoidInputFileGetContext(FileObject);
    RtlZeroMemory(fc, sizeof(*fc));
    fc->FileObject    = FileObject;
    fc->Type          = VoidInputDeviceNone;
    fc->LiveIndex     = -1;
    fc->DeviceContext = VoidInputDeviceGetContext(Device);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = FileObject;

    // Per-handle output/feature event channel.
    NTSTATUS status = WdfWaitLockCreate(&attr, &fc->EventLock);
    if (NT_SUCCESS(status)) { status = WdfCollectionCreate(&attr, &fc->WaitingEvents); }
    if (NT_SUCCESS(status)) { status = WdfCollectionCreate(&attr, &fc->OutstandingEvents); }
    if (!NT_SUCCESS(status)) {
        VOID_LOG("event-channel setup failed 0x%08X", status);
        WdfRequestComplete(Request, status);
        return;
    }

    status = WdfIoTargetCreate(Device, &attr, &fc->VhfIoTarget);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfIoTargetCreate failed 0x%08X", status);
        fc->VhfIoTarget = nullptr;
        WdfRequestComplete(Request, status);
        return;
    }

    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&openParams, NULL);
    status = WdfIoTargetOpen(fc->VhfIoTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfIoTargetOpen (VHF) failed 0x%08X", status);
        WdfObjectDelete(fc->VhfIoTarget);
        fc->VhfIoTarget = nullptr;
        WdfRequestComplete(Request, status);
        return;
    }

    // Seed the VHF config with the lower target handle; the report descriptor and
    // identity are filled from the device type at CREATE.
    VHF_CONFIG_INIT(&fc->VhfConfig,
                    WdfIoTargetWdmGetTargetFileHandle(fc->VhfIoTarget), 0, NULL);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID VoidInputFileClose(WDFFILEOBJECT FileObject)
{
    auto* fc = VoidInputFileGetContext(FileObject);
    auto* dc = VoidInputDeviceGetContext(WdfFileObjectGetDevice(FileObject));

    // Tears down the HID device (if any) and frees its live slot.
    VoidInputDoDestroy(dc, fc);

    if (fc->VhfIoTarget) {
        WdfObjectDelete(fc->VhfIoTarget);
        fc->VhfIoTarget = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Control IOCTLs
// ---------------------------------------------------------------------------
_Use_decl_annotations_
VOID VoidInputIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                              size_t OutputBufferLength, size_t InputBufferLength,
                              ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    auto* dc = VoidInputDeviceGetContext(WdfIoQueueGetDevice(Queue));
    auto* fc = VoidInputFileGetContext(WdfRequestGetFileObject(Request));

    NTSTATUS  status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info   = 0;

    switch (IoControlCode) {
    case IOCTL_VOIDINPUT_VERSION: {
        ULONG* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) { *out = VOIDINPUT_VERSION; info = sizeof(ULONG); }
        break;
    }
    case IOCTL_VOIDINPUT_LIST: {
        VOIDINPUT_STATE* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(out, sizeof(*out));
            WdfWaitLockAcquire(dc->Lock, NULL);
            for (UINT32 i = 0; i < VOIDINPUT_MAX_DEVICES; ++i) {
                if (dc->Slots[i].InUse) {
                    VOIDINPUT_ENTRY* e = &out->Entries[out->Count++];
                    e->Type      = dc->Slots[i].Type;
                    e->VendorId  = dc->Slots[i].Vid;
                    e->ProductId = dc->Slots[i].Pid;
                }
            }
            WdfWaitLockRelease(dc->Lock);
            info = sizeof(*out);
        }
        break;
    }
    case IOCTL_VOIDINPUT_CREATE: {
        VOIDINPUT_CREATE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) {
            status = VoidInputDoCreate(dc, fc, in);
        }
        break;
    }
    case IOCTL_VOIDINPUT_DESTROY: {
        VoidInputDoDestroy(dc, fc);
        status = STATUS_SUCCESS;   // idempotent
        break;
    }
    case IOCTL_VOIDINPUT_GET_EVENT: {
        if (!fc->VhfHandle) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        if (VoidInputTryCompleteGetEvent(fc, Request)) {
            return;   // completed inside (with an event or an error)
        }
        // Nothing waiting; pend it until an event arrives for this file.
        status = WdfRequestForwardToIoQueue(Request, dc->GetEventQueue);
        if (NT_SUCCESS(status)) {
            return;   // pended
        }
        break;        // forward failed -> complete with the error below
    }
    case IOCTL_VOIDINPUT_COMPLETE_EVENT: {
        VOIDINPUT_EVENT_COMPLETE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID*)&in, nullptr);
        if (!NT_SUCCESS(status)) {
            break;
        }
        WDFOBJECT op = VoidInputTakeOutstandingById(fc, in->RequestId);
        if (!op) {
            status = STATUS_NOT_FOUND;
            break;
        }
        auto* oc = VoidInputOpGetContext(op);
        // For a feature read the host returns data; copy it into the VHF packet
        // before completing. (Feature reports arrive with DS4/DS5.)
        if (oc->Type == VoidInputEventGetFeature && oc->Packet.reportBuffer && in->Status == 0) {
            ULONG n = in->DataLength;
            if (n > oc->Packet.reportBufferLen) { n = oc->Packet.reportBufferLen; }
            if (n) { RtlCopyMemory(oc->Packet.reportBuffer, in->Data, n); }
        }
        if (oc->VhfOp) {
            VhfAsyncOperationComplete(oc->VhfOp, (in->Status == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
        }
        WdfObjectDelete(op);
        status = STATUS_SUCCESS;
        break;
    }
    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

// ---------------------------------------------------------------------------
// Input report hot path: WriteFile -> VhfReadReportSubmit. Valid only after a
// device has been created on this handle.
// ---------------------------------------------------------------------------
_Use_decl_annotations_
VOID VoidInputIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
    UNREFERENCED_PARAMETER(Queue);

    auto* fc = VoidInputFileGetContext(WdfRequestGetFileObject(Request));
    if (!fc->VhfHandle) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    PUCHAR buffer = nullptr;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, 1, (PVOID*)&buffer, nullptr);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    if (Length > MAXUINT32) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    HID_XFER_PACKET packet;
    packet.reportId       = fc->NumberedReports ? buffer[0] : 0;
    packet.reportBuffer   = buffer;
    packet.reportBufferLen = (ULONG)Length;

    status = VhfReadReportSubmit(fc->VhfHandle, &packet);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}
