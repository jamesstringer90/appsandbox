#pragma warning (disable : 4127)

#include <initguid.h>
#include "driverdefs.h"
#include "pinnodes.h"

//=============================================================================
// Mixer register counts
//=============================================================================

#define MAX_TOPOLOGY_NODES      2
#define MAX_CHANNELS            2
//=============================================================================
// Sub-device cache entry
//=============================================================================

typedef struct _MINIPAIR_UNKNOWN
{
    LIST_ENTRY              ListEntry;
    WCHAR                   Name[MAX_PATH];
    PUNKNOWN                PortInterface;
    PUNKNOWN                MiniportInterface;
    PADAPTERPOWERMANAGEMENT PowerInterface;
} MINIPAIR_UNKNOWN;

//=============================================================================
// CVadAdapterCommon
//=============================================================================

class CVadAdapterCommon :
    public IAdapterCommon,
    public IAdapterPowerManagement,
    public CUnknown
{
private:
    PSERVICEGROUP           m_pServiceGroupWave;
    PDEVICE_OBJECT          m_pDeviceObject;
    PDEVICE_OBJECT          m_pPhysicalDeviceObject;
    WDFDEVICE               m_WdfDevice;
    DEVICE_POWER_STATE      m_PowerState;
    PPORTCLSETWHELPER       m_pPortClsEtwHelper;
    LIST_ENTRY              m_SubdeviceCache;

    static LONG             m_AdapterInstances;

    // Inline mixer state (no separate HW object)
    BOOL                    m_MixerMute[MAX_TOPOLOGY_NODES][MAX_CHANNELS];
    LONG                    m_MixerVolume[MAX_TOPOLOGY_NODES][MAX_CHANNELS];
    LONG                    m_PeakMeter[MAX_TOPOLOGY_NODES][MAX_CHANNELS];
    ULONG                   m_MixerMux;
    BOOL                    m_bDevSpecific;
    INT                     m_iDevSpecific;
    UINT                    m_uiDevSpecific;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CVadAdapterCommon);
    ~CVadAdapterCommon();

    IMP_IAdapterPowerManagement;

    STDMETHODIMP_(NTSTATUS) Init
    (
        _In_  PDEVICE_OBJECT  DeviceObject
    );

    STDMETHODIMP_(PDEVICE_OBJECT)   GetDeviceObject(void);
    STDMETHODIMP_(PDEVICE_OBJECT)   GetPhysicalDeviceObject(void);
    STDMETHODIMP_(WDFDEVICE)        GetWdfDevice(void);

    STDMETHODIMP_(void) SetWaveServiceGroup
    (
        _In_  PSERVICEGROUP   ServiceGroup
    );

    STDMETHODIMP_(BOOL)    bDevSpecificRead();
    STDMETHODIMP_(void)    bDevSpecificWrite(_In_ BOOL bDevSpecific);
    STDMETHODIMP_(INT)     iDevSpecificRead();
    STDMETHODIMP_(void)    iDevSpecificWrite(_In_ INT iDevSpecific);
    STDMETHODIMP_(UINT)    uiDevSpecificRead();
    STDMETHODIMP_(void)    uiDevSpecificWrite(_In_ UINT uiDevSpecific);

    STDMETHODIMP_(BOOL)    MixerMuteRead(_In_ ULONG Index, _In_ ULONG Channel);
    STDMETHODIMP_(void)    MixerMuteWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ BOOL Value);
    STDMETHODIMP_(ULONG)   MixerMuxRead(void);
    STDMETHODIMP_(void)    MixerMuxWrite(_In_ ULONG Index);
    STDMETHODIMP_(void)    MixerReset(void);
    STDMETHODIMP_(LONG)    MixerVolumeRead(_In_ ULONG Index, _In_ ULONG Channel);
    STDMETHODIMP_(void)    MixerVolumeWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ LONG Value);
    STDMETHODIMP_(LONG)    MixerPeakMeterRead(_In_ ULONG Index, _In_ ULONG Channel);

    STDMETHODIMP_(NTSTATUS) WriteEtwEvent
    (
        _In_ EPcMiniportEngineEvent miniportEventType,
        _In_ ULONGLONG ullData1,
        _In_ ULONGLONG ullData2,
        _In_ ULONGLONG ullData3,
        _In_ ULONGLONG ullData4
    );

    STDMETHODIMP_(VOID) SetEtwHelper(PPORTCLSETWHELPER _pPortClsEtwHelper);

    STDMETHODIMP_(NTSTATUS) InstallSubdevice
    (
        _In_opt_        PIRP                                    Irp,
        _In_            PWSTR                                   Name,
        _In_            REFGUID                                 PortClassId,
        _In_            REFGUID                                 MiniportClassId,
        _In_opt_        PFNCREATEMINIPORT                       MiniportCreate,
        _In_            ULONG                                   cPropertyCount,
        _In_reads_opt_(cPropertyCount) const VAD_DEVPROPERTY  * pProperties,
        _In_opt_        PVOID                                   DeviceContext,
        _In_            PENDPOINT_MINIPAIR                      MiniportPair,
        _In_opt_        PRESOURCELIST                           ResourceList,
        _In_            REFGUID                                 PortInterfaceId,
        _Out_opt_       PUNKNOWN                              * OutPortInterface,
        _Out_opt_       PUNKNOWN                              * OutPortUnknown,
        _Out_opt_       PUNKNOWN                              * OutMiniportUnknown
    );

    STDMETHODIMP_(NTSTATUS) UnregisterSubdevice(_In_opt_ PUNKNOWN UnknownPort);

    STDMETHODIMP_(NTSTATUS) ConnectTopologies
    (
        _In_ PUNKNOWN                   UnknownTopology,
        _In_ PUNKNOWN                   UnknownWave,
        _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
        _In_ ULONG                      PhysicalConnectionCount
    );

    STDMETHODIMP_(NTSTATUS) DisconnectTopologies
    (
        _In_ PUNKNOWN                   UnknownTopology,
        _In_ PUNKNOWN                   UnknownWave,
        _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
        _In_ ULONG                      PhysicalConnectionCount
    );

    STDMETHODIMP_(NTSTATUS) InstallEndpointFilters
    (
        _In_opt_    PIRP                Irp,
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_opt_    PVOID               DeviceContext,
        _Out_opt_   PUNKNOWN *          UnknownTopology,
        _Out_opt_   PUNKNOWN *          UnknownWave,
        _Out_opt_   PUNKNOWN *          UnknownMiniportTopology,
        _Out_opt_   PUNKNOWN *          UnknownMiniportWave
    );

    STDMETHODIMP_(NTSTATUS) RemoveEndpointFilters
    (
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_opt_    PUNKNOWN            UnknownTopology,
        _In_opt_    PUNKNOWN            UnknownWave
    );

    STDMETHODIMP_(NTSTATUS) GetFilters
    (
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _Out_opt_   PUNKNOWN            *UnknownTopologyPort,
        _Out_opt_   PUNKNOWN            *UnknownTopologyMiniport,
        _Out_opt_   PUNKNOWN            *UnknownWavePort,
        _Out_opt_   PUNKNOWN            *UnknownWaveMiniport
    );

    STDMETHODIMP_(NTSTATUS) SetIdlePowerManagement
    (
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_        BOOL                bEnabled
    );

    STDMETHODIMP_(VOID) Cleanup();

    friend NTSTATUS NewAdapterCommon
    (
        _Out_       PUNKNOWN *              Unknown,
        _In_        REFCLSID,
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        POOL_FLAGS              PoolFlags
    );

