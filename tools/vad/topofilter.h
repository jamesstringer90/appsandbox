#ifndef _APPSANDBOXVAD_TOPOFILTER_H_
#define _APPSANDBOXVAD_TOPOFILTER_H_

//=============================================================================
// Topology pin data ranges (bridge)
//=============================================================================

static
KSDATARANGE SpeakerTopoPinDataRangesBridge[] =
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
PKSDATARANGE SpeakerTopoPinDataRangePointersBridge[] =
{
    &SpeakerTopoPinDataRangesBridge[0]
};

//=============================================================================
// Topology pins
//=============================================================================

static
PCPIN_DESCRIPTOR SpeakerTopoMiniportPins[] =
{
    // KSPIN_TOPO_WAVEOUT_SOURCE
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
            SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),
            SpeakerTopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },
    // KSPIN_TOPO_LINEOUT_DEST
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
            SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),
            SpeakerTopoPinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER,
            NULL,
            0
        }
    }
};

//=============================================================================
// Jack description (always connected, integrated)
//=============================================================================

static
KSJACK_DESCRIPTION SpeakerJackDesc =
{
    KSAUDIO_SPEAKER_STEREO,
    JACKDESC_RGB(0x4A, 0x8C, 0xD6),    // blue-ish
    eConnTypeUnknown,
    eGeoLocFront,
    eGenLocPrimaryBox,
    ePortConnIntegratedDevice,
    TRUE                                // IsConnected = always
};

static
PKSJACK_DESCRIPTION SpeakerJackDescriptions[] =
{
    NULL,
    &SpeakerJackDesc
};

//=============================================================================
// Volume property
//=============================================================================

static
PCPROPERTY_ITEM SpeakerPropertiesVolume[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadSpeakerTopoHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerVolume, SpeakerPropertiesVolume);

//=============================================================================
// Mute property
//=============================================================================

static
PCPROPERTY_ITEM SpeakerPropertiesMute[] =
{
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadSpeakerTopoHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerMute, SpeakerPropertiesMute);

//=============================================================================
// Topology nodes: Volume → Mute
//=============================================================================

static
PCNODE_DESCRIPTOR SpeakerTopologyNodes[] =
{
    // KSNODE_TOPO_VOLUME
    {
        0,
        &AutomationSpeakerVolume,
        &KSNODETYPE_VOLUME,
        &KSAUDFNAME_MASTER_VOLUME
    },
    // KSNODE_TOPO_MUTE
    {
        0,
        &AutomationSpeakerMute,
        &KSNODETYPE_MUTE,
        &KSAUDFNAME_MASTER_MUTE
    }
};

C_ASSERT(KSNODE_TOPO_VOLUME == 0);
C_ASSERT(KSNODE_TOPO_MUTE == 1);

//=============================================================================
// Topology connections: Pin0 → Volume → Mute → Pin1
//=============================================================================

static
PCCONNECTION_DESCRIPTOR SpeakerTopoConnections[] =
{
    //  FromNode,              FromPin,                    ToNode,              ToPin
    {   PCFILTER_NODE,         KSPIN_TOPO_WAVEOUT_SOURCE,  KSNODE_TOPO_VOLUME,  1 },
    {   KSNODE_TOPO_VOLUME,    0,                          KSNODE_TOPO_MUTE,    1 },
    {   KSNODE_TOPO_MUTE,      0,                          PCFILTER_NODE,       KSPIN_TOPO_LINEOUT_DEST }
};

//=============================================================================
// Topology filter properties (jack description)
//=============================================================================

static
PCPROPERTY_ITEM SpeakerTopoFilterProperties[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadSpeakerFilterHandler
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        VadSpeakerFilterHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerTopoFilter, SpeakerTopoFilterProperties);

//=============================================================================
// Topology filter descriptor
//=============================================================================

static
PCFILTER_DESCRIPTOR SpeakerTopoFilterDescriptor =
{
    0,                                              // Version
    &AutomationSpeakerTopoFilter,                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    SIZEOF_ARRAY(SpeakerTopoMiniportPins),          // PinCount
    SpeakerTopoMiniportPins,                        // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    SIZEOF_ARRAY(SpeakerTopologyNodes),             // NodeCount
    SpeakerTopologyNodes,                           // Nodes
    SIZEOF_ARRAY(SpeakerTopoConnections),           // ConnectionCount
    SpeakerTopoConnections,                         // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};

#endif // _APPSANDBOXVAD_TOPOFILTER_H_
