#ifndef _APPSANDBOXVAD_ADAPTERCOMMON_H_
#define _APPSANDBOXVAD_ADAPTERCOMMON_H_

//=============================================================================
// Macros
//=============================================================================

#define UNREFERENCED_VAR(status) \
    status = status

#define JUMP(label)                                             \
        goto label;

#define IF_TRUE_JUMP(condition, label)                          \
    if (condition)                                               \
    {                                                           \
        goto label;                                             \
    }

#define IF_TRUE_ACTION_JUMP(condition, action, label)           \
    if (condition)                                               \
    {                                                           \
        action;                                                 \
        goto label;                                             \
    }

#define IF_FAILED_ACTION_JUMP(ntStatus, action, label)          \
        if (!NT_SUCCESS(ntStatus))                              \
        {                                                       \
            action;                                             \
            goto label;                                         \
        }

#define IF_FAILED_JUMP(ntStatus, label)                         \
    if (!NT_SUCCESS(ntStatus))                                   \
    {                                                           \
        goto label;                                             \
    }

#define SAFE_RELEASE(p) {if (p) { (p)->Release(); (p) = nullptr; } }
#define SAFE_DELETE_PTR_WITH_TAG(ptr, tag) if(ptr) { ExFreePoolWithTag((ptr), tag); (ptr) = NULL; }

#define JACKDESC_RGB(r, g, b) \
    ((COLORREF)((r << 16) | (g << 8) | (b)))

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

//=============================================================================
// Enumerations
//=============================================================================

typedef enum
{
    eSpeakerDevice = 0,
    eMaxDeviceType,
} eDeviceType;

//=============================================================================
// Signal processing modes and default formats
//=============================================================================

typedef struct _MODE_AND_DEFAULT_FORMAT {
    GUID            Mode;
    KSDATAFORMAT*   DefaultFormat;
} MODE_AND_DEFAULT_FORMAT, *PMODE_AND_DEFAULT_FORMAT;

//=============================================================================
// Pin types
//=============================================================================

typedef enum
{
    NoPin,
    BridgePin,
    SystemRenderPin,
} PINTYPE;

//=============================================================================
// Pin device formats and modes
//=============================================================================

typedef struct _PIN_DEVICE_FORMATS_AND_MODES
{
    PINTYPE                             PinType;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE * WaveFormats;
    ULONG                               WaveFormatsCount;
    MODE_AND_DEFAULT_FORMAT *           ModeAndDefaultFormat;
    ULONG                               ModeAndDefaultFormatCount;
} PIN_DEVICE_FORMATS_AND_MODES, *PPIN_DEVICE_FORMATS_AND_MODES;

//=============================================================================
// Connection type for physical bridge
//=============================================================================
typedef enum {
    CONNECTIONTYPE_TOPOLOGY_OUTPUT = 0,
    CONNECTIONTYPE_WAVE_OUTPUT     = 1
} CONNECTIONTYPE;

typedef struct _PHYSICALCONNECTIONTABLE
{
    ULONG            ulTopology;
    ULONG            ulWave;
    CONNECTIONTYPE   eType;
} PHYSICALCONNECTIONTABLE, *PPHYSICALCONNECTIONTABLE;

//=============================================================================
// Forward declaration
//=============================================================================
typedef struct _ENDPOINT_MINIPAIR *PENDPOINT_MINIPAIR;

//=============================================================================
// Miniport create function prototype
//=============================================================================
typedef HRESULT (*PFNCREATEMINIPORT)(
    _Out_           PUNKNOWN                              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
);

//=============================================================================
// Device property for interface registration
//=============================================================================
typedef struct _VAD_DEVPROPERTY {
    const DEVPROPKEY   *PropertyKey;
    DEVPROPTYPE Type;
    ULONG BufferSize;
    __field_bcount_opt(BufferSize) PVOID Buffer;
} VAD_DEVPROPERTY, PVAD_DEVPROPERTY;

#define ENDPOINT_NO_FLAGS   0x00000000