private:
    NTSTATUS GetCachedSubdevice(_In_ PWSTR Name, _Out_opt_ PUNKNOWN *OutUnknownPort, _Out_opt_ PUNKNOWN *OutUnknownMiniport);
    NTSTATUS CacheSubdevice(_In_ PWSTR Name, _In_ PUNKNOWN UnknownPort, _In_ PUNKNOWN UnknownMiniport);
    NTSTATUS RemoveCachedSubdevice(_In_ PWSTR Name);
    VOID EmptySubdeviceCache();

    NTSTATUS CreateAudioInterfaceWithProperties
    (
        _In_ PCWSTR                                                 ReferenceString,
        _In_ ULONG                                                  cPropertyCount,
        _In_reads_opt_(cPropertyCount) const VAD_DEVPROPERTY       *pProperties,
        _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem)) PUNICODE_STRING AudioSymbolicLinkName
    );

};

LONG CVadAdapterCommon::m_AdapterInstances = 0;

//=============================================================================
// Helper: set device interface properties
//=============================================================================

#pragma code_seg("PAGE")
static
NTSTATUS VadIoSetDeviceInterfacePropertyDataMultiple
(
    _In_ PUNICODE_STRING                                        SymbolicLinkName,
    _In_ ULONG                                                  cPropertyCount,
    _In_reads_opt_(cPropertyCount) const VAD_DEVPROPERTY       *pProperties
)
{
    NTSTATUS ntStatus;

    PAGED_CODE();

    if (pProperties)
    {
        for (ULONG i = 0; i < cPropertyCount; i++)
        {
            ntStatus = IoSetDeviceInterfacePropertyData(
                SymbolicLinkName,
                pProperties[i].PropertyKey,
                LOCALE_NEUTRAL,
                PLUGPLAY_PROPERTY_PERSISTENT,
                pProperties[i].Type,
                pProperties[i].BufferSize,
                pProperties[i].Buffer);

            if (!NT_SUCCESS(ntStatus))
            {
                return ntStatus;
            }
        }
    }

    return STATUS_SUCCESS;
}

