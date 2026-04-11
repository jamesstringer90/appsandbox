#ifndef _APPSANDBOXVAD_DRIVERDEFS_H_
#define _APPSANDBOXVAD_DRIVERDEFS_H_

#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <ntintsafe.h>
#include <wdf.h>
#include <wdfminiport.h>
#include <Ntstrsafe.h>
#include "poolalloc.h"

//=============================================================================
// Pool tags
//=============================================================================

#define VAD_ADAPTER_POOLTAG     'dAaS'
#define VAD_WAVRT_POOLTAG       'dWaS'
#define VAD_TOPO_POOLTAG        'dTaS'

//=============================================================================
// Debug
//=============================================================================

#define STR_MODULENAME          "APPSANDBOXVAD: "

#define D_FUNC                  4
#define D_BLAB                  DEBUGLVL_BLAB
#define D_VERBOSE               DEBUGLVL_VERBOSE
#define D_TERSE                 DEBUGLVL_TERSE
#define D_ERROR                 DEBUGLVL_ERROR
#define DPF                     _DbgPrintF
#define DPF_ENTER(x)            DPF(D_FUNC, x)

//=============================================================================
// Constants
//=============================================================================

// Channel indices
#define CHAN_LEFT                0
#define CHAN_RIGHT               1
#define CHAN_MASTER              (-1)

// DMA buffer size
#define DMA_BUFFER_SIZE         0x16000

// 100-nanosecond units per millisecond
#define _100NS_PER_MILLISECOND  10000
#define HNSTIME_PER_MILLISECOND 10000

// Volume range
#define VOLUME_STEPPING_DELTA   0x8000
#define VOLUME_SIGNED_MAXIMUM   0x00000000
#define VOLUME_SIGNED_MINIMUM   (-96 * 0x10000)

// Peak meter range
#define PEAKMETER_STEPPING_DELTA    0x1000
#define PEAKMETER_SIGNED_MAXIMUM    LONG_MAX
#define PEAKMETER_SIGNED_MINIMUM    LONG_MIN

// Property type shorthand
#define KSPROPERTY_TYPE_ALL     KSPROPERTY_TYPE_BASICSUPPORT | \
                                KSPROPERTY_TYPE_GET | \
                                KSPROPERTY_TYPE_SET

// Device-specific node numbers
#define DEV_SPECIFIC_VT_BOOL    9
#define DEV_SPECIFIC_VT_I4      10
#define DEV_SPECIFIC_VT_UI4     11

// Value normalization macros
#define VALUE_NORMALIZE_P(v, step) \
    ((((v) + (step)/2) / (step)) * (step))

#define VALUE_NORMALIZE(v, step) \
    ((v) > 0 ? VALUE_NORMALIZE_P((v), (step)) : -(VALUE_NORMALIZE_P(-(v), (step))))

#define VALUE_NORMALIZE_IN_RANGE_EX(v, min, max, step) \
    ((v) > (max) ? (max) : \
     (v) < (min) ? (min) : \
     VALUE_NORMALIZE((v), (step)))

#define VOLUME_NORMALIZE_IN_RANGE(v) \
    VALUE_NORMALIZE_IN_RANGE_EX((v), VOLUME_SIGNED_MINIMUM, VOLUME_SIGNED_MAXIMUM, VOLUME_STEPPING_DELTA)

#define PEAKMETER_NORMALIZE_IN_RANGE(v) \
    VALUE_NORMALIZE_IN_RANGE_EX((v), PEAKMETER_SIGNED_MINIMUM, PEAKMETER_SIGNED_MAXIMUM, PEAKMETER_STEPPING_DELTA)

// Pin instance counting
#define ALL_CHANNELS_ID             UINT32_MAX

#define VERIFY_PIN_INSTANCE_RESOURCES_AVAILABLE(status, allocated, max) \
    status = (allocated < max) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES

#define ALLOCATE_PIN_INSTANCE_RESOURCES(allocated) \
    allocated++

#define FREE_PIN_INSTANCE_RESOURCES(allocated) \
    allocated--

//=============================================================================
// Product GUID
// {7C8A91E0-4D3B-4F5A-8E6D-2B1C0A9F8E7D}
//=============================================================================
#define STATIC_PID_APPSANDBOXVAD \
    0x7c8a91e0, 0x4d3b, 0x4f5a, 0x8e, 0x6d, 0x2b, 0x1c, 0x0a, 0x9f, 0x8e, 0x7d
DEFINE_GUIDSTRUCT("7C8A91E0-4D3B-4F5A-8E6D-2B1C0A9F8E7D", PID_APPSANDBOXVAD);
#define PID_APPSANDBOXVAD DEFINE_GUIDNAMED(PID_APPSANDBOXVAD)

//=============================================================================
// Global settings
//=============================================================================
extern DWORD g_DoNotCreateDataFiles;
extern UNICODE_STRING g_RegistryPath;

//=============================================================================
// Forward declarations for property handlers
//=============================================================================
NTSTATUS VadTopoPropertyHandler
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

NTSTATUS VadWaveFilterHandler
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

NTSTATUS VadGenericPinHandler
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
);

// pin/node enums and signal processing mode attributes
#include "pinnodes.h"

// adaptercommon.h uses definitions above
#include "adaptercommon.h"

// topology property handler declarations (needed by topofilter.h)
#include "topology.h"

// helper functions for property handlers
#include "kshelper.h"

#endif // _APPSANDBOXVAD_DRIVERDEFS_H_
