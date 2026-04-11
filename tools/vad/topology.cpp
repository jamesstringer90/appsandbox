#pragma warning (disable : 4127)

#include "driverdefs.h"
#include "pinnodes.h"
#include "wavminiport.h"
#include "topofilter.h"

//=============================================================================
// CVadTopologyMiniport
//
// Single topology miniport class for the speaker endpoint.
//=============================================================================

class CVadTopologyMiniport :
    public IMiniportTopology,
    public CUnknown
{
private:
    PADAPTERCOMMON              m_AdapterCommon;
    PCFILTER_DESCRIPTOR        *m_FilterDescriptor;
    PPORTEVENTS                 m_PortEvents;
    USHORT                      m_DeviceMaxChannels;
    eDeviceType                 m_DeviceType;

    union {
        PVOID                   m_DeviceContext;
    };

public:
    DECLARE_STD_UNKNOWN();

    CVadTopologyMiniport
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PCFILTER_DESCRIPTOR    *FilterDesc,
        _In_        USHORT                  DeviceMaxChannels,
        _In_        eDeviceType             DeviceType,
        _In_opt_    PVOID                   DeviceContext
    )
    : CUnknown(UnknownOuter),
      m_AdapterCommon(NULL),
      m_FilterDescriptor(FilterDesc),
      m_PortEvents(NULL),
      m_DeviceMaxChannels(DeviceMaxChannels),
      m_DeviceType(DeviceType),
      m_DeviceContext(DeviceContext)
    {
        ASSERT(FilterDesc != NULL);
        ASSERT(DeviceMaxChannels > 0);
    }

    ~CVadTopologyMiniport();

    IMP_IMiniportTopology;

    NTSTATUS PropertyHandlerGeneric
    (
        _In_  PPCPROPERTY_REQUEST     PropertyRequest
    );

    NTSTATUS PropertyHandlerJackDescription
    (
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions
    );

    NTSTATUS PropertyHandlerJackDescription2
    (
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions,
        _In_        DWORD                                       JackCapabilities
    );
};