//=============================================================================
// NewAdapterCommon
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
NewAdapterCommon
(
    _Out_       PUNKNOWN *              Unknown,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        POOL_FLAGS              PoolFlags
)
{
    PAGED_CODE();

    ASSERT(Unknown);

    NTSTATUS ntStatus;

    if (InterlockedCompareExchange(&CVadAdapterCommon::m_AdapterInstances, 1, 0) != 0)
    {
        ntStatus = STATUS_DEVICE_BUSY;
        DPF(D_ERROR, ("NewAdapterCommon failed, only one instance is allowed"));
        goto Done;
    }

    {
        CVadAdapterCommon *p = new(PoolFlags, VAD_ADAPTER_POOLTAG) CVadAdapterCommon(UnknownOuter);
        if (p == NULL)
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
            DPF(D_ERROR, ("NewAdapterCommon failed, 0x%x", ntStatus));
            goto Done;
        }

        *Unknown = PUNKNOWN((PADAPTERCOMMON)(p));
        (*Unknown)->AddRef();
        ntStatus = STATUS_SUCCESS;
    }

Done:
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
CVadAdapterCommon::~CVadAdapterCommon(void)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::~CVadAdapterCommon]"));

    SAFE_RELEASE(m_pPortClsEtwHelper);
    SAFE_RELEASE(m_pServiceGroupWave);

    if (m_WdfDevice)
    {
        WdfObjectDelete(m_WdfDevice);
        m_WdfDevice = NULL;
    }

    InterlockedDecrement(&CVadAdapterCommon::m_AdapterInstances);
    ASSERT(CVadAdapterCommon::m_AdapterInstances == 0);
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT) CVadAdapterCommon::GetDeviceObject(void)
{
    PAGED_CODE();
    return m_pDeviceObject;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT) CVadAdapterCommon::GetPhysicalDeviceObject(void)
{
    PAGED_CODE();
    return m_pPhysicalDeviceObject;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(WDFDEVICE) CVadAdapterCommon::GetWdfDevice(void)
{
    PAGED_CODE();
    return m_WdfDevice;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CVadAdapterCommon::Init
(
    _In_  PDEVICE_OBJECT  DeviceObject
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::Init]"));

    ASSERT(DeviceObject);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    m_pServiceGroupWave     = NULL;
    m_pDeviceObject         = DeviceObject;
    m_pPhysicalDeviceObject = NULL;
    m_WdfDevice             = NULL;
    m_PowerState            = PowerDeviceD0;
    m_pPortClsEtwHelper     = NULL;

    InitializeListHead(&m_SubdeviceCache);

    ntStatus = PcGetPhysicalDeviceObject(DeviceObject, &m_pPhysicalDeviceObject);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("PcGetPhysicalDeviceObject failed, 0x%x", ntStatus)),
        Done);

    ntStatus = WdfDeviceMiniportCreate(WdfGetDriver(),
                                       WDF_NO_OBJECT_ATTRIBUTES,
                                       DeviceObject,
                                       NULL,
                                       NULL,
                                       &m_WdfDevice);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("WdfDeviceMiniportCreate failed, 0x%x", ntStatus)),
        Done);

    // Initialize mixer state
    MixerReset();

Done:
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(void) CVadAdapterCommon::MixerReset(void)
{
    PAGED_CODE();

    RtlZeroMemory(m_MixerMute, sizeof(m_MixerMute));
    RtlZeroMemory(m_PeakMeter, sizeof(m_PeakMeter));
    m_MixerMux = 0;
    m_bDevSpecific = FALSE;
    m_iDevSpecific = 0;
    m_uiDevSpecific = 0;

    for (ULONG i = 0; i < MAX_TOPOLOGY_NODES; i++)
    {
        for (ULONG j = 0; j < MAX_CHANNELS; j++)
        {
            m_MixerVolume[i][j] = VOLUME_SIGNED_MAXIMUM;
        }
    }
}

