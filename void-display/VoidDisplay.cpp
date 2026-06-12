#include "VoidDisplay.h"

using namespace Microsoft::WRL;

// Per-monitor container id derived from a fixed base so monitors are distinct
// but stable across reboots.
static const GUID kVoidContainerBase =
    { 0x61912c91, 0x979a, 0x40e7, { 0xb4, 0x2a, 0xab, 0xf6, 0xd9, 0x17, 0xbf, 0xf3 } };

// ---------------------------------------------------------------------------
// Mode-list helpers
// ---------------------------------------------------------------------------
static void FillMonitorMode(IDDCX_MONITOR_MODE& m, const VOID_MODE_DESC& d)
{
    m.Size   = sizeof(IDDCX_MONITOR_MODE);
    m.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    // Monitor modes require vSyncFreqDivider == 0.
    m.MonitorVideoSignalInfo = VoidCreateSignalInfo(d.Width, d.Height, d.RefreshHz, 0);
}

static void FillTargetMode(IDDCX_TARGET_MODE& m, const VOID_MODE_DESC& d)
{
    m.Size = sizeof(IDDCX_TARGET_MODE);
    // Target modes require vSyncFreqDivider != 0.
    m.TargetVideoSignalInfo.targetVideoSignalInfo =
        VoidCreateSignalInfo(d.Width, d.Height, d.RefreshHz, 1);
    m.RequiredBandwidth = (UINT64)d.Width * (UINT64)d.Height * (UINT64)d.RefreshHz * 4ull;
}

// ---------------------------------------------------------------------------
// Preferred render GPU
//
// On a multi-GPU headless host the OS would otherwise auto-pick the adapter
// that composes the virtual desktop. We let the operator pin it by DXGI vendor
// id via the driver's Parameters key:
//   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\VoidDisplay\
//        Parameters\PreferredRenderAdapterVendorId  (REG_DWORD)
//   0 or absent = auto; 0x10DE = NVIDIA, 0x1002 = AMD, 0x8086 = Intel.
// Returns true and fills outLuid if a matching adapter is found.
// ---------------------------------------------------------------------------
static bool VoidResolveRenderAdapterLuid(LUID* outLuid)
{
    DWORD vendorId = 0;
    DWORD cb = sizeof(vendorId);
    LSTATUS rs = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\VoidDisplay\\Parameters",
        L"PreferredRenderAdapterVendorId",
        RRF_RT_REG_DWORD, nullptr, &vendorId, &cb);
    if (rs != ERROR_SUCCESS || vendorId == 0) {
        return false;  // auto
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.VendorId == vendorId &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            *outLuid = desc.AdapterLuid;
            VOID_LOG("Preferred render adapter: vendor 0x%04X -> LUID %ld:%lu",
                     vendorId, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            return true;
        }
        adapter.Reset();
    }

    VOID_LOG("No DXGI adapter for preferred vendor 0x%04X; using auto", vendorId);
    return false;
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
    WDF_DRIVER_CONFIG_INIT(&config, VoidDisplayDeviceAdd);
    config.DriverPoolTag = 'dDoV';

    NTSTATUS status = WdfDriverCreate(pDriverObject, pRegistryPath, &attributes,
                                      &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDriverCreate failed 0x%08X", status);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDeviceD0Entry = VoidDisplayDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpPower);

    IDD_CX_CLIENT_CONFIG iddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished               = VoidDisplayAdapterInitFinished;
    iddConfig.EvtIddCxParseMonitorDescription           = VoidDisplayParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VoidDisplayMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes           = VoidDisplayMonitorQueryModes;
    iddConfig.EvtIddCxAdapterCommitModes                = VoidDisplayAdapterCommitModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain            = VoidDisplayMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain          = VoidDisplayMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &iddConfig);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("IddCxDeviceInitConfig failed 0x%08X", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES deviceAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttr, VoidDeviceContextWrapper);
    deviceAttr.EvtCleanupCallback = [](WDFOBJECT obj) {
        auto* w = WdfObjectGet_VoidDeviceContextWrapper(obj);
        if (w) { w->Cleanup(); }
    };

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&pDeviceInit, &deviceAttr, &device);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("IddCxDeviceInitialize failed 0x%08X", status);
        return status;
    }

    auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(device);
    wrapper->pContext = new VoidDisplayDevice(device);

    // Control IOCTL queue.
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = VoidDisplayIoDeviceControl;

    WDFQUEUE queue = nullptr;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfIoQueueCreate failed 0x%08X", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_VOIDDISPLAY, nullptr);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("WdfDeviceCreateDeviceInterface failed 0x%08X", status);
        return status;
    }

    VOID_LOG("Device added");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(Device);
    if (wrapper && wrapper->pContext) {
        wrapper->pContext->InitAdapter();
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Control IOCTLs
// ---------------------------------------------------------------------------
_Use_decl_annotations_
VOID VoidDisplayIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                                size_t OutputBufferLength, size_t InputBufferLength,
                                ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(device);
    VoidDisplayDevice* dev = wrapper ? wrapper->pContext : nullptr;
    if (!dev) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    NTSTATUS  status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info   = 0;

    switch (IoControlCode) {
    case IOCTL_VOIDDISPLAY_VERSION: {
        ULONG* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) { *out = VOIDDISPLAY_VERSION; info = sizeof(ULONG); }
        break;
    }
    case IOCTL_VOIDDISPLAY_ADD: {
        VOIDDISPLAY_MODE* in = nullptr;
        ULONG* out = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VOIDDISPLAY_MODE), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) {
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&out, nullptr);
        }
        if (NT_SUCCESS(status)) {
            UINT32 idx = 0;
            status = dev->AddMonitor(*in, &idx);
            if (NT_SUCCESS(status)) { *out = idx; info = sizeof(ULONG); }
        }
        break;
    }
    case IOCTL_VOIDDISPLAY_REMOVE: {
        ULONG* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) { status = dev->RemoveMonitor(*in); }
        break;
    }
    case IOCTL_VOIDDISPLAY_SET_MODE: {
        VOIDDISPLAY_SET_MODE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VOIDDISPLAY_SET_MODE), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) { status = dev->SetMode(in->Index, in->Mode); }
        break;
    }
    case IOCTL_VOIDDISPLAY_LIST: {
        VOIDDISPLAY_STATE* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VOIDDISPLAY_STATE), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) { dev->GetState(out); info = sizeof(VOIDDISPLAY_STATE); }
        break;
    }
    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
}

