#ifndef _APPSANDBOXVAD_RENDERENDPOINTS_H_
#define _APPSANDBOXVAD_RENDERENDPOINTS_H_

#include "topology.h"
#include "topofilter.h"
#include "wavefilter.h"

//=============================================================================
// Factory function declarations
//=============================================================================

NTSTATUS
CreateVadWaveMiniport
(
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

NTSTATUS
CreateVadTopologyMiniport
(
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

//=============================================================================
// Speaker physical connections (wave bridge → topology bridge)
//=============================================================================

/*********************************************************************
* Topology/Wave bridge connection for speaker (internal)             *
*                                                                    *
*              +------+                +------+                      *
*              | Wave |                | Topo |                      *
*              |      |                |      |                      *
* System   --->|0    1|--------------->|0    1|---> Line Out         *
*              |      |                |      |                      *
*              +------+                +------+                      *
*********************************************************************/

static
PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,
        KSPIN_WAVE_RENDER_SOURCE,
        CONNECTIONTYPE_WAVE_OUTPUT
    }
};

//=============================================================================
// Speaker endpoint miniport pair
//=============================================================================

static
ENDPOINT_MINIPAIR SpeakerEndpoint =
{
    eSpeakerDevice,
    L"TopologySpeaker",                                     // must match KSNAME_TopologySpeaker in INF
    CreateVadTopologyMiniport,
    &SpeakerTopoFilterDescriptor,
    0, NULL,                                                // interface properties
    L"WaveSpeaker",                                         // must match KSNAME_WaveSpeaker in INF
    CreateVadWaveMiniport,
    &SpeakerWaveFilterDescriptor,
    0,                                                      // interface properties
    NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

//=============================================================================
// Render endpoint array
//=============================================================================

static
PENDPOINT_MINIPAIR  g_RenderEndpoints[] =
{
    &SpeakerEndpoint,
};

#define g_cRenderEndpoints  (SIZEOF_ARRAY(g_RenderEndpoints))

//=============================================================================
// Total miniports = endpoints * 2 (topology + wave)
//=============================================================================

#define g_MaxMiniports  (g_cRenderEndpoints * 2)

#endif // _APPSANDBOXVAD_RENDERENDPOINTS_H_
