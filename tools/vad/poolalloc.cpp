#pragma warning (disable : 4127)

#include "driverdefs.h"

#ifdef _NEW_DELETE_OPERATORS_

#pragma code_seg()
PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags,
    ULONG       tag
)
{
    return ExAllocatePool2(poolFlags, iSize, tag);
}

#pragma code_seg()
PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags
)
{
    return ExAllocatePool2(poolFlags, iSize, VAD_ADAPTER_POOLTAG);
}

#pragma code_seg()
void __cdecl operator delete
(
    PVOID pVoid,
    ULONG tag
)
{
    if (pVoid)
    {
        ExFreePoolWithTag(pVoid, tag);
    }
}

#pragma code_seg()
void __cdecl operator delete
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid,
    _In_ size_t cbSize
)
{
    UNREFERENCED_PARAMETER(cbSize);
    if (pVoid)
    {
        ExFreePool(pVoid);
    }
}

#pragma code_seg()
void __cdecl operator delete[]
(
    PVOID pVoid
)
{
    if (pVoid)
    {
        ExFreePool(pVoid);
    }
}

#endif // _NEW_DELETE_OPERATORS_