//=============================================================================
// Endpoint miniport pair (wave/topology descriptor)
//=============================================================================
typedef struct _ENDPOINT_MINIPAIR
{
    eDeviceType                     DeviceType;

    // Topology miniport
    PWSTR                           TopoName;
    PFNCREATEMINIPORT               TopoCreateCallback;
    PCFILTER_DESCRIPTOR*            TopoDescriptor;
    ULONG                           TopoInterfacePropertyCount;
    const VAD_DEVPROPERTY*          TopoInterfaceProperties;

    // Wave RT miniport
    PWSTR                           WaveName;
    PFNCREATEMINIPORT               WaveCreateCallback;
    PCFILTER_DESCRIPTOR*            WaveDescriptor;
    ULONG                           WaveInterfacePropertyCount;
    const VAD_DEVPROPERTY*          WaveInterfaceProperties;

    USHORT                          DeviceMaxChannels;
    PIN_DEVICE_FORMATS_AND_MODES*   PinDeviceFormatsAndModes;
    ULONG                           PinDeviceFormatsAndModesCount;

    // Physical connections between wave and topology
    PHYSICALCONNECTIONTABLE*        PhysicalConnections;
    ULONG                           PhysicalConnectionCount;

    ULONG                           DeviceFlags;
} ENDPOINT_MINIPAIR;

//=============================================================================
// PortCls FDO device extension layout
//=============================================================================
struct IAdapterCommon;
typedef struct _PortClassDeviceContext
{
    ULONG_PTR m_pulReserved1[2];
    PDEVICE_OBJECT m_DoNotUsePhysicalDeviceObject;
    PVOID m_pvReserved2;
    PVOID m_pvReserved3;
    IAdapterCommon* m_pCommon;
    PVOID m_pvUnused1;
    PVOID m_pvUnused2;
} PortClassDeviceContext;

//=============================================================================
// Major/Minor target casting
//=============================================================================
#define MajorTarget_to_Obj(ptr) \
    reinterpret_cast<CVadWaveMiniport*>(ptr)

#define MinorTarget_to_Obj(ptr) \
    static_cast<CVadWaveStream*>(reinterpret_cast<PMINIPORTWAVERTSTREAM>(ptr))

//=============================================================================
// IAdapterCommon interface
// {8B2E4F60-A71C-4D39-BE56-0C3A7F8D9E1B}
//=============================================================================
DEFINE_GUID(IID_IAdapterCommon,
0x8b2e4f60, 0xa71c, 0x4d39, 0xbe, 0x56, 0x0c, 0x3a, 0x7f, 0x8d, 0x9e, 0x1b);

