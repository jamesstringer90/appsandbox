#ifndef _APPSANDBOXVAD_WAVSTREAM_H_
#define _APPSANDBOXVAD_WAVSTREAM_H_

//=============================================================================
// Notification list entry for event-driven mode
//=============================================================================

typedef struct _NotificationListEntry
{
    LIST_ENTRY  ListEntry;
    PKEVENT     NotificationEvent;
} NotificationListEntry;

EXT_CALLBACK VadTimerNotifyRT;

//=============================================================================
// Forward declarations
//=============================================================================

class CVadWaveMiniport;
typedef CVadWaveMiniport *PCVadWaveMiniport;

//=============================================================================
// CVadWaveStream
//=============================================================================

class CVadWaveStream :
    public IDrmAudioStream,
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTOutputStream,
    public CUnknown
{
protected:
    PPORTWAVERTSTREAM           m_pPortStream;
    LIST_ENTRY                  m_NotificationList;
    PEX_TIMER                   m_pNotificationTimer;
    ULONG                       m_ulNotificationIntervalMs;
    ULONG                       m_ulCurrentWritePosition;
    LONG                        m_IsCurrentWritePositionUpdated;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CVadWaveStream);
    ~CVadWaveStream();

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTOutputStream;
    IMP_IDrmAudioStream;

    NTSTATUS Init
    (
        _In_  PCVadWaveMiniport Miniport,
        _In_  PPORTWAVERTSTREAM Stream,
        _In_  ULONG             Channel,
        _In_  BOOLEAN           Capture,
        _In_  PKSDATAFORMAT     DataFormat,
        _In_  GUID              SignalProcessingMode
    );

    friend class                CVadWaveMiniport;
    friend EXT_CALLBACK         VadTimerNotifyRT;

protected:
    CVadWaveMiniport*           m_pMiniport;
    ULONG                       m_ulPin;
    BOOLEAN                     m_bCapture;
    BOOLEAN                     m_bUnregisterStream;
    ULONG                       m_ulDmaBufferSize;
    BYTE*                       m_pDmaBuffer;
    ULONG                       m_ulNotificationsPerBuffer;
    KSSTATE                     m_KsState;
    PKTIMER                     m_pTimer;
    PRKDPC                      m_pDpc;
    ULONGLONG                   m_ullPlayPosition;
    ULONGLONG                   m_ullWritePosition;
    ULONGLONG                   m_ullLinearPosition;
    ULONGLONG                   m_ullPresentationPosition;
    ULONG                       m_ulLastOsReadPacket;
    ULONG                       m_ulLastOsWritePacket;
    LONGLONG                    m_llPacketCounter;
    ULONGLONG                   m_ullDmaTimeStamp;
    LARGE_INTEGER               m_ullPerformanceCounterFrequency;
    ULONGLONG                   m_hnsElapsedTimeCarryForward;
    ULONGLONG                   m_ullLastDPCTimeStamp;
    ULONGLONG                   m_hnsDPCTimeCarryForward;
    ULONG                       m_byteDisplacementCarryForward;
    ULONG                       m_ulDmaMovementRate;
    BOOL                        m_bLfxEnabled;
    PBOOL                       m_pbMuted;
    PLONG                       m_plVolumeLevel;
    PLONG                       m_plPeakMeter;
    PWAVEFORMATEXTENSIBLE       m_pWfExt;
    ULONG                       m_ulContentId;
    GUID                        m_SignalProcessingMode;
    BOOLEAN                     m_bEoSReceived;
    BOOLEAN                     m_bLastBufferRendered;
    KSPIN_LOCK                  m_PositionSpinLock;

public:
    NTSTATUS GetPresentationPosition
    (
        _Out_  KSAUDIO_PRESENTATION_POSITION *_pPresentationPosition
    );

    ULONG GetCurrentWaveRTWritePosition()
    {
        return m_ulCurrentWritePosition;
    };

    BOOL IsCurrentWaveRTWritePositionUpdated()
    {
        return InterlockedExchange(&m_IsCurrentWritePositionUpdated, 0) ? TRUE : FALSE;
    };

    GUID GetSignalProcessingMode()
    {
        return m_SignalProcessingMode;
    }

private:

#pragma code_seg()

    VOID UpdatePosition
    (
        _In_ LARGE_INTEGER ilQPC
    );

    NTSTATUS SetCurrentWritePositionInternal
    (
        _In_  ULONG ulCurrentWritePosition
    );

    NTSTATUS GetPositions
    (
        _Out_opt_  ULONGLONG *      _pullLinearBufferPosition,
        _Out_opt_  ULONGLONG *      _pullPresentationPosition,
        _Out_opt_  LARGE_INTEGER *  _pliQPCTime
    );

#pragma code_seg()
};

typedef CVadWaveStream *PCVadWaveStream;

#endif // _APPSANDBOXVAD_WAVSTREAM_H_