//=============================================================================
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::WriteEtwEvent
(
    _In_ EPcMiniportEngineEvent miniportEventType,
    _In_ ULONGLONG ullData1,
    _In_ ULONGLONG ullData2,
    _In_ ULONGLONG ullData3,
    _In_ ULONGLONG ullData4
)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;

    if (m_pPortClsEtwHelper)
    {
        ntStatus = m_pPortClsEtwHelper->MiniportWriteEtwEvent(miniportEventType, ullData1, ullData2, ullData3, ullData4);
    }
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CVadAdapterCommon::SetEtwHelper(PPORTCLSETWHELPER _pPortClsEtwHelper)
{
    PAGED_CODE();

    SAFE_RELEASE(m_pPortClsEtwHelper);
    m_pPortClsEtwHelper = _pPortClsEtwHelper;
    if (m_pPortClsEtwHelper)
    {
        m_pPortClsEtwHelper->AddRef();
    }
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP CVadAdapterCommon::NonDelegatingQueryInterface
(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PADAPTERCOMMON(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterCommon))
    {
        *Object = PVOID(PADAPTERCOMMON(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement))
    {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(void) CVadAdapterCommon::SetWaveServiceGroup(_In_ PSERVICEGROUP ServiceGroup)
{
    PAGED_CODE();
    SAFE_RELEASE(m_pServiceGroupWave);
    m_pServiceGroupWave = ServiceGroup;
    if (m_pServiceGroupWave)
    {
        m_pServiceGroupWave->AddRef();
    }
}

//=============================================================================
// Mixer accessors
//=============================================================================

#pragma code_seg()
STDMETHODIMP_(BOOL) CVadAdapterCommon::bDevSpecificRead() { return m_bDevSpecific; }
STDMETHODIMP_(void) CVadAdapterCommon::bDevSpecificWrite(_In_ BOOL b) { m_bDevSpecific = b; }
STDMETHODIMP_(INT)  CVadAdapterCommon::iDevSpecificRead() { return m_iDevSpecific; }
STDMETHODIMP_(void) CVadAdapterCommon::iDevSpecificWrite(_In_ INT i) { m_iDevSpecific = i; }
STDMETHODIMP_(UINT) CVadAdapterCommon::uiDevSpecificRead() { return m_uiDevSpecific; }
STDMETHODIMP_(void) CVadAdapterCommon::uiDevSpecificWrite(_In_ UINT ui) { m_uiDevSpecific = ui; }

STDMETHODIMP_(BOOL) CVadAdapterCommon::MixerMuteRead(_In_ ULONG Index, _In_ ULONG Channel)
{
    if (Index < MAX_TOPOLOGY_NODES && Channel < MAX_CHANNELS)
    {
        return m_MixerMute[Index][Channel];
    }
    return FALSE;
}

STDMETHODIMP_(void) CVadAdapterCommon::MixerMuteWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ BOOL Value)
{
    if (Index < MAX_TOPOLOGY_NODES && Channel < MAX_CHANNELS)
    {
        m_MixerMute[Index][Channel] = Value;
    }
}

STDMETHODIMP_(ULONG) CVadAdapterCommon::MixerMuxRead(void) { return m_MixerMux; }
STDMETHODIMP_(void)  CVadAdapterCommon::MixerMuxWrite(_In_ ULONG Index) { m_MixerMux = Index; }

STDMETHODIMP_(LONG) CVadAdapterCommon::MixerVolumeRead(_In_ ULONG Index, _In_ ULONG Channel)
{
    if (Index < MAX_TOPOLOGY_NODES && Channel < MAX_CHANNELS)
    {
        return m_MixerVolume[Index][Channel];
    }
    return 0;
}

STDMETHODIMP_(void) CVadAdapterCommon::MixerVolumeWrite(_In_ ULONG Index, _In_ ULONG Channel, _In_ LONG Value)
{
    if (Index < MAX_TOPOLOGY_NODES && Channel < MAX_CHANNELS)
    {
        m_MixerVolume[Index][Channel] = Value;
    }
}

STDMETHODIMP_(LONG) CVadAdapterCommon::MixerPeakMeterRead(_In_ ULONG Index, _In_ ULONG Channel)
{
    if (Index < MAX_TOPOLOGY_NODES && Channel < MAX_CHANNELS)
    {
        return m_PeakMeter[Index][Channel];
    }
    return 0;
}

//=============================================================================
// Power management
//=============================================================================

#pragma code_seg()
STDMETHODIMP_(void) CVadAdapterCommon::PowerChangeState(_In_ POWER_STATE NewState)
{
    DPF_ENTER(("[CVadAdapterCommon::PowerChangeState]"));

    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);
        if (pRecord->PowerInterface)
        {
            pRecord->PowerInterface->PowerChangeState(NewState);
        }
    }

    if (NewState.DeviceState != m_PowerState)
    {
        switch (NewState.DeviceState)
        {
            case PowerDeviceD0:
            case PowerDeviceD1:
            case PowerDeviceD2:
            case PowerDeviceD3:
                m_PowerState = NewState.DeviceState;
                DPF(D_VERBOSE, ("Entering D%u", ULONG(m_PowerState) - ULONG(PowerDeviceD0)));
                break;
            default:
                DPF(D_VERBOSE, ("Unknown Device Power State"));
                break;
        }
    }
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CVadAdapterCommon::QueryDeviceCapabilities(
    _Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES)) PDEVICE_CAPABILITIES PowerDeviceCaps
)
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CVadAdapterCommon::QueryPowerChangeState(_In_ POWER_STATE NewStateQuery)
{
    NTSTATUS status = STATUS_SUCCESS;

    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && NT_SUCCESS(status); le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);
        if (pRecord->PowerInterface)
        {
            status = pRecord->PowerInterface->QueryPowerChangeState(NewStateQuery);
        }
    }

    return status;
}