DECLARE_INTERFACE_(IAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS,        Init)
    (
        THIS_
        _In_  PDEVICE_OBJECT      DeviceObject
    ) PURE;

    STDMETHOD_(PDEVICE_OBJECT,  GetDeviceObject)
    (
        THIS
    ) PURE;

    STDMETHOD_(PDEVICE_OBJECT,  GetPhysicalDeviceObject)
    (
        THIS
    ) PURE;

    STDMETHOD_(WDFDEVICE,       GetWdfDevice)
    (
        THIS
    ) PURE;

    STDMETHOD_(VOID,            SetWaveServiceGroup)
    (
        THIS_
        _In_ PSERVICEGROUP        ServiceGroup
    ) PURE;

    STDMETHOD_(BOOL,            bDevSpecificRead)
    (
        THIS_
    ) PURE;

    STDMETHOD_(VOID,            bDevSpecificWrite)
    (
        THIS_
        _In_  BOOL                bDevSpecific
    );

    STDMETHOD_(INT,             iDevSpecificRead)
    (
        THIS_
    ) PURE;

    STDMETHOD_(VOID,            iDevSpecificWrite)
    (
        THIS_
        _In_  INT                 iDevSpecific
    );

    STDMETHOD_(UINT,            uiDevSpecificRead)
    (
        THIS_
    ) PURE;

    STDMETHOD_(VOID,            uiDevSpecificWrite)
    (
        THIS_
        _In_  UINT                uiDevSpecific
    );

    STDMETHOD_(BOOL,            MixerMuteRead)
    (
        THIS_
        _In_  ULONG               Index,
        _In_  ULONG               Channel
    ) PURE;

    STDMETHOD_(VOID,            MixerMuteWrite)
    (
        THIS_
        _In_  ULONG               Index,
        _In_  ULONG               Channel,
        _In_  BOOL                Value
    );

    STDMETHOD_(ULONG,           MixerMuxRead)
    (
        THIS
    );

    STDMETHOD_(VOID,            MixerMuxWrite)
    (
        THIS_
        _In_  ULONG               Index
    );

    STDMETHOD_(LONG,            MixerVolumeRead)
    (
        THIS_
        _In_  ULONG               Index,
        _In_  ULONG               Channel
    ) PURE;

    STDMETHOD_(VOID,            MixerVolumeWrite)
    (
        THIS_
        _In_  ULONG               Index,
        _In_  ULONG               Channel,
        _In_  LONG                Value
    ) PURE;

    STDMETHOD_(LONG,            MixerPeakMeterRead)
    (
        THIS_
        _In_  ULONG               Index,
        _In_  ULONG               Channel
    ) PURE;

    STDMETHOD_(VOID,            MixerReset)
    (
        THIS
    ) PURE;

    STDMETHOD_(NTSTATUS,        WriteEtwEvent)
    (
        THIS_
        _In_ EPcMiniportEngineEvent    miniportEventType,
        _In_ ULONGLONG  ullData1,
        _In_ ULONGLONG  ullData2,
        _In_ ULONGLONG  ullData3,
        _In_ ULONGLONG  ullData4
    ) PURE;

    STDMETHOD_(VOID,            SetEtwHelper)
    (
        THIS_
        PPORTCLSETWHELPER _pPortClsEtwHelper
    ) PURE;

    STDMETHOD_(NTSTATUS,        InstallSubdevice)
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

    STDMETHOD_(NTSTATUS,        UnregisterSubdevice)
    (
        THIS_
        _In_opt_   PUNKNOWN     UnknownPort
    );

    STDMETHOD_(NTSTATUS,        ConnectTopologies)
    (
        THIS_
        _In_ PUNKNOWN                   UnknownTopology,
        _In_ PUNKNOWN                   UnknownWave,
        _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
        _In_ ULONG                      PhysicalConnectionCount
    );

    STDMETHOD_(NTSTATUS,        DisconnectTopologies)
    (
        THIS_
        _In_ PUNKNOWN                   UnknownTopology,
        _In_ PUNKNOWN                   UnknownWave,
        _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
        _In_ ULONG                      PhysicalConnectionCount
    );

    STDMETHOD_(NTSTATUS,        InstallEndpointFilters)
    (
        THIS_
        _In_opt_    PIRP                Irp,
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_opt_    PVOID               DeviceContext,
        _Out_opt_   PUNKNOWN *          UnknownTopology,
        _Out_opt_   PUNKNOWN *          UnknownWave,
        _Out_opt_   PUNKNOWN *          UnknownMiniportTopology,
        _Out_opt_   PUNKNOWN *          UnknownMiniportWave
    );

    STDMETHOD_(NTSTATUS,        RemoveEndpointFilters)
    (
        THIS_
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_opt_    PUNKNOWN            UnknownTopology,
        _In_opt_    PUNKNOWN            UnknownWave
    );

    STDMETHOD_(NTSTATUS,        GetFilters)
    (
        THIS_
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _Out_opt_   PUNKNOWN            *UnknownTopologyPort,
        _Out_opt_   PUNKNOWN            *UnknownTopologyMiniport,
        _Out_opt_   PUNKNOWN            *UnknownWavePort,
        _Out_opt_   PUNKNOWN            *UnknownWaveMiniport
    );

    STDMETHOD_(NTSTATUS,        SetIdlePowerManagement)
    (
        THIS_
        _In_        PENDPOINT_MINIPAIR  MiniportPair,
        _In_        BOOL                bEnable
    );

    STDMETHOD_(VOID, Cleanup)();
};

typedef IAdapterCommon *PADAPTERCOMMON;

//=============================================================================
// Factory function
//=============================================================================
NTSTATUS
NewAdapterCommon
(
    _Out_       PUNKNOWN *              Unknown,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        POOL_FLAGS              PoolFlags
);

#endif // _APPSANDBOXVAD_ADAPTERCOMMON_H_