// ---------------------------------------------------------------------------
// IddCx callbacks
// ---------------------------------------------------------------------------
_Use_decl_annotations_
NTSTATUS VoidDisplayAdapterInitFinished(IDDCX_ADAPTER AdapterObject,
                                        const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(AdapterObject);
    if (wrapper && wrapper->pContext && NT_SUCCESS(pInArgs->AdapterInitStatus)) {
        wrapper->pContext->FinishInit(AdapterObject);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayAdapterCommitModes(IDDCX_ADAPTER AdapterObject,
                                       const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    // Accept whatever path/mode set the OS commits.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
                                            IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    pOutArgs->MonitorModeBufferOutputCount = g_VoidDefaultModeCount;
    if (pInArgs->MonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;  // size query
    }
    if (pInArgs->MonitorModeBufferInputCount < g_VoidDefaultModeCount) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < g_VoidDefaultModeCount; ++i) {
        FillMonitorMode(pInArgs->pMonitorModes[i], g_VoidDefaultModes[i]);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject,
                                           const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                           IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    pOutArgs->DefaultMonitorModeBufferOutputCount = g_VoidDefaultModeCount;
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < g_VoidDefaultModeCount) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < g_VoidDefaultModeCount; ++i) {
        FillMonitorMode(pInArgs->pDefaultMonitorModes[i], g_VoidDefaultModes[i]);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayMonitorQueryModes(IDDCX_MONITOR MonitorObject,
                                      const IDARG_IN_QUERYTARGETMODES* pInArgs,
                                      IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    pOutArgs->TargetModeBufferOutputCount = g_VoidDefaultModeCount;
    if (pInArgs->TargetModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (pInArgs->TargetModeBufferInputCount < g_VoidDefaultModeCount) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < g_VoidDefaultModeCount; ++i) {
        FillTargetMode(pInArgs->pTargetModes[i], g_VoidDefaultModes[i]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject,
                                           const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* mw = WdfObjectGet_VoidMonitorContextWrapper(MonitorObject);
    if (mw && mw->Device) {
        mw->Device->AssignSwapChain(mw->Index, pInArgs->hSwapChain,
                                    pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* mw = WdfObjectGet_VoidMonitorContextWrapper(MonitorObject);
    if (mw && mw->Device) {
        mw->Device->UnassignSwapChain(mw->Index);
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// VoidDeviceContextWrapper
// ---------------------------------------------------------------------------
void VoidDeviceContextWrapper::Cleanup()
{
    if (pContext) {
        delete pContext;
        pContext = nullptr;
    }
}

// ---------------------------------------------------------------------------
// VoidDisplayDevice
// ---------------------------------------------------------------------------
VoidDisplayDevice::VoidDisplayDevice(WDFDEVICE wdfDevice)
    : m_WdfDevice(wdfDevice), m_Adapter(nullptr), m_Lock(nullptr), m_InitStarted(false)
{
    for (UINT32 i = 0; i < VOIDDISPLAY_MAX_DISPLAYS; ++i) {
        m_Slots[i].InUse   = false;
        m_Slots[i].Mode    = VOIDDISPLAY_MODE{};
        m_Slots[i].Monitor = nullptr;
    }

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = wdfDevice;
    WdfWaitLockCreate(&attr, &m_Lock);
}

VoidDisplayDevice::~VoidDisplayDevice()
{
    // SwapChainProcessor destructors join their threads as the slots tear down.
}

void VoidDisplayDevice::InitAdapter()
{
    if (m_InitStarted) {
        return;
    }
    m_InitStarted = true;

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size                 = sizeof(caps);
    caps.MaxMonitorsSupported = VOIDDISPLAY_MAX_DISPLAYS;

    IDDCX_ENDPOINT_VERSION firmware = {};
    firmware.Size     = sizeof(firmware);
    firmware.MajorVer = 1;
    IDDCX_ENDPOINT_VERSION hardware = {};
    hardware.Size     = sizeof(hardware);
    hardware.MajorVer = 1;

    caps.EndPointDiagnostics.Size                      = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport              = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType          = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Void Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Void Virtual Driver";
    caps.EndPointDiagnostics.pEndPointModelName        = L"VoidDisplay";
    caps.EndPointDiagnostics.pFirmwareVersion          = &firmware;
    caps.EndPointDiagnostics.pHardwareVersion          = &hardware;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, VoidDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init = {};
    init.WdfDevice        = m_WdfDevice;
    init.pCaps            = &caps;
    init.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT initOut = {};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
    if (NT_SUCCESS(status)) {
        auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(initOut.AdapterObject);
        if (wrapper) {
            wrapper->pContext = this;  // shared, non-owning (only the device wrapper deletes)
        }
    } else {
        VOID_LOG("IddCxAdapterInitAsync failed 0x%08X", status);
        m_InitStarted = false;
    }
}

void VoidDisplayDevice::FinishInit(IDDCX_ADAPTER adapter)
{
    m_Adapter = adapter;
    VOID_LOG("Adapter init finished");

    // Pin the render/parent GPU if configured. Best done before any monitor is
    // added (monitors are created on-demand via IOCTL, so here is the right spot).
    LUID luid = {};
    if (VoidResolveRenderAdapterLuid(&luid) &&
        IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
        IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
        in.PreferredRenderAdapter = luid;
        IddCxAdapterSetRenderAdapter(m_Adapter, &in);
        VOID_LOG("Pinned render adapter");
    }

    // TODO: restore persisted displays from the registry here.
}

NTSTATUS VoidDisplayDevice::CreateMonitorLocked(UINT32 index, const VOIDDISPLAY_MODE& mode)
{
    UINT8 edid[VOIDDISPLAY_EDID_SIZE];
    VoidBuildEdid(edid, 0x56564400u + index);  // "VVD" + slot

    IDDCX_MONITOR_INFO info = {};
    info.Size                       = sizeof(info);
    info.MonitorType                = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex             = index;
    info.MonitorDescription.Size    = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type    = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = VOIDDISPLAY_EDID_SIZE;
    info.MonitorDescription.pData   = edid;
    info.MonitorContainerId         = kVoidContainerBase;
    info.MonitorContainerId.Data4[7] = (UINT8)(kVoidContainerBase.Data4[7] + index);

    WDF_OBJECT_ATTRIBUTES monAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttr, VoidMonitorContextWrapper);

    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &monAttr;
    create.pMonitorInfo     = &info;

    IDARG_OUT_MONITORCREATE createOut = {};
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &create, &createOut);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("IddCxMonitorCreate slot %u failed 0x%08X", index, status);
        return status;
    }

    auto* mw = WdfObjectGet_VoidMonitorContextWrapper(createOut.MonitorObject);
    if (mw) {
        mw->Device = this;
        mw->Index  = index;
    }

    m_Slots[index].Monitor = createOut.MonitorObject;
    m_Slots[index].Mode    = mode;
    m_Slots[index].InUse   = true;

    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    status = IddCxMonitorArrival(createOut.MonitorObject, &arrivalOut);
    if (!NT_SUCCESS(status)) {
        VOID_LOG("IddCxMonitorArrival slot %u failed 0x%08X", index, status);
        m_Slots[index].InUse   = false;
        m_Slots[index].Monitor = nullptr;
        return status;
    }

    VOID_LOG("Monitor added slot %u %ux%u@%u", index, mode.Width, mode.Height, mode.RefreshHz);
    return STATUS_SUCCESS;
}

void VoidDisplayDevice::DestroyMonitorLocked(UINT32 index)
{
    if (!m_Slots[index].InUse) {
        return;
    }

    m_Slots[index].Processor.reset();  // stop frame processing first

    if (m_Slots[index].Monitor) {
        NTSTATUS status = IddCxMonitorDeparture(m_Slots[index].Monitor);
        if (!NT_SUCCESS(status)) {
            VOID_LOG("IddCxMonitorDeparture slot %u failed 0x%08X", index, status);
        }
    }

    m_Slots[index].Monitor = nullptr;
    m_Slots[index].InUse   = false;
    VOID_LOG("Monitor removed slot %u", index);
}

NTSTATUS VoidDisplayDevice::AddMonitor(const VOIDDISPLAY_MODE& mode, UINT32* outIndex)
{
    WdfWaitLockAcquire(m_Lock, NULL);

    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (m_Adapter != nullptr) {
        UINT32 slot = VOIDDISPLAY_MAX_DISPLAYS;
        for (UINT32 i = 0; i < VOIDDISPLAY_MAX_DISPLAYS; ++i) {
            if (!m_Slots[i].InUse) { slot = i; break; }
        }

        if (slot == VOIDDISPLAY_MAX_DISPLAYS) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            VOIDDISPLAY_MODE m = mode;
            if (m.Width == 0 || m.Height == 0 || m.RefreshHz == 0) {
                m.Width = VOID_DEFAULT_WIDTH;
                m.Height = VOID_DEFAULT_HEIGHT;
                m.RefreshHz = VOID_DEFAULT_REFRESH;
            }
            status = CreateMonitorLocked(slot, m);
            if (NT_SUCCESS(status) && outIndex) {
                *outIndex = slot;
            }
        }
    }

    WdfWaitLockRelease(m_Lock);
    return status;
}

NTSTATUS VoidDisplayDevice::RemoveMonitor(UINT32 index)
{
    if (index >= VOIDDISPLAY_MAX_DISPLAYS) {
        return STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockAcquire(m_Lock, NULL);
    DestroyMonitorLocked(index);
    WdfWaitLockRelease(m_Lock);
    return STATUS_SUCCESS;
}

NTSTATUS VoidDisplayDevice::SetMode(UINT32 index, const VOIDDISPLAY_MODE& mode)
{
    if (index >= VOIDDISPLAY_MAX_DISPLAYS) {
        return STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockAcquire(m_Lock, NULL);
    NTSTATUS status = STATUS_NOT_FOUND;
    if (m_Slots[index].InUse) {
        m_Slots[index].Mode = mode;
        status = STATUS_SUCCESS;
        // TODO: drive an actual mode switch via IddCxMonitorUpdateModes + path commit.
    }
    WdfWaitLockRelease(m_Lock);
    return status;
}

void VoidDisplayDevice::GetState(VOIDDISPLAY_STATE* out)
{
    WdfWaitLockAcquire(m_Lock, NULL);
    out->Count = 0;
    for (UINT32 i = 0; i < VOIDDISPLAY_MAX_DISPLAYS; ++i) {
        out->Entries[i].InUse = m_Slots[i].InUse ? 1u : 0u;
        out->Entries[i].Mode  = m_Slots[i].Mode;
        if (m_Slots[i].InUse) {
            out->Count++;
        }
    }
    WdfWaitLockRelease(m_Lock);
}

void VoidDisplayDevice::AssignSwapChain(UINT32 index, IDDCX_SWAPCHAIN swapChain,
                                        LUID renderAdapter, HANDLE newFrameEvent)
{
    WdfWaitLockAcquire(m_Lock, NULL);
    if (index < VOIDDISPLAY_MAX_DISPLAYS && m_Slots[index].InUse) {
        m_Slots[index].Processor.reset();

        auto device = std::make_shared<Direct3DDevice>(renderAdapter);
        if (SUCCEEDED(device->Init())) {
            m_Slots[index].Processor =
                std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
        } else {
            VOID_LOG("Direct3DDevice Init failed for slot %u", index);
        }
    }
    WdfWaitLockRelease(m_Lock);
}

void VoidDisplayDevice::UnassignSwapChain(UINT32 index)
{
    WdfWaitLockAcquire(m_Lock, NULL);
    if (index < VOIDDISPLAY_MAX_DISPLAYS) {
        m_Slots[index].Processor.reset();
    }
    WdfWaitLockRelease(m_Lock);
}

// ---------------------------------------------------------------------------
// Direct3DDevice
// ---------------------------------------------------------------------------
Direct3DDevice::Direct3DDevice(LUID adapterLuid) : AdapterLuid(adapterLuid) {}
Direct3DDevice::Direct3DDevice() : AdapterLuid{} {}

HRESULT Direct3DDevice::Init()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) {
        return hr;
    }

    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) {
        return hr;
    }

    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &Device, &featureLevel, &DeviceContext);
    return hr;
}

// ---------------------------------------------------------------------------
// SwapChainProcessor
// ---------------------------------------------------------------------------
SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
                                       std::shared_ptr<Direct3DDevice> device,
                                       HANDLE newFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(device), m_hAvailableBufferEvent(newFrameEvent),
      m_hThread(nullptr), m_hTerminateEvent(nullptr)
{
    m_hTerminateEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_hThread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
}

SwapChainProcessor::~SwapChainProcessor()
{
    if (m_hTerminateEvent) {
        SetEvent(m_hTerminateEvent);
    }
    if (m_hThread) {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    if (m_hTerminateEvent) {
        CloseHandle(m_hTerminateEvent);
        m_hTerminateEvent = nullptr;
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID argument)
{
    reinterpret_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD  avTaskIndex = 0;
    HANDLE avTask = AvSetMmThreadCharacteristicsW(L"Distribution", &avTaskIndex);
    RunCore();
    if (avTask) {
        AvRevertMmThreadCharacteristics(avTask);
    }
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_Device->Device.As(&dxgiDevice);
    if (FAILED(hr)) {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
    if (FAILED(hr)) {
        return;
    }

    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);

        if (hr == E_PENDING) {
            HANDLE waitHandles[] = { m_hAvailableBufferEvent, m_hTerminateEvent };
            DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 16);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
                continue;
            }
            break;  // terminate event or error
        } else if (SUCCEEDED(hr)) {
            // Headless target: acknowledge the frame. Capture hand-off is future work.
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr)) {
                break;
            }
        } else {
            break;  // unexpected error
        }
    }
}