//=============================================================================
// Audio interface creation
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
CVadAdapterCommon::CreateAudioInterfaceWithProperties
(
    _In_ PCWSTR ReferenceString,
    _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const VAD_DEVPROPERTY *pProperties,
    _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem)) PUNICODE_STRING AudioSymbolicLinkName
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::CreateAudioInterfaceWithProperties]"));

    NTSTATUS        ntStatus;
    UNICODE_STRING  referenceString;

    RtlInitUnicodeString(&referenceString, ReferenceString);
    RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));

    ntStatus = IoRegisterDeviceInterface(
        GetPhysicalDeviceObject(),
        &KSCATEGORY_AUDIO,
        &referenceString,
        AudioSymbolicLinkName);

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("CreateAudioInterfaceWithProperties: IoRegisterDeviceInterface failed, 0x%x", ntStatus)),
        Done);

    ntStatus = VadIoSetDeviceInterfacePropertyDataMultiple(AudioSymbolicLinkName, cPropertyCount, pProperties);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("VadIoSetDeviceInterfacePropertyDataMultiple failed, 0x%x", ntStatus)),
        Done);

    ntStatus = STATUS_SUCCESS;

Done:
    if (!NT_SUCCESS(ntStatus))
    {
        RtlFreeUnicodeString(AudioSymbolicLinkName);
        RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));
    }
    return ntStatus;
}