typedef CVadTopologyMiniport *PCVadTopologyMiniport;

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CreateVadTopologyMiniport
(
    _Out_           PUNKNOWN *                              Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(UnknownAdapter);

    ASSERT(Unknown);
    ASSERT(MiniportPair);

    CVadTopologyMiniport *obj =
        new (PoolFlags, VAD_TOPO_POOLTAG)
            CVadTopologyMiniport(
                UnknownOuter,
                MiniportPair->TopoDescriptor,
                MiniportPair->DeviceMaxChannels,
                MiniportPair->DeviceType,
                DeviceContext);
    if (NULL == obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();
    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
CVadTopologyMiniport::~CVadTopologyMiniport(void)
{
    PAGED_CODE();

    DPF_ENTER(("[CVadTopologyMiniport::~CVadTopologyMiniport]"));

    SAFE_RELEASE(m_AdapterCommon);
    SAFE_RELEASE(m_PortEvents);
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadTopologyMiniport::DataRangeIntersection
(
    _In_        ULONG                       PinId,
    _In_        PKSDATARANGE                ClientDataRange,
    _In_        PKSDATARANGE                MyDataRange,
    _In_        ULONG                       OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                       ResultantFormat,
    _Out_       PULONG                      ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);

    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadTopologyMiniport::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR *  OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    *OutFilterDescriptor = m_FilterDescriptor;

    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadTopologyMiniport::Init
(
    _In_ PUNKNOWN                 UnknownAdapter,
    _In_ PRESOURCELIST            ResourceList,
    _In_ PPORTTOPOLOGY            Port_
)
{
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(UnknownAdapter);
    ASSERT(Port_);

    DPF_ENTER(("[CVadTopologyMiniport::Init]"));

    NTSTATUS ntStatus;

    ntStatus =
        UnknownAdapter->QueryInterface(
            IID_IAdapterCommon,
            (PVOID *)&m_AdapterCommon);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = Port_->QueryInterface(
            IID_IPortEvents,
            (PVOID *)&m_PortEvents);
    }

    if (NT_SUCCESS(ntStatus))
    {
        m_AdapterCommon->MixerReset();
    }

    if (!NT_SUCCESS(ntStatus))
    {
        SAFE_RELEASE(m_AdapterCommon);
        SAFE_RELEASE(m_PortEvents);
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CVadTopologyMiniport::NonDelegatingQueryInterface
(
    _In_ REFIID                  Interface,
    _COM_Outptr_ PVOID      * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
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
NTSTATUS
CVadTopologyMiniport::PropertyHandlerGeneric
(
    _In_  PPCPROPERTY_REQUEST     PropertyRequest
)
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    switch (PropertyRequest->PropertyItem->Id)
    {
        case KSPROPERTY_AUDIO_VOLUMELEVEL:
            ntStatus = PropertyHandler_Volume(
                m_AdapterCommon,
                PropertyRequest,
                m_DeviceMaxChannels);
            break;

        case KSPROPERTY_AUDIO_MUTE:
            ntStatus = PropertyHandler_Mute(
                m_AdapterCommon,
                PropertyRequest,
                m_DeviceMaxChannels);
            break;

        case KSPROPERTY_AUDIO_PEAKMETER2:
            ntStatus = PropertyHandler_PeakMeter2(
                m_AdapterCommon,
                PropertyRequest,
                m_DeviceMaxChannels);
            break;

        case KSPROPERTY_AUDIO_CPU_RESOURCES:
            ntStatus = PropertyHandler_CpuResources(PropertyRequest);
            break;

        default:
            DPF(D_TERSE, ("[PropertyHandlerGeneric: Invalid Device Request]"));
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CVadTopologyMiniport::PropertyHandlerJackDescription
(
    _In_        PPCPROPERTY_REQUEST                      PropertyRequest,
    _In_        ULONG                                    cJackDescriptions,
    _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION *  JackDescriptions
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[CVadTopologyMiniport::PropertyHandlerJackDescription]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    nPinId = (ULONG)-1;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if ((nPinId < cJackDescriptions) && (JackDescriptions[nPinId] != NULL))
        {
            if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
            {
                ntStatus =
                    PropertyHandler_BasicSupport
                    (
                        PropertyRequest,
                        KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                        VT_ILLEGAL
                    );
            }
            else
            {
                ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);

                if (PropertyRequest->ValueSize == 0)
                {
                    PropertyRequest->ValueSize = cbNeeded;
                    ntStatus = STATUS_BUFFER_OVERFLOW;
                }
                else if (PropertyRequest->ValueSize < cbNeeded)
                {
                    ntStatus = STATUS_BUFFER_TOO_SMALL;
                }
                else
                {
                    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
                    {
                        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
                        PKSJACK_DESCRIPTION pDesc = (PKSJACK_DESCRIPTION)(pMI + 1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        RtlCopyMemory(pDesc, JackDescriptions[nPinId], sizeof(KSJACK_DESCRIPTION));
                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CVadTopologyMiniport::PropertyHandlerJackDescription2
(
    _In_        PPCPROPERTY_REQUEST                      PropertyRequest,
    _In_        ULONG                                    cJackDescriptions,
    _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION *  JackDescriptions,
    _In_        DWORD                                    JackCapabilities
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[CVadTopologyMiniport::PropertyHandlerJackDescription2]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    nPinId = (ULONG)-1;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if ((nPinId < cJackDescriptions) && (JackDescriptions[nPinId] != NULL))
        {
            if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
            {
                ntStatus =
                    PropertyHandler_BasicSupport
                    (
                        PropertyRequest,
                        KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                        VT_ILLEGAL
                    );
            }
            else
            {
                ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);

                if (PropertyRequest->ValueSize == 0)
                {
                    PropertyRequest->ValueSize = cbNeeded;
                    ntStatus = STATUS_BUFFER_OVERFLOW;
                }
                else if (PropertyRequest->ValueSize < cbNeeded)
                {
                    ntStatus = STATUS_BUFFER_TOO_SMALL;
                }
                else
                {
                    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
                    {
                        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
                        PKSJACK_DESCRIPTION2 pDesc = (PKSJACK_DESCRIPTION2)(pMI + 1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        RtlZeroMemory(pDesc, sizeof(KSJACK_DESCRIPTION2));
                        pDesc->DeviceStateInfo = 0;
                        pDesc->JackCapabilities = JackCapabilities;

                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

//=============================================================================
// Speaker filter property handler (jack description)
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
VadSpeakerFilterHandler
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[VadSpeakerFilterHandler]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCVadTopologyMiniport pMiniport = (PCVadTopologyMiniport)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions
            );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions,
                0
            );
        }
    }

    return ntStatus;
}

//=============================================================================
// Speaker topology property handler (volume, mute)
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
VadSpeakerTopoHandler
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[VadSpeakerTopoHandler]"));

    PCVadTopologyMiniport pMiniport = (PCVadTopologyMiniport)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
}

//=============================================================================
// Generic topology property handler
//=============================================================================

#pragma code_seg("PAGE")
NTSTATUS
VadTopoPropertyHandler
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[VadTopoPropertyHandler]"));

    return
        ((PCVadTopologyMiniport)
        (PropertyRequest->MajorTarget))->PropertyHandlerGeneric
                    (
                        PropertyRequest
                    );
}

#pragma code_seg()
