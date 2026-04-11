#pragma once

#ifdef _NEW_DELETE_OPERATORS_

PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags,
    ULONG       tag
);

PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags
);

void __cdecl operator delete
(
    PVOID pVoid,
    ULONG tag
);

void __cdecl operator delete
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid,
    _In_ size_t cbSize
);

void __cdecl operator delete
(
    PVOID pVoid
);

void __cdecl operator delete[]
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid,
    _In_ size_t cbSize
);

void __cdecl operator delete[]
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid
);

#endif // _NEW_DELETE_OPERATORS_