//=============================================================================
// InstallSubdevice
//=============================================================================

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::InstallSubdevice
(
    _In_opt_        PIRP                                    Irp,
    _In_            PWSTR                                   Name,
    _In_            REFGUID                                 PortClassId,
    _In_            REFGUID                                 MiniportClassId,
    _In_opt_        PFNCREATEMINIPORT                       MiniportCreate,
    _In_            ULONG                                   cPropertyCount,
    _In_reads_opt_(cPropertyCount) const VAD_DEVPROPERTY  * pProperties,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair,
    _In_opt_        PRESOURCELIST                           ResourceList,
    _In_            REFGUID                                 PortInterfaceId,
    _Out_opt_       PUNKNOWN                              * OutPortInterface,
    _Out_opt_       PUNKNOWN                              * OutPortUnknown,
    _Out_opt_       PUNKNOWN                              * OutMiniportUnknown
)
{
    PAGED_CODE();
    DPF_ENTER(("[InstallSubDevice %S]", Name));

    ASSERT(Name != NULL);
    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                    ntStatus;
    PPORT                       port            = NULL;
    PUNKNOWN                    miniport        = NULL;
    PADAPTERCOMMON              adapterCommon   = NULL;
    UNICODE_STRING              symbolicLink    = { 0 };

    adapterCommon = PADAPTERCOMMON(this);

    ntStatus = CreateAudioInterfaceWithProperties(Name, cPropertyCount, pProperties, &symbolicLink);
    if (NT_SUCCESS(ntStatus))
    {
        RtlFreeUnicodeString(&symbolicLink);
        ntStatus = PcNewPort(&port, PortClassId);
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (MiniportCreate)
        {
            ntStatus =
                MiniportCreate
                (
                    &miniport,
                    MiniportClassId,
                    NULL,
                    POOL_FLAG_NON_PAGED,
                    adapterCommon,
                    DeviceContext,
                    MiniportPair
                );
        }
        else
        {
            ntStatus =
                PcNewMiniport
                (
                    (PMINIPORT *)&miniport,
                    MiniportClassId
                );
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
#pragma warning(push)
#pragma warning(disable:6387)
        ntStatus =
            port->Init
            (
                m_pDeviceObject,
                Irp,
                miniport,
                adapterCommon,
                ResourceList
            );
#pragma warning (pop)

        if (NT_SUCCESS(ntStatus))
        {
            ntStatus =
                PcRegisterSubdevice
                (
                    m_pDeviceObject,
                    Name,
                    port
                );
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (OutPortUnknown)
        {
            ntStatus = port->QueryInterface(IID_IUnknown, (PVOID *)OutPortUnknown);
        }

        if (OutPortInterface)
        {
            ntStatus = port->QueryInterface(PortInterfaceId, (PVOID *)OutPortInterface);
        }

        if (OutMiniportUnknown)
        {
            ntStatus = miniport->QueryInterface(IID_IUnknown, (PVOID *)OutMiniportUnknown);
        }
    }

    if (port)
    {
        port->Release();
    }

    if (miniport)
    {
        miniport->Release();
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CVadAdapterCommon::UnregisterSubdevice(_In_opt_ PUNKNOWN UnknownPort)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::UnregisterSubdevice]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                ntStatus            = STATUS_SUCCESS;
    PUNREGISTERSUBDEVICE    unregisterSubdevice = NULL;

    if (NULL == UnknownPort)
    {
        return ntStatus;
    }

    ntStatus = UnknownPort->QueryInterface(
        IID_IUnregisterSubdevice,
        (PVOID *)&unregisterSubdevice);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = unregisterSubdevice->UnregisterSubdevice(m_pDeviceObject, UnknownPort);
        unregisterSubdevice->Release();
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CVadAdapterCommon::ConnectTopologies
(
    _In_ PUNKNOWN                   UnknownTopology,
    _In_ PUNKNOWN                   UnknownWave,
    _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
    _In_ ULONG                      PhysicalConnectionCount
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::ConnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS ntStatus = STATUS_SUCCESS;

    for (ULONG i = 0; i < PhysicalConnectionCount && NT_SUCCESS(ntStatus); i++)
    {
        switch (PhysicalConnections[i].eType)
        {
            case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                ntStatus = PcRegisterPhysicalConnection(
                    m_pDeviceObject,
                    UnknownTopology,
                    PhysicalConnections[i].ulTopology,
                    UnknownWave,
                    PhysicalConnections[i].ulWave);
                break;
            case CONNECTIONTYPE_WAVE_OUTPUT:
                ntStatus = PcRegisterPhysicalConnection(
                    m_pDeviceObject,
                    UnknownWave,
                    PhysicalConnections[i].ulWave,
                    UnknownTopology,
                    PhysicalConnections[i].ulTopology);
                break;
        }
    }

    if (!NT_SUCCESS(ntStatus))
    {
        DisconnectTopologies(UnknownTopology, UnknownWave, PhysicalConnections, PhysicalConnectionCount);
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CVadAdapterCommon::DisconnectTopologies
(
    _In_ PUNKNOWN                   UnknownTopology,
    _In_ PUNKNOWN                   UnknownWave,
    _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
    _In_ ULONG                      PhysicalConnectionCount
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::DisconnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                        ntStatus                        = STATUS_SUCCESS;
    NTSTATUS                        ntStatus2                       = STATUS_SUCCESS;
    PUNREGISTERPHYSICALCONNECTION   unregisterPhysicalConnection    = NULL;

    ntStatus = UnknownTopology->QueryInterface(
        IID_IUnregisterPhysicalConnection,
        (PVOID *)&unregisterPhysicalConnection);

    if (NT_SUCCESS(ntStatus))
    {
        for (ULONG i = 0; i < PhysicalConnectionCount; i++)
        {
            switch (PhysicalConnections[i].eType)
            {
                case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                    ntStatus2 = unregisterPhysicalConnection->UnregisterPhysicalConnection(
                        m_pDeviceObject,
                        UnknownTopology,
                        PhysicalConnections[i].ulTopology,
                        UnknownWave,
                        PhysicalConnections[i].ulWave);
                    break;
                case CONNECTIONTYPE_WAVE_OUTPUT:
                    ntStatus2 = unregisterPhysicalConnection->UnregisterPhysicalConnection(
                        m_pDeviceObject,
                        UnknownWave,
                        PhysicalConnections[i].ulWave,
                        UnknownTopology,
                        PhysicalConnections[i].ulTopology);
                    break;
            }

            if (NT_SUCCESS(ntStatus))
            {
                ntStatus = ntStatus2;
            }
        }
    }

    SAFE_RELEASE(unregisterPhysicalConnection);
    return ntStatus;
}

//=============================================================================
// Sub-device cache
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS CVadAdapterCommon::GetCachedSubdevice(
    _In_ PWSTR Name,
    _Out_opt_ PUNKNOWN *OutUnknownPort,
    _Out_opt_ PUNKNOWN *OutUnknownMiniport
)
{
    PAGED_CODE();

    PLIST_ENTRY le = NULL;
    BOOL bFound = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bFound; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);
        if (0 == wcscmp(Name, pRecord->Name))
        {
            if (OutUnknownPort)
            {
                *OutUnknownPort = pRecord->PortInterface;
                (*OutUnknownPort)->AddRef();
            }
            if (OutUnknownMiniport)
            {
                *OutUnknownMiniport = pRecord->MiniportInterface;
                (*OutUnknownMiniport)->AddRef();
            }
            bFound = TRUE;
        }
    }

    return bFound ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

#pragma code_seg("PAGE")
NTSTATUS CVadAdapterCommon::CacheSubdevice(
    _In_ PWSTR Name,
    _In_ PUNKNOWN UnknownPort,
    _In_ PUNKNOWN UnknownMiniport
)
{
    PAGED_CODE();

    NTSTATUS         ntStatus       = STATUS_SUCCESS;
    MINIPAIR_UNKNOWN *pNewSubdevice = NULL;

    pNewSubdevice = new(POOL_FLAG_NON_PAGED, VAD_ADAPTER_POOLTAG) MINIPAIR_UNKNOWN;
    if (!pNewSubdevice)
    {
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(ntStatus))
    {
        memset(pNewSubdevice, 0, sizeof(MINIPAIR_UNKNOWN));
        ntStatus = RtlStringCchCopyW(pNewSubdevice->Name, SIZEOF_ARRAY(pNewSubdevice->Name), Name);
    }

    if (NT_SUCCESS(ntStatus))
    {
        pNewSubdevice->PortInterface = UnknownPort;
        pNewSubdevice->PortInterface->AddRef();
        pNewSubdevice->MiniportInterface = UnknownMiniport;
        pNewSubdevice->MiniportInterface->AddRef();
        UnknownMiniport->QueryInterface(IID_IAdapterPowerManagement, (PVOID *)&(pNewSubdevice->PowerInterface));
        InsertTailList(&m_SubdeviceCache, &pNewSubdevice->ListEntry);
    }

    if (!NT_SUCCESS(ntStatus))
    {
        if (pNewSubdevice)
        {
            delete pNewSubdevice;
        }
    }

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS CVadAdapterCommon::RemoveCachedSubdevice(_In_ PWSTR Name)
{
    PAGED_CODE();

    PLIST_ENTRY le = NULL;
    BOOL bRemoved = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bRemoved; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);
        if (0 == wcscmp(Name, pRecord->Name))
        {
            SAFE_RELEASE(pRecord->PortInterface);
            SAFE_RELEASE(pRecord->MiniportInterface);
            SAFE_RELEASE(pRecord->PowerInterface);
            memset(pRecord->Name, 0, sizeof(pRecord->Name));
            RemoveEntryList(le);
            bRemoved = TRUE;
            delete pRecord;
            break;
        }
    }

    return bRemoved ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

#pragma code_seg("PAGE")
VOID CVadAdapterCommon::EmptySubdeviceCache()
{
    PAGED_CODE();

    while (!IsListEmpty(&m_SubdeviceCache))
    {
        PLIST_ENTRY le = RemoveHeadList(&m_SubdeviceCache);
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);
        SAFE_RELEASE(pRecord->PortInterface);
        SAFE_RELEASE(pRecord->MiniportInterface);
        SAFE_RELEASE(pRecord->PowerInterface);
        memset(pRecord->Name, 0, sizeof(pRecord->Name));
        delete pRecord;
    }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CVadAdapterCommon::Cleanup()
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::Cleanup]"));
    EmptySubdeviceCache();
}

//=============================================================================
// InstallEndpointFilters
//=============================================================================

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::InstallEndpointFilters
(
    _In_opt_    PIRP                Irp,
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _In_opt_    PVOID               DeviceContext,
    _Out_opt_   PUNKNOWN *          UnknownTopology,
    _Out_opt_   PUNKNOWN *          UnknownWave,
    _Out_opt_   PUNKNOWN *          UnknownMiniportTopology,
    _Out_opt_   PUNKNOWN *          UnknownMiniportWave
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::InstallEndpointFilters]"));

    NTSTATUS            ntStatus            = STATUS_SUCCESS;
    PUNKNOWN            unknownTopology     = NULL;
    PUNKNOWN            unknownWave         = NULL;
    BOOL                bTopologyCreated    = FALSE;
    BOOL                bWaveCreated        = FALSE;
    PUNKNOWN            unknownMiniTopo     = NULL;
    PUNKNOWN            unknownMiniWave     = NULL;

    if (UnknownTopology)     { *UnknownTopology = NULL; }
    if (UnknownWave)         { *UnknownWave = NULL; }
    if (UnknownMiniportTopology) { *UnknownMiniportTopology = NULL; }
    if (UnknownMiniportWave)     { *UnknownMiniportWave = NULL; }

    ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopology, &unknownMiniTopo);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownTopology || NULL == unknownMiniTopo)
    {
        bTopologyCreated = TRUE;

        ntStatus = InstallSubdevice(Irp,
                                    MiniportPair->TopoName,
                                    CLSID_PortTopology,
                                    CLSID_PortTopology,
                                    MiniportPair->TopoCreateCallback,
                                    MiniportPair->TopoInterfacePropertyCount,
                                    MiniportPair->TopoInterfaceProperties,
                                    DeviceContext,
                                    MiniportPair,
                                    NULL,
                                    IID_IPortTopology,
                                    NULL,
                                    &unknownTopology,
                                    &unknownMiniTopo);
        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = CacheSubdevice(MiniportPair->TopoName, unknownTopology, unknownMiniTopo);
        }
    }

    ntStatus = GetCachedSubdevice(MiniportPair->WaveName, &unknownWave, &unknownMiniWave);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownWave || NULL == unknownMiniWave)
    {
        bWaveCreated = TRUE;

        ntStatus = InstallSubdevice(Irp,
                                    MiniportPair->WaveName,
                                    CLSID_PortWaveRT,
                                    CLSID_PortWaveRT,
                                    MiniportPair->WaveCreateCallback,
                                    MiniportPair->WaveInterfacePropertyCount,
                                    MiniportPair->WaveInterfaceProperties,
                                    DeviceContext,
                                    MiniportPair,
                                    NULL,
                                    IID_IPortWaveRT,
                                    NULL,
                                    &unknownWave,
                                    &unknownMiniWave);

        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = CacheSubdevice(MiniportPair->WaveName, unknownWave, unknownMiniWave);
        }
    }

    if (unknownTopology && unknownWave)
    {
        ntStatus = ConnectTopologies(
            unknownTopology,
            unknownWave,
            MiniportPair->PhysicalConnections,
            MiniportPair->PhysicalConnectionCount);
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (UnknownTopology != NULL && unknownTopology != NULL)
        {
            unknownTopology->AddRef();
            *UnknownTopology = unknownTopology;
        }
        if (UnknownWave != NULL && unknownWave != NULL)
        {
            unknownWave->AddRef();
            *UnknownWave = unknownWave;
        }
        if (UnknownMiniportTopology != NULL && unknownMiniTopo != NULL)
        {
            unknownMiniTopo->AddRef();
            *UnknownMiniportTopology = unknownMiniTopo;
        }
        if (UnknownMiniportWave != NULL && unknownMiniWave != NULL)
        {
            unknownMiniWave->AddRef();
            *UnknownMiniportWave = unknownMiniWave;
        }
    }
    else
    {
        if (bTopologyCreated && unknownTopology != NULL)
        {
            UnregisterSubdevice(unknownTopology);
            RemoveCachedSubdevice(MiniportPair->TopoName);
        }
        if (bWaveCreated && unknownWave != NULL)
        {
            UnregisterSubdevice(unknownWave);
            RemoveCachedSubdevice(MiniportPair->WaveName);
        }
    }

    SAFE_RELEASE(unknownMiniTopo);
    SAFE_RELEASE(unknownTopology);
    SAFE_RELEASE(unknownMiniWave);
    SAFE_RELEASE(unknownWave);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::RemoveEndpointFilters
