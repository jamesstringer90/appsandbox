#ifndef _APPSANDBOXVAD_PINNODES_H_
#define _APPSANDBOXVAD_PINNODES_H_

// Name GUID for this driver
// {3E5F7A2C-1B8D-4C9E-A0F6-5D4E3C2B1A09}
#define STATIC_NAME_APPSANDBOXVAD \
    0x3e5f7a2c, 0x1b8d, 0x4c9e, 0xa0, 0xf6, 0x5d, 0x4e, 0x3c, 0x2b, 0x1a, 0x09
DEFINE_GUIDSTRUCT("3E5F7A2C-1B8D-4C9E-A0F6-5D4E3C2B1A09", NAME_APPSANDBOXVAD);
#define NAME_APPSANDBOXVAD DEFINE_GUIDNAMED(NAME_APPSANDBOXVAD)

//=============================================================================
// Render pin layout (render3 pattern: system sink + bridge source)
//=============================================================================

// Default pin instances
#define MAX_INPUT_SYSTEM_STREAMS    1

// Wave pins
enum
{
    KSPIN_WAVE_RENDER_SINK_SYSTEM = 0,
    KSPIN_WAVE_RENDER_SOURCE
};

// Wave topology nodes (unused in render3 — direct passthrough)
enum
{
    KSNODE_WAVE_DAC = 0
};

//=============================================================================
// Topology pins
//=============================================================================

enum
{
    KSPIN_TOPO_WAVEOUT_SOURCE = 0,
    KSPIN_TOPO_LINEOUT_DEST,
};

// Topology nodes
enum
{
    KSNODE_TOPO_VOLUME = 0,
    KSNODE_TOPO_MUTE
};

//=============================================================================
// Signal processing mode attribute (for data range attributes)
//=============================================================================

static
KSATTRIBUTE VadSignalProcessingModeAttribute =
{
    sizeof(KSATTRIBUTE),
    0,
    STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE),
};

static
PKSATTRIBUTE VadDataRangeAttributes[] =
{
    &VadSignalProcessingModeAttribute,
};

static
KSATTRIBUTE_LIST VadDataRangeAttributeList =
{
    ARRAYSIZE(VadDataRangeAttributes),
    VadDataRangeAttributes,
};

#endif // _APPSANDBOXVAD_PINNODES_H_
