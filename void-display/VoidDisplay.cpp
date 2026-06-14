#include "VoidDisplay.h"

using namespace Microsoft::WRL;

// Per-monitor container id derived from a fixed base so monitors are distinct
// but stable across reboots.
static const GUID kVoidContainerBase =
    { 0x61912c91, 0x979a, 0x40e7, { 0xb4, 0x2a, 0xab, 0xf6, 0xd9, 0x17, 0xbf, 0xf3 } };

// The driver's WUDF service Parameters key - source of the operator-tunable
// settings the driver reads at init (render adapter, custom modes, persisted
// displays, hardware cursor). The UMDF host can read it but not write it.
static const wchar_t kVoidParamsKey[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services\\VoidDisplay\\Parameters";

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
    bool found = false;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            VOID_LOG("DXGI adapter %u: \"%S\" vendor=0x%04X device=0x%04X LUID=%ld:%lu%s",
                     i, desc.Description, desc.VendorId, desc.DeviceId,
                     desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                     software ? " [software]" : "");
            if (!found && desc.VendorId == vendorId && !software) {
                *outLuid = desc.AdapterLuid;
                found = true;
                VOID_LOG("Preferred render adapter: \"%S\" vendor 0x%04X -> LUID %ld:%lu",
                         desc.Description, vendorId,
                         desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            }
        }
        adapter.Reset();
    }

    if (!found) {
        VOID_LOG("No DXGI adapter for preferred vendor 0x%04X; using auto", vendorId);
    }
    return found;
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
    iddConfig.EvtIddCxDeviceIoControl                   = VoidDisplayIoDeviceControl;

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

    // IO control requests are delivered through IddCx (EvtIddCxDeviceIoControl,
    // set above) because IddCx owns the device's IRP_MJ_DEVICE_CONTROL dispatching.
    // We only expose a device interface for user mode to open and target.
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
VOID VoidDisplayIoDeviceControl(WDFDEVICE Device, WDFREQUEST Request,
                                size_t OutputBufferLength, size_t InputBufferLength,
                                ULONG IoControlCode)
{
    VOID_LOG("IoDeviceControl code=0x%08X inLen=%Iu outLen=%Iu", IoControlCode,
             InputBufferLength, OutputBufferLength);

    auto* wrapper = WdfObjectGet_VoidDeviceContextWrapper(Device);
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
    case IOCTL_VOIDDISPLAY_SET_MODE_DYNAMIC: {
        VOIDDISPLAY_SET_MODE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VOIDDISPLAY_SET_MODE), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) { status = dev->SetModeDynamic(in->Index, in->Mode); }
        break;
    }
    case IOCTL_VOIDDISPLAY_LIST: {
        VOIDDISPLAY_STATE* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VOIDDISPLAY_STATE), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) { dev->GetState(out); info = sizeof(VOIDDISPLAY_STATE); }
        break;
    }
    case IOCTL_VOIDDISPLAY_ADD_MODE: {
        VOIDDISPLAY_MODE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VOIDDISPLAY_MODE), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) { status = dev->AddMode(*in); }
        break;
    }
    case IOCTL_VOIDDISPLAY_REMOVE_MODE: {
        VOIDDISPLAY_MODE* in = nullptr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(VOIDDISPLAY_MODE), (PVOID*)&in, nullptr);
        if (NT_SUCCESS(status)) { status = dev->RemoveMode(*in); }
        break;
    }
    case IOCTL_VOIDDISPLAY_LIST_MODES: {
        VOIDDISPLAY_MODE_LIST* out = nullptr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VOIDDISPLAY_MODE_LIST), (PVOID*)&out, nullptr);
        if (NT_SUCCESS(status)) { dev->GetModes(out); info = sizeof(VOIDDISPLAY_MODE_LIST); }
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
    VOID_LOG("AdapterCommitModes PathCount=%u", pInArgs ? pInArgs->PathCount : 0);
    // Accept whatever path/mode set the OS commits.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VoidDisplayParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
                                            IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    VOID_MODE_DESC modes[VOIDDISPLAY_MAX_MODES];
    unsigned count = VoidModesGet(modes, VOIDDISPLAY_MAX_MODES);

    VOID_LOG("ParseMonitorDescription in=%u out=%u", pInArgs->MonitorModeBufferInputCount, count);
    pOutArgs->MonitorModeBufferOutputCount = count;
    if (pInArgs->MonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;  // size query
    }
    if (pInArgs->MonitorModeBufferInputCount < count) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < count; ++i) {
        FillMonitorMode(pInArgs->pMonitorModes[i], modes[i]);
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
    VOID_MODE_DESC modes[VOIDDISPLAY_MAX_MODES];
    unsigned count = VoidModesGet(modes, VOIDDISPLAY_MAX_MODES);

    pOutArgs->DefaultMonitorModeBufferOutputCount = count;
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < count) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < count; ++i) {
        FillMonitorMode(pInArgs->pDefaultMonitorModes[i], modes[i]);
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
    VOID_MODE_DESC modes[VOIDDISPLAY_MAX_MODES];
    unsigned count = VoidModesGet(modes, VOIDDISPLAY_MAX_MODES);

    VOID_LOG("QueryTargetModes in=%u out=%u", pInArgs->TargetModeBufferInputCount, count);
    pOutArgs->TargetModeBufferOutputCount = count;
    if (pInArgs->TargetModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (pInArgs->TargetModeBufferInputCount < count) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < count; ++i) {
        FillTargetMode(pInArgs->pTargetModes[i], modes[i]);
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
    : m_WdfDevice(wdfDevice), m_Adapter(nullptr), m_Lock(nullptr),
      m_InitStarted(false), m_HardwareCursor(true)
{
    for (UINT32 i = 0; i < VOIDDISPLAY_MAX_DISPLAYS; ++i) {
        m_Slots[i].InUse       = false;
        m_Slots[i].Mode        = VOIDDISPLAY_MODE{};
        m_Slots[i].Monitor     = nullptr;
        m_Slots[i].CursorEvent = nullptr;
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
    caps.Size                    = sizeof(caps);
    caps.MaxMonitorsSupported    = VOIDDISPLAY_MAX_DISPLAYS;
    // Total display bandwidth budget. 0 would mean "no active mode may exceed 0",
    // i.e. nothing can be driven; set it very high so any mode set is allowed.
    caps.MaxDisplayPipelineRate  = (UINT64)1000000000ULL * 1000ULL;

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

    // Hardware cursor: on by default. With a hardware-cursor plane the OS draws
    // the pointer as an overlay instead of compositing it into the captured
    // frames, so remote-desktop apps that render their own client-side cursor do
    // not show a double pointer. Set HardwareCursorEnabled=0 to bake the cursor
    // into the frames instead (server-side cursor).
    DWORD hwc = 1;
    DWORD hwcb = sizeof(hwc);
    RegGetValueW(HKEY_LOCAL_MACHINE, kVoidParamsKey, L"HardwareCursorEnabled",
                 RRF_RT_REG_DWORD, NULL, &hwc, &hwcb);
    m_HardwareCursor = (hwc != 0);

    // Seed any SDK-persisted custom modes so they are advertised the moment a
    // monitor arrives (survives device restart / reboot). Defaults are always
    // present; this only adds user-defined extras.
    VoidModesLoadPersisted();

    // Recreate the displays that were active before the last unload so a headless
    // host comes back configured (gated on RestoreOnStart; default on). Done after
    // the modes are loaded and before any control IOCTL is served.
    RestoreDisplays();
}

NTSTATUS VoidDisplayDevice::CreateMonitorLocked(UINT32 index, const VOIDDISPLAY_MODE& mode)
{
    UINT8 edid[VOIDDISPLAY_EDID_SIZE];
    VoidBuildEdid(edid, 0x56564400u + index);  // "VVD" + slot

    IDDCX_MONITOR_INFO info = {};
    info.Size                       = sizeof(info);
    // Declare DisplayPort-External (what Parsec's VDD reports). HDMI makes
    // Windows pop the Win+P projection chooser and leave the display inactive;
    // INDIRECT_WIRED suppresses the chooser but the OS won't auto-extend to it.
    // DISPLAYPORT_EXTERNAL gives both: no chooser AND auto-extend.
    info.MonitorType                = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
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

NTSTATUS VoidDisplayDevice::AddMonitor(const VOIDDISPLAY_MODE& mode, UINT32* outIndex)
{
    VOIDDISPLAY_MODE m = mode;
    if (m.Width == 0 || m.Height == 0 || m.RefreshHz == 0) {
        m.Width = VOID_DEFAULT_WIDTH;
        m.Height = VOID_DEFAULT_HEIGHT;
        m.RefreshHz = VOID_DEFAULT_REFRESH;
    }
    // Ensure the OS sees this mode in the monitor's list when it arrives.
    VoidModesAdd(m.Width, m.Height, m.RefreshHz);

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

    IDDCX_MONITOR monitor = nullptr;
    std::unique_ptr<SwapChainProcessor> processor;

    WdfWaitLockAcquire(m_Lock, NULL);
    if (m_Slots[index].InUse) {
        monitor   = m_Slots[index].Monitor;
        processor = std::move(m_Slots[index].Processor);
        m_Slots[index].Monitor = nullptr;
        m_Slots[index].InUse   = false;
        if (m_Slots[index].CursorEvent) {
            CloseHandle(m_Slots[index].CursorEvent);
            m_Slots[index].CursorEvent = nullptr;
        }
    }
    WdfWaitLockRelease(m_Lock);

    // Tear down OUTSIDE the lock: joining the swap-chain thread and
    // IddCxMonitorDeparture can both re-enter EvtIddCxMonitorUnassignSwapChain,
    // which takes m_Lock - holding it here deadlocks until the OS times out
    // (the cause of the ~2-3s "remove failed").
    processor.reset();
    if (monitor) {
        IddCxMonitorDeparture(monitor);
    }
    VOID_LOG("Monitor removed slot %u", index);
    return STATUS_SUCCESS;
}

NTSTATUS VoidDisplayDevice::SetMode(UINT32 index, const VOIDDISPLAY_MODE& mode)
{
    if (index >= VOIDDISPLAY_MAX_DISPLAYS) {
        return STATUS_INVALID_PARAMETER;
    }
    // The target mode must be advertised; ensure it is before applying.
    VoidModesAdd(mode.Width, mode.Height, mode.RefreshHz);

    WdfWaitLockAcquire(m_Lock, NULL);
    NTSTATUS status = STATUS_NOT_FOUND;
    if (m_Slots[index].InUse) {
        m_Slots[index].Mode = mode;
        status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(m_Lock);
    return status;
}

// Like SetMode but for on-the-fly resolutions (e.g. a client window resize). The
// set of settable resolutions is fixed when the monitor arrives (ParseMonitorDescription),
// so a brand-new mode is only exposed by re-plugging the monitor; an already-advertised
// mode (a default or one added earlier) is settable directly with no re-plug. Not
// persisted: dynamic modes are ephemeral and do not change the restore-on-start state.
NTSTATUS VoidDisplayDevice::SetModeDynamic(UINT32 index, const VOIDDISPLAY_MODE& mode)
{
    if (index >= VOIDDISPLAY_MAX_DISPLAYS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (mode.Width == 0 || mode.Height == 0 || mode.RefreshHz == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    bool wasAdvertised = VoidModesContains(mode.Width, mode.Height, mode.RefreshHz);
    VoidModesAdd(mode.Width, mode.Height, mode.RefreshHz);

    WdfWaitLockAcquire(m_Lock, NULL);
    NTSTATUS status = STATUS_NOT_FOUND;
    bool replug = false;
    if (m_Slots[index].InUse) {
        m_Slots[index].Mode = mode;
        replug = !wasAdvertised;   // a new resolution needs a re-plug to be settable
        status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(m_Lock);

    if (NT_SUCCESS(status) && replug) {
        ReplugMonitor(index);
    }
    return status;
}

// Depart and re-create a live monitor in the same slot. The identity (ContainerId +
// EDID serial) is derived from the slot index, so the OS sees the same monitor return -
// but ParseMonitorDescription runs again, which is the only way a newly-added
// resolution becomes settable on a live display. This is a single-monitor hotplug:
// it blinks only this monitor, NOT the whole desktop (unlike IddCxMonitorUpdateModes,
// which forces a global topology re-evaluation and modesets the other displays).
void VoidDisplayDevice::ReplugMonitor(UINT32 index)
{
    IDDCX_MONITOR                       oldMonitor  = nullptr;
    std::unique_ptr<SwapChainProcessor> processor;
    HANDLE                              cursorEvent = nullptr;
    VOIDDISPLAY_MODE                    mode{};

    WdfWaitLockAcquire(m_Lock, NULL);
    if (index < VOIDDISPLAY_MAX_DISPLAYS && m_Slots[index].InUse) {
        oldMonitor  = m_Slots[index].Monitor;
        processor   = std::move(m_Slots[index].Processor);
        cursorEvent = m_Slots[index].CursorEvent;
        mode        = m_Slots[index].Mode;
        m_Slots[index].Monitor     = nullptr;
        m_Slots[index].CursorEvent = nullptr;
        m_Slots[index].InUse       = false;
    }
    WdfWaitLockRelease(m_Lock);

    if (!oldMonitor) {
        return;
    }

    // Tear down OUTSIDE the lock: departure re-enters EvtIddCxMonitorUnassignSwapChain,
    // which takes m_Lock (the same deadlock as RemoveMonitor).
    processor.reset();
    if (cursorEvent) {
        CloseHandle(cursorEvent);
    }
    IddCxMonitorDeparture(oldMonitor);

    // Re-create in the same slot with the refreshed (now larger) mode list.
    WdfWaitLockAcquire(m_Lock, NULL);
    NTSTATUS status = CreateMonitorLocked(index, mode);
    WdfWaitLockRelease(m_Lock);
    VOID_LOG("Replug slot %u %ux%u@%u status=0x%08X",
             index, mode.Width, mode.Height, mode.RefreshHz, status);
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

// On-disk form of one persisted display: its slot index plus mode. The SDK writes
// a REG_BINARY array of these under the driver's WUDF service Parameters key (the
// UMDF host itself has no registry write rights); the driver reads it here. Same
// key the driver reads PreferredRenderAdapterVendorId and the custom modes from.
struct VoidPersistEntry
{
    UINT32 Index;
    UINT32 Width;
    UINT32 Height;
    UINT32 RefreshHz;
};

// Recreate displays the SDK persisted to the Parameters key. Called once at
// adapter init,
// after the custom modes are loaded and before any control IOCTL is served. Gated
// on RestoreOnStart (REG_DWORD; absent or non-zero means restore).
void VoidDisplayDevice::RestoreDisplays()
{
    DWORD restore = 1;  // default on; left unchanged when the value is absent
    DWORD cb = sizeof(restore);
    RegGetValueW(HKEY_LOCAL_MACHINE, kVoidParamsKey, L"RestoreOnStart",
                 RRF_RT_REG_DWORD, NULL, &restore, &cb);
    if (restore == 0) {
        VOID_LOG("RestoreOnStart=0; not restoring displays");
        return;
    }

    VoidPersistEntry entries[VOIDDISPLAY_MAX_DISPLAYS];
    DWORD byteLen = sizeof(entries);
    LSTATUS rs = RegGetValueW(HKEY_LOCAL_MACHINE, kVoidParamsKey, L"PersistedDisplays",
                              RRF_RT_REG_BINARY, NULL, entries, &byteLen);
    if (rs != ERROR_SUCCESS || byteLen == 0) {
        return;  // none persisted
    }

    UINT32 count    = byteLen / (DWORD)sizeof(VoidPersistEntry);
    UINT32 restored = 0;
    WdfWaitLockAcquire(m_Lock, NULL);
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 idx = entries[i].Index;
        if (idx >= VOIDDISPLAY_MAX_DISPLAYS || m_Slots[idx].InUse) {
            continue;  // bad index or slot already taken
        }
        VOIDDISPLAY_MODE mode;
        mode.Width     = entries[i].Width;
        mode.Height    = entries[i].Height;
        mode.RefreshHz = entries[i].RefreshHz;
        if (mode.Width == 0 || mode.Height == 0 || mode.RefreshHz == 0) {
            mode.Width     = VOID_DEFAULT_WIDTH;
            mode.Height    = VOID_DEFAULT_HEIGHT;
            mode.RefreshHz = VOID_DEFAULT_REFRESH;
        }
        VoidModesAdd(mode.Width, mode.Height, mode.RefreshHz);  // ensure advertised
        if (NT_SUCCESS(CreateMonitorLocked(idx, mode))) {
            ++restored;
        }
    }
    WdfWaitLockRelease(m_Lock);
    VOID_LOG("Restored %u of %u persisted display(s)", restored, count);
}

NTSTATUS VoidDisplayDevice::AddMode(const VOIDDISPLAY_MODE& mode)
{
    if (!VoidModesAdd(mode.Width, mode.Height, mode.RefreshHz)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // Applies to displays added afterward (and existing ones on re-add). We do
    // not re-plug live monitors: it is disruptive and resets the Windows 10
    // per-combination topology (see parsec-vdd issue #23).
    VOID_LOG("Added custom mode %ux%u@%u", mode.Width, mode.Height, mode.RefreshHz);
    return STATUS_SUCCESS;
}

NTSTATUS VoidDisplayDevice::RemoveMode(const VOIDDISPLAY_MODE& mode)
{
    if (!VoidModesRemove(mode.Width, mode.Height, mode.RefreshHz)) {
        return STATUS_NOT_FOUND;  // not a custom mode (defaults cannot be removed)
    }
    VOID_LOG("Removed custom mode %ux%u@%u", mode.Width, mode.Height, mode.RefreshHz);
    return STATUS_SUCCESS;
}

void VoidDisplayDevice::GetModes(VOIDDISPLAY_MODE_LIST* out)
{
    VOID_MODE_DESC modes[VOIDDISPLAY_MAX_MODES];
    unsigned count = VoidModesGet(modes, VOIDDISPLAY_MAX_MODES);
    out->Count = count;
    for (unsigned i = 0; i < count && i < VOIDDISPLAY_MAX_MODES; ++i) {
        out->Modes[i].Width = modes[i].Width;
        out->Modes[i].Height = modes[i].Height;
        out->Modes[i].RefreshHz = modes[i].RefreshHz;
    }
}

void VoidDisplayDevice::AssignSwapChain(UINT32 index, IDDCX_SWAPCHAIN swapChain,
                                        LUID renderAdapter, HANDLE newFrameEvent)
{
    VOID_LOG("AssignSwapChain slot=%u renderLUID=%ld:%lu", index,
             renderAdapter.HighPart, renderAdapter.LowPart);
    WdfWaitLockAcquire(m_Lock, NULL);
    if (index < VOIDDISPLAY_MAX_DISPLAYS && m_Slots[index].InUse) {
        m_Slots[index].Processor.reset();

        auto device = std::make_shared<Direct3DDevice>(renderAdapter);
        HRESULT hr = device->Init();
        if (SUCCEEDED(hr)) {
            m_Slots[index].Processor =
                std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
            VOID_LOG("SwapChain processor started for slot %u", index);

            // Expose a hardware-cursor plane so the OS does not bake the pointer
            // into the swap-chain frames (avoids a double cursor on remote-desktop
            // apps that draw their own client-side pointer). Each monitor gets its
            // own unnamed data-available event. Available since IddCx 1.0, so no
            // version guard is needed.
            if (m_HardwareCursor) {
                if (m_Slots[index].CursorEvent) {
                    CloseHandle(m_Slots[index].CursorEvent);
                    m_Slots[index].CursorEvent = nullptr;
                }
                HANDLE cursorEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (cursorEvent) {
                    IDDCX_CURSOR_CAPS caps = {};
                    caps.Size                  = sizeof(caps);
                    caps.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
                    caps.AlphaCursorSupport    = TRUE;
                    caps.MaxX                  = 256;   // covers high-DPI pointers
                    caps.MaxY                  = 256;

                    IDARG_IN_SETUP_HWCURSOR in = {};
                    in.CursorInfo              = caps;
                    in.hNewCursorDataAvailable = cursorEvent;

                    NTSTATUS cs = IddCxMonitorSetupHardwareCursor(m_Slots[index].Monitor, &in);
                    if (NT_SUCCESS(cs)) {
                        m_Slots[index].CursorEvent = cursorEvent;
                        VOID_LOG("Hardware cursor enabled for slot %u", index);
                    } else {
                        CloseHandle(cursorEvent);
                        VOID_LOG("SetupHardwareCursor slot %u failed 0x%08X", index, cs);
                    }
                }
            }
        } else {
            VOID_LOG("Direct3DDevice Init failed for slot %u hr=0x%08X", index, hr);
        }
    }
    WdfWaitLockRelease(m_Lock);
}

void VoidDisplayDevice::UnassignSwapChain(UINT32 index)
{
    WdfWaitLockAcquire(m_Lock, NULL);
    if (index < VOIDDISPLAY_MAX_DISPLAYS) {
        m_Slots[index].Processor.reset();
        if (m_Slots[index].CursorEvent) {
            CloseHandle(m_Slots[index].CursorEvent);
            m_Slots[index].CursorEvent = nullptr;
        }
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