(
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _In_opt_    PUNKNOWN            UnknownTopology,
    _In_opt_    PUNKNOWN            UnknownWave
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::RemoveEndpointFilters]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;

    if (UnknownTopology != NULL && UnknownWave != NULL)
    {
        ntStatus = DisconnectTopologies(
            UnknownTopology,
            UnknownWave,
            MiniportPair->PhysicalConnections,
            MiniportPair->PhysicalConnectionCount);

        UnregisterSubdevice(UnknownWave);
        RemoveCachedSubdevice(MiniportPair->WaveName);

        UnregisterSubdevice(UnknownTopology);
        RemoveCachedSubdevice(MiniportPair->TopoName);
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::GetFilters
(
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _Out_opt_   PUNKNOWN            *UnknownTopologyPort,
    _Out_opt_   PUNKNOWN            *UnknownTopologyMiniport,
    _Out_opt_   PUNKNOWN            *UnknownWavePort,
    _Out_opt_   PUNKNOWN            *UnknownWaveMiniport
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::GetFilters]"));

    NTSTATUS ntStatus;
    PUNKNOWN unknownTopoPort = NULL;
    PUNKNOWN unknownTopoMiniport = NULL;
    PUNKNOWN unknownWavePort = NULL;
    PUNKNOWN unknownWaveMiniport = NULL;

    ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopoPort, &unknownTopoMiniport);
    if (!NT_SUCCESS(ntStatus))
    {
        goto Done;
    }

    ntStatus = GetCachedSubdevice(MiniportPair->WaveName, &unknownWavePort, &unknownWaveMiniport);
    if (!NT_SUCCESS(ntStatus))
    {
        goto Done;
    }

    if (UnknownTopologyPort && unknownTopoPort)
    {
        unknownTopoPort->AddRef();
        *UnknownTopologyPort = unknownTopoPort;
    }
    if (UnknownTopologyMiniport && unknownTopoMiniport)
    {
        unknownTopoMiniport->AddRef();
        *UnknownTopologyMiniport = unknownTopoMiniport;
    }
    if (UnknownWavePort && unknownWavePort)
    {
        unknownWavePort->AddRef();
        *UnknownWavePort = unknownWavePort;
    }
    if (UnknownWaveMiniport && unknownWaveMiniport)
    {
        unknownWaveMiniport->AddRef();
        *UnknownWaveMiniport = unknownWaveMiniport;
    }

Done:
    SAFE_RELEASE(unknownTopoPort);
    SAFE_RELEASE(unknownTopoMiniport);
    SAFE_RELEASE(unknownWavePort);
    SAFE_RELEASE(unknownWaveMiniport);

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadAdapterCommon::SetIdlePowerManagement
(
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _In_        BOOL                bEnabled
)
{
    PAGED_CODE();
    DPF_ENTER(("[CVadAdapterCommon::SetIdlePowerManagement]"));

    UNREFERENCED_PARAMETER(MiniportPair);
    UNREFERENCED_PARAMETER(bEnabled);

    return STATUS_SUCCESS;
}

#pragma code_seg()
