#pragma warning (disable : 4127)

#include "driverdefs.h"

//=============================================================================
#pragma code_seg("PAGE")
PWAVEFORMATEX
GetWaveFormatEx
(
    _In_  PKSDATAFORMAT pDataFormat
)
{
    PAGED_CODE();

    PWAVEFORMATEX pWfx = NULL;

    if
    (
        pDataFormat &&
        ( IsEqualGUIDAligned(pDataFormat->MajorFormat,
                KSDATAFORMAT_TYPE_AUDIO)             &&
          ( IsEqualGUIDAligned(pDataFormat->Specifier,
                KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) ||
            IsEqualGUIDAligned(pDataFormat->Specifier,
                KSDATAFORMAT_SPECIFIER_DSOUND) ) )
    )
    {
        pWfx = PWAVEFORMATEX(pDataFormat + 1);

        if (IsEqualGUIDAligned(pDataFormat->Specifier,
                KSDATAFORMAT_SPECIFIER_DSOUND))
        {
            PKSDSOUND_BUFFERDESC pwfxds;

            pwfxds = PKSDSOUND_BUFFERDESC(pDataFormat + 1);
            pWfx = &pwfxds->WaveFormatEx;
        }
    }

    return pWfx;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
ValidatePropertyParams
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               cbValueSize,
    _In_ ULONG               cbInstanceSize /* = 0 */
)
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;

    if (PropertyRequest && cbValueSize)
    {
        if (0 == PropertyRequest->ValueSize)
        {
            PropertyRequest->ValueSize = cbValueSize;
            ntStatus = STATUS_BUFFER_OVERFLOW;
        }
        else if (PropertyRequest->ValueSize < cbValueSize)
        {
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
        else if (PropertyRequest->InstanceSize < cbInstanceSize)
        {
            ntStatus = STATUS_BUFFER_TOO_SMALL;
        }
        else if (PropertyRequest->ValueSize >= cbValueSize)
        {
            if (PropertyRequest->Value)
            {
                ntStatus = STATUS_SUCCESS;
            }
        }
    }
    else
    {
        ntStatus = STATUS_INVALID_PARAMETER;
    }

    if (PropertyRequest &&
        STATUS_SUCCESS != ntStatus &&
        STATUS_BUFFER_OVERFLOW != ntStatus)
    {
        PropertyRequest->ValueSize = 0;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_BasicSupport
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               Flags,
    _In_ DWORD               PropTypeSetId
)
{
    PAGED_CODE();

    ASSERT(Flags & KSPROPERTY_TYPE_BASICSUPPORT);

    NTSTATUS ntStatus = STATUS_INVALID_PARAMETER;

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION PropDesc =
            PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

        PropDesc->AccessFlags       = Flags;
        PropDesc->DescriptionSize   = sizeof(KSPROPERTY_DESCRIPTION);
        if (VT_ILLEGAL != PropTypeSetId)
        {
            PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
            PropDesc->PropTypeSet.Id    = PropTypeSetId;
        }
        else
        {
            PropDesc->PropTypeSet.Set   = GUID_NULL;
            PropDesc->PropTypeSet.Id    = 0;
        }
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount  = 0;
        PropDesc->Reserved          = 0;

        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        ntStatus = STATUS_SUCCESS;
    }
    else if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        *(PULONG(PropertyRequest->Value)) = Flags;

        PropertyRequest->ValueSize = sizeof(ULONG);
        ntStatus = STATUS_SUCCESS;
    }
    else
    {
        PropertyRequest->ValueSize = 0;
        ntStatus = STATUS_BUFFER_TOO_SMALL;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
static
NTSTATUS
PropertyHandler_BasicSupportVolume
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG    cbFullProperty =
        sizeof(KSPROPERTY_DESCRIPTION) +
        sizeof(KSPROPERTY_MEMBERSHEADER) +
        sizeof(KSPROPERTY_STEPPING_LONG) * MaxChannels;

    ASSERT(MaxChannels > 0);

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION PropDesc =
            PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

        PropDesc->AccessFlags       = KSPROPERTY_TYPE_ALL;
        PropDesc->DescriptionSize   = cbFullProperty;
        PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
        PropDesc->PropTypeSet.Id    = VT_I4;
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount  = 1;
        PropDesc->Reserved          = 0;

        if (PropertyRequest->ValueSize >= cbFullProperty)
        {
            PKSPROPERTY_MEMBERSHEADER Members =
                PKSPROPERTY_MEMBERSHEADER(PropDesc + 1);

            Members->MembersFlags   = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize    = sizeof(KSPROPERTY_STEPPING_LONG);
            Members->MembersCount   = MaxChannels;
            Members->Flags          = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;

            PKSPROPERTY_STEPPING_LONG Range =
                PKSPROPERTY_STEPPING_LONG(Members + 1);

            for (ULONG i = 0; i < MaxChannels; ++i)
            {
                Range[i].Bounds.SignedMaximum = VOLUME_SIGNED_MAXIMUM;
                Range[i].Bounds.SignedMinimum = VOLUME_SIGNED_MINIMUM;
                Range[i].SteppingDelta        = VOLUME_STEPPING_DELTA;
                Range[i].Reserved             = 0;
            }

            PropertyRequest->ValueSize = cbFullProperty;
        }
        else
        {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
    }
    else if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        PULONG AccessFlags = PULONG(PropertyRequest->Value);

        PropertyRequest->ValueSize = sizeof(ULONG);
        *AccessFlags = KSPROPERTY_TYPE_ALL;
    }
    else
    {
        PropertyRequest->ValueSize = 0;
        ntStatus = STATUS_BUFFER_TOO_SMALL;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
static
NTSTATUS
PropertyHandler_BasicSupportMute
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG    cbFullProperty =
        sizeof(KSPROPERTY_DESCRIPTION) +
        sizeof(KSPROPERTY_MEMBERSHEADER) +
        sizeof(KSPROPERTY_STEPPING_LONG) * MaxChannels;

    ASSERT(MaxChannels > 0);

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION PropDesc =
            PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

        PropDesc->AccessFlags       = KSPROPERTY_TYPE_ALL;
        PropDesc->DescriptionSize   = cbFullProperty;
        PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
        PropDesc->PropTypeSet.Id    = VT_BOOL;
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount  = 1;
        PropDesc->Reserved          = 0;

        if (PropertyRequest->ValueSize >= cbFullProperty)
        {
            PKSPROPERTY_MEMBERSHEADER Members =
                PKSPROPERTY_MEMBERSHEADER(PropDesc + 1);

            Members->MembersFlags   = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize    = sizeof(KSPROPERTY_STEPPING_LONG);
            Members->MembersCount   = MaxChannels;
            Members->Flags          = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;

            PKSPROPERTY_STEPPING_LONG Range =
                PKSPROPERTY_STEPPING_LONG(Members + 1);

            for (ULONG i = 0; i < MaxChannels; ++i)
            {
                Range[i].Bounds.SignedMaximum = 1;
                Range[i].Bounds.SignedMinimum = 0;
                Range[i].SteppingDelta        = 1;
                Range[i].Reserved             = 0;
            }

            PropertyRequest->ValueSize = cbFullProperty;
        }
        else
        {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
    }
    else if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        PULONG AccessFlags = PULONG(PropertyRequest->Value);

        PropertyRequest->ValueSize = sizeof(ULONG);
        *AccessFlags = KSPROPERTY_TYPE_ALL;
    }
    else
    {
        PropertyRequest->ValueSize = 0;
        ntStatus = STATUS_BUFFER_TOO_SMALL;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
static
NTSTATUS
PropertyHandler_BasicSupportPeakMeter2
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG    cbFullProperty =
        sizeof(KSPROPERTY_DESCRIPTION) +
        sizeof(KSPROPERTY_MEMBERSHEADER) +
        sizeof(KSPROPERTY_STEPPING_LONG) * MaxChannels;

    ASSERT(MaxChannels > 0);

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
    {
        PKSPROPERTY_DESCRIPTION PropDesc =
            PKSPROPERTY_DESCRIPTION(PropertyRequest->Value);

        PropDesc->AccessFlags       = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        PropDesc->DescriptionSize   = cbFullProperty;
        PropDesc->PropTypeSet.Set   = KSPROPTYPESETID_General;
        PropDesc->PropTypeSet.Id    = VT_I4;
        PropDesc->PropTypeSet.Flags = 0;
        PropDesc->MembersListCount  = 1;
        PropDesc->Reserved          = 0;

        if (PropertyRequest->ValueSize >= cbFullProperty)
        {
            PKSPROPERTY_MEMBERSHEADER Members =
                PKSPROPERTY_MEMBERSHEADER(PropDesc + 1);

            Members->MembersFlags   = KSPROPERTY_MEMBER_STEPPEDRANGES;
            Members->MembersSize    = sizeof(KSPROPERTY_STEPPING_LONG);
            Members->MembersCount   = MaxChannels;
            Members->Flags          = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;

            PKSPROPERTY_STEPPING_LONG Range =
                PKSPROPERTY_STEPPING_LONG(Members + 1);

            for (ULONG i = 0; i < MaxChannels; ++i)
            {
                Range[i].Bounds.SignedMaximum = PEAKMETER_SIGNED_MAXIMUM;
                Range[i].Bounds.SignedMinimum = PEAKMETER_SIGNED_MINIMUM;
                Range[i].SteppingDelta        = PEAKMETER_STEPPING_DELTA;
                Range[i].Reserved             = 0;
            }

            PropertyRequest->ValueSize = cbFullProperty;
        }
        else
        {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
    }
    else if (PropertyRequest->ValueSize >= sizeof(ULONG))
    {
        PULONG AccessFlags = PULONG(PropertyRequest->Value);

        PropertyRequest->ValueSize = sizeof(ULONG);
        *AccessFlags = KSPROPERTY_TYPE_ALL;
    }
    else
    {
        PropertyRequest->ValueSize = 0;
        ntStatus = STATUS_BUFFER_TOO_SMALL;
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_CpuResources
(
    _In_ PPCPROPERTY_REQUEST PropertyRequest
)
{
    PAGED_CODE();

    DPF_ENTER(("[PropertyHandler_CpuResources]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        ntStatus = ValidatePropertyParams(PropertyRequest, sizeof(ULONG));
        if (NT_SUCCESS(ntStatus))
        {
            *(PULONG(PropertyRequest->Value)) = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
            PropertyRequest->ValueSize = sizeof(ULONG);
        }
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        ntStatus =
            PropertyHandler_BasicSupport
            (
                PropertyRequest,
                KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
                VT_UI4
            );
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_Volume
(
    _In_ PADAPTERCOMMON      AdapterCommon,
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    DPF_ENTER(("[PropertyHandler_Volume]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    ulChannel;
    PLONG    plVolume;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        ntStatus = PropertyHandler_BasicSupportVolume(
                            PropertyRequest,
                            MaxChannels);
    }
    else
    {
        ntStatus =
            ValidatePropertyParams
            (
                PropertyRequest,
                sizeof(LONG),
                sizeof(ULONG)
            );
        if (NT_SUCCESS(ntStatus))
        {
            ulChannel = *(PULONG(PropertyRequest->Instance));
            plVolume  = PLONG(PropertyRequest->Value);

            if (ulChannel >= MaxChannels &&
                ulChannel != ALL_CHANNELS_ID)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
            }
            else if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
            {
                *plVolume =
                    AdapterCommon->MixerVolumeRead
                    (
                        PropertyRequest->Node,
                        ulChannel == ALL_CHANNELS_ID ? 0 : ulChannel
                    );
                PropertyRequest->ValueSize = sizeof(ULONG);
            }
            else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
            {
                if (ALL_CHANNELS_ID == ulChannel)
                {
                    for (ULONG i = 0; i < MaxChannels; ++i)
                    {
                        AdapterCommon->MixerVolumeWrite
                        (
                            PropertyRequest->Node,
                            i,
                            VOLUME_NORMALIZE_IN_RANGE(*plVolume)
                        );
                    }
                }
                else
                {
                    AdapterCommon->MixerVolumeWrite
                    (
                        PropertyRequest->Node,
                        ulChannel,
                        VOLUME_NORMALIZE_IN_RANGE(*plVolume)
                    );
                }
            }
        }

        if (!NT_SUCCESS(ntStatus))
        {
            DPF(D_TERSE, ("[PropertyHandler_Volume - ntStatus=0x%08x]", ntStatus));
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_Mute
(
    _In_ PADAPTERCOMMON      AdapterCommon,
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    DPF_ENTER(("[PropertyHandler_Mute]"));

    NTSTATUS ntStatus;
    ULONG    ulChannel;
    PBOOL    pfMute;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        ntStatus = PropertyHandler_BasicSupportMute(
                            PropertyRequest,
                            MaxChannels);
    }
    else
    {
        ntStatus =
            ValidatePropertyParams
            (
                PropertyRequest,
                sizeof(BOOL),
                sizeof(ULONG)
            );
        if (NT_SUCCESS(ntStatus))
        {
            ulChannel = *(PULONG(PropertyRequest->Instance));
            pfMute    = PBOOL(PropertyRequest->Value);

            if (ulChannel >= MaxChannels &&
                ulChannel != ALL_CHANNELS_ID)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
            }
            else if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
            {
                *pfMute =
                    AdapterCommon->MixerMuteRead
                    (
                        PropertyRequest->Node,
                        ulChannel == ALL_CHANNELS_ID ? 0 : ulChannel
                    );
                PropertyRequest->ValueSize = sizeof(BOOL);
            }
            else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
            {
                if (ALL_CHANNELS_ID == ulChannel)
                {
                    for (ULONG i = 0; i < MaxChannels; ++i)
                    {
                        AdapterCommon->MixerMuteWrite
                        (
                            PropertyRequest->Node,
                            i,
                            (*pfMute) ? TRUE : FALSE
                        );
                    }
                }
                else
                {
                    AdapterCommon->MixerMuteWrite
                    (
                        PropertyRequest->Node,
                        ulChannel,
                        (*pfMute) ? TRUE : FALSE
                    );
                }
            }
        }

        if (!NT_SUCCESS(ntStatus))
        {
            DPF(D_TERSE, ("[PropertyHandler_Mute - ntStatus=0x%08x]", ntStatus));
        }
    }

    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_PeakMeter2
(
    _In_ PADAPTERCOMMON      AdapterCommon,
    _In_ PPCPROPERTY_REQUEST PropertyRequest,
    _In_ ULONG               MaxChannels
)
{
    PAGED_CODE();

    DPF_ENTER(("[PropertyHandler_PeakMeter2]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    ulChannel;
    PLONG    plSample;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        ntStatus = PropertyHandler_BasicSupportPeakMeter2(
                            PropertyRequest,
                            MaxChannels);
    }
    else
    {
        ntStatus =
            ValidatePropertyParams
            (
                PropertyRequest,
                sizeof(LONG),
                sizeof(ULONG)
            );
        if (NT_SUCCESS(ntStatus))
        {
            ulChannel = *(PULONG(PropertyRequest->Instance));
            plSample  = PLONG(PropertyRequest->Value);

            if (ulChannel >= MaxChannels &&
                ulChannel != ALL_CHANNELS_ID)
            {
                ntStatus = STATUS_INVALID_PARAMETER;
            }
            else if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
            {
                *plSample =
                    PEAKMETER_NORMALIZE_IN_RANGE(
                        AdapterCommon->MixerPeakMeterRead
                        (
                            PropertyRequest->Node,
                            ulChannel == ALL_CHANNELS_ID ? 0 : ulChannel
                        ));

                PropertyRequest->ValueSize = sizeof(ULONG);
            }
        }

        if (!NT_SUCCESS(ntStatus))
        {
            DPF(D_TERSE, ("[PropertyHandler_PeakMeter2 - ntStatus=0x%08x]", ntStatus));
        }
    }

    return ntStatus;
}

#pragma code_seg()
