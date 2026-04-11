#ifndef _APPSANDBOXVAD_WAVEFILTER_H_
#define _APPSANDBOXVAD_WAVEFILTER_H_

//=============================================================================
// Speaker render configuration
//=============================================================================

#define SPEAKER_DEVICE_MAX_CHANNELS     2
#define SPEAKER_HOST_MAX_CHANNELS       2
#define SPEAKER_HOST_MIN_BITS_PER_SAMPLE    16
#define SPEAKER_HOST_MAX_BITS_PER_SAMPLE    24
#define SPEAKER_HOST_MIN_SAMPLE_RATE        44100
#define SPEAKER_HOST_MAX_SAMPLE_RATE        48000
#define SPEAKER_MAX_INPUT_SYSTEM_STREAMS    1

//=============================================================================
// Supported device formats (4 entries: 16/24-bit at 44.1k/48k)
//=============================================================================

static
KSDATAFORMAT_WAVEFORMATEXTENSIBLE SpeakerPinSupportedFormats[] =
{
    // 0) 16-bit, Stereo, 44100 Hz
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                44100,
                44100 * 2 * 16 / 8,
                2 * 16 / 8,
                16,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            16,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },

    // 1) 16-bit, Stereo, 48000 Hz
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                48000 * 2 * 16 / 8,
                2 * 16 / 8,
                16,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            16,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },

    // 2) 24-bit, Stereo, 44100 Hz
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                44100,
                44100 * 2 * 24 / 8,
                2 * 24 / 8,
                24,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            24,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    },

    // 3) 24-bit, Stereo, 48000 Hz
    {
        {
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
            0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        {
            {
                WAVE_FORMAT_EXTENSIBLE,
                2,
                48000,
                48000 * 2 * 24 / 8,
                2 * 24 / 8,
                24,
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
            },
            24,
            KSAUDIO_SPEAKER_STEREO,
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
        }
    }
};

//=============================================================================
// Supported modes (each format with default signal processing)
//=============================================================================

static
MODE_AND_DEFAULT_FORMAT SpeakerPinSupportedModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerPinSupportedFormats[0].DataFormat
    },
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerPinSupportedFormats[1].DataFormat
    },
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerPinSupportedFormats[2].DataFormat
    },
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &SpeakerPinSupportedFormats[3].DataFormat
    }
};

//=============================================================================
// Pin device formats and modes array (indexed by pin ID)
//=============================================================================

static
PIN_DEVICE_FORMATS_AND_MODES SpeakerPinDeviceFormatsAndModes[] =
{
    {
        SystemRenderPin,
        SpeakerPinSupportedFormats,
        SIZEOF_ARRAY(SpeakerPinSupportedFormats),
        SpeakerPinSupportedModes,
        SIZEOF_ARRAY(SpeakerPinSupportedModes)
    },
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    }
};

//=============================================================================
// Wave data range (used by the OS to match format requests)
//=============================================================================

static
KSDATARANGE_AUDIO SpeakerWaveDataRangesStream[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2,          // MaximumChannels
        16,         // MinimumBitsPerSample
        24,         // MaximumBitsPerSample
        44100,      // MinimumSampleFrequency
        48000       // MaximumSampleFrequency
    }
};

static
PKSDATARANGE SpeakerWaveDataRangePointersStream[] =
{
    PKSDATARANGE(&SpeakerWaveDataRangesStream[0]),
    PKSDATARANGE(&VadDataRangeAttributeList),
};

//=============================================================================
// Bridge pin data ranges
//=============================================================================

static
KSDATARANGE SpeakerWaveDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE SpeakerWaveDataRangePointersBridge[] =
{
    &SpeakerWaveDataRangesBridge[0]
};

//=============================================================================
// Wave miniport pin descriptors
//=============================================================================

static
PCPIN_DESCRIPTOR SpeakerWaveMiniportPins[] =
{
    // KSPIN_WAVE_RENDER_SINK_SYSTEM
    {
        SPEAKER_MAX_INPUT_SYSTEM_STREAMS,
        SPEAKER_MAX_INPUT_SYSTEM_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(SpeakerWaveDataRangePointersStream),
            SpeakerWaveDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // KSPIN_WAVE_RENDER_SOURCE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(SpeakerWaveDataRangePointersBridge),
            SpeakerWaveDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
};

//=============================================================================
// Wave connections: system pin → bridge pin
//=============================================================================

static
PCCONNECTION_DESCRIPTOR SpeakerWaveConnections[] =
{
    { PCFILTER_NODE, KSPIN_WAVE_RENDER_SINK_SYSTEM, PCFILTER_NODE, KSPIN_WAVE_RENDER_SOURCE }
};

//=============================================================================
// Wave filter properties
//=============================================================================

static
PCPROPERTY_ITEM SpeakerWaveFilterProperties[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadWaveFilterHandler
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadWaveFilterHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerWaveFilter, SpeakerWaveFilterProperties);

//=============================================================================
// Wave filter descriptor
//=============================================================================

static
PCFILTER_DESCRIPTOR SpeakerWaveFilterDescriptor =
{
    0,                                              // Version
    &AutomationSpeakerWaveFilter,                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(SpeakerWaveMiniportPins),          // PinCount
    SpeakerWaveMiniportPins,                        // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    SIZEOF_ARRAY(SpeakerWaveConnections),           // ConnectionCount
    SpeakerWaveConnections,                         // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};

#endif // _APPSANDBOXVAD_WAVEFILTER_H_
