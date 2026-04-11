#ifndef _APPSANDBOXVAD_WAVMINIPORT_H_
#define _APPSANDBOXVAD_WAVMINIPORT_H_

//=============================================================================
// Forward declarations
//=============================================================================
class CVadWaveStream;
typedef CVadWaveStream *PCVadWaveStream;

//=============================================================================
// CVadWaveMiniport
//=============================================================================

class CVadWaveMiniport :
    public IMiniportWaveRT,
    public IMiniportAudioSignalProcessing,
    public CUnknown
{
private:
    ULONG                               m_ulSystemAllocated;
    ULONG                               m_ulMaxSystemStreams;

    // Weak ref of running streams
    PCVadWaveStream                   * m_SystemStreams;

    BOOL                                m_bGfxEnabled;
    PBOOL                               m_pbMuted;
    PLONG                               m_plVolumeLevel;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE  m_pMixFormat;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE  m_pDeviceFormat;
    PCFILTER_DESCRIPTOR                 m_FilterDesc;
    PIN_DEVICE_FORMATS_AND_MODES *      m_DeviceFormatsAndModes;
    KSPIN_LOCK                          m_DeviceFormatsAndModesLock;
    KIRQL                               m_DeviceFormatsAndModesIrql;
    ULONG                               m_DeviceFormatsAndModesCount;
    USHORT                              m_DeviceMaxChannels;
    PDRMPORT                            m_pDrmPort;
    DRMRIGHTS                           m_MixDrmRights;
    ULONG                               m_ulMixDrmContentId;

    union {
        PVOID                           m_DeviceContext;
    };

protected:
    PADAPTERCOMMON                      m_pAdapterCommon;
    ULONG                               m_DeviceFlags;
    eDeviceType                         m_DeviceType;
    PPORTEVENTS                         m_pPortEvents;
    PENDPOINT_MINIPAIR                  m_pMiniportPair;

public:
    NTSTATUS EventHandler_PinCapsChange
    (
        _In_  PPCEVENT_REQUEST EventRequest
    );

    NTSTATUS ValidateStreamCreate
    (
        _In_    ULONG   _Pin,
        _In_    BOOLEAN _Capture
    );

    NTSTATUS StreamCreated
    (
        _In_ ULONG              _Pin,
        _In_ PCVadWaveStream    _Stream
    );

    NTSTATUS StreamClosed
    (
        _In_ ULONG              _Pin,
        _In_ PCVadWaveStream    _Stream
    );

    NTSTATUS IsFormatSupported
    (
        _In_ ULONG          _ulPin,
        _In_ BOOLEAN        _bCapture,
        _In_ PKSDATAFORMAT  _pDataFormat
    );

    static NTSTATUS GetAttributesFromAttributeList
    (
        _In_ const KSMULTIPLE_ITEM *_pAttributes,
        _In_ size_t _Size,
        _Out_ GUID* _pSignalProcessingMode
    );

protected:
    NTSTATUS UpdateDrmRights(void);

public:
    DECLARE_STD_UNKNOWN();

#pragma code_seg("PAGE")
    CVadWaveMiniport(
        _In_            PUNKNOWN                UnknownAdapter,
        _In_            PENDPOINT_MINIPAIR      MiniportPair,
        _In_opt_        PVOID                   DeviceContext
    )
        :CUnknown(0),
        m_ulMaxSystemStreams(0),
        m_DeviceType(MiniportPair->DeviceType),
        m_DeviceContext(DeviceContext),
        m_DeviceMaxChannels(MiniportPair->DeviceMaxChannels),
        m_DeviceFormatsAndModes(MiniportPair->PinDeviceFormatsAndModes),
        m_DeviceFormatsAndModesCount(MiniportPair->PinDeviceFormatsAndModesCount),
        m_DeviceFlags(MiniportPair->DeviceFlags),
        m_pMiniportPair(MiniportPair)
    {
        PAGED_CODE();

        m_pAdapterCommon = (PADAPTERCOMMON)UnknownAdapter; // weak ref

        if (MiniportPair->WaveDescriptor)
        {
            RtlCopyMemory(&m_FilterDesc, MiniportPair->WaveDescriptor, sizeof(m_FilterDesc));

            if (IsRenderDevice())
            {
                if (m_FilterDesc.PinCount > KSPIN_WAVE_RENDER_SOURCE)
                {
                    m_ulMaxSystemStreams = m_FilterDesc.Pins[KSPIN_WAVE_RENDER_SINK_SYSTEM].MaxFilterInstanceCount;
                }
            }
        }

        KeInitializeSpinLock(&m_DeviceFormatsAndModesLock);
        m_DeviceFormatsAndModesIrql = PASSIVE_LEVEL;
    }
#pragma code_seg()

    ~CVadWaveMiniport();

    IMP_IMiniportWaveRT;
    IMP_IMiniportAudioSignalProcessing;

    friend class        CVadWaveStream;

    friend NTSTATUS VadWaveFilterHandler
    (
        _In_ PPCPROPERTY_REQUEST      PropertyRequest
    );

public:
    NTSTATUS PropertyHandlerProposedFormat
    (
        _In_ PPCPROPERTY_REQUEST PropertyRequest
    );

    NTSTATUS PropertyHandlerProposedFormat2
    (
        _In_ PPCPROPERTY_REQUEST PropertyRequest
    );

    PADAPTERCOMMON GetAdapterCommObj()
    {
        return m_pAdapterCommon;
    };

#pragma code_seg()

private:
    _IRQL_raises_(DISPATCH_LEVEL)
    _Acquires_lock_(m_DeviceFormatsAndModesLock)
    _Requires_lock_not_held_(m_DeviceFormatsAndModesLock)
    _IRQL_saves_global_(SpinLock, m_DeviceFormatsAndModesIrql)
    VOID AcquireFormatsAndModesLock();

    _Releases_lock_(m_DeviceFormatsAndModesLock)
    _Requires_lock_held_(m_DeviceFormatsAndModesLock)
    _IRQL_restores_global_(SpinLock, m_DeviceFormatsAndModesIrql)
    VOID ReleaseFormatsAndModesLock();

    _Post_satisfies_(return > 0)
    ULONG GetPinSupportedDeviceFormats(_In_ ULONG PinId, _Outptr_opt_result_buffer_(return) KSDATAFORMAT_WAVEFORMATEXTENSIBLE **ppFormats);

    _Success_(return != 0)
    ULONG GetPinSupportedDeviceModes(_In_ ULONG PinId, _Outptr_opt_result_buffer_(return) _On_failure_(_Deref_post_null_) MODE_AND_DEFAULT_FORMAT **ppModes);

#pragma code_seg()

protected:
    BOOL IsRenderDevice()
    {
        return m_DeviceType == eSpeakerDevice ? TRUE : FALSE;
    }

    BOOL IsSystemRenderPin(ULONG nPinId);
    BOOL IsBridgePin(ULONG nPinId);

    ULONG GetSystemPinId()
    {
        ASSERT(IsRenderDevice());
        return KSPIN_WAVE_RENDER_SINK_SYSTEM;
    }
};

typedef CVadWaveMiniport *PCVadWaveMiniport;

#endif // _APPSANDBOXVAD_WAVMINIPORT_H_
