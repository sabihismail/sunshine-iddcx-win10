/*
 * VoidDisplay - IddCx UMDF virtual display driver.
 *
 * One root-enumerated adapter hosts up to VOIDDISPLAY_MAX_DISPLAYS virtual
 * monitors. Monitors are created and destroyed on demand through the control
 * IOCTLs in Public.h. Displays persist until explicitly removed - there is no
 * keepalive.
 */

#pragma once

#define NOMINMAX

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>

// Allocate GUID_DEVINTERFACE_VOIDDISPLAY here (this header is included by exactly
// one translation unit). Must precede Public.h, which declares it via DEFINE_GUID.
#include <initguid.h>
#include "Public.h"
#include "Trace.h"
#include "Edid.h"
#include "Modes.h"

// ---------------------------------------------------------------------------
// Direct3D render device bound to the OS-chosen render adapter LUID.
// ---------------------------------------------------------------------------
struct Direct3DDevice
{
    explicit Direct3DDevice(LUID adapterLuid);
    Direct3DDevice();
    HRESULT Init();

    LUID AdapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5>      DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>      Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device>       Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

// ---------------------------------------------------------------------------
// Consumes frames from an indirect swap-chain on a dedicated thread. For a
// headless capture target it simply drains and acknowledges frames; the
// capture hand-off is a later integration.
// ---------------------------------------------------------------------------
class SwapChainProcessor
{
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
                       std::shared_ptr<Direct3DDevice> device,
                       HANDLE newFrameEvent);
    ~SwapChainProcessor();

private:
    static DWORD CALLBACK RunThread(LPVOID argument);
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN                 m_hSwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE                          m_hAvailableBufferEvent;
    HANDLE                          m_hThread;
    HANDLE                          m_hTerminateEvent;
};

// One virtual-monitor slot.
struct VoidDisplaySlot
{
    bool                                InUse;
    VOIDDISPLAY_MODE                    Mode;
    IDDCX_MONITOR                       Monitor;
    std::unique_ptr<SwapChainProcessor> Processor;
};

// ---------------------------------------------------------------------------
// Per-adapter device context. Owns the monitor table and the IddCx adapter.
// ---------------------------------------------------------------------------
class VoidDisplayDevice
{
public:
    explicit VoidDisplayDevice(WDFDEVICE wdfDevice);
    ~VoidDisplayDevice();

    void InitAdapter();                          // from D0Entry
    void FinishInit(IDDCX_ADAPTER adapter);      // from AdapterInitFinished

    NTSTATUS AddMonitor(const VOIDDISPLAY_MODE& mode, UINT32* outIndex);
    NTSTATUS RemoveMonitor(UINT32 index);
    NTSTATUS SetMode(UINT32 index, const VOIDDISPLAY_MODE& mode);
    void     GetState(VOIDDISPLAY_STATE* out);

    NTSTATUS AddMode(const VOIDDISPLAY_MODE& mode);
    NTSTATUS RemoveMode(const VOIDDISPLAY_MODE& mode);
    void     GetModes(VOIDDISPLAY_MODE_LIST* out);

    void AssignSwapChain(UINT32 index, IDDCX_SWAPCHAIN swapChain,
                         LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain(UINT32 index);

    WDFDEVICE   WdfDevice() const { return m_WdfDevice; }
    IDDCX_ADAPTER Adapter() const { return m_Adapter; }

private:
    NTSTATUS CreateMonitorLocked(UINT32 index, const VOIDDISPLAY_MODE& mode);

    WDFDEVICE       m_WdfDevice;
    IDDCX_ADAPTER   m_Adapter;
    WDFWAITLOCK     m_Lock;
    bool            m_InitStarted;
    VoidDisplaySlot m_Slots[VOIDDISPLAY_MAX_DISPLAYS];
};

// WDF object context wrappers (POD; hold pointers to the C++ objects).
struct VoidDeviceContextWrapper
{
    VoidDisplayDevice* pContext;
    void Cleanup();
};
WDF_DECLARE_CONTEXT_TYPE(VoidDeviceContextWrapper);

struct VoidMonitorContextWrapper
{
    VoidDisplayDevice* Device;
    UINT32             Index;
};
WDF_DECLARE_CONTEXT_TYPE(VoidMonitorContextWrapper);

// ---------------------------------------------------------------------------
// Entry points and callbacks.
// ---------------------------------------------------------------------------
EXTERN_C DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD    VoidDisplayDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY      VoidDisplayDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL VoidDisplayIoDeviceControl;

EVT_IDD_CX_ADAPTER_INIT_FINISHED                 VoidDisplayAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES                  VoidDisplayAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION             VoidDisplayParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VoidDisplayMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES            VoidDisplayMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN              VoidDisplayMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN            VoidDisplayMonitorUnassignSwapChain;
