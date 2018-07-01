/*!
    @file PhysicalMemoryDescriptor.hpp

    @brief Kernel code to get information about system's physical memory ranges.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include "Common.hpp"

SIMPLESVMHOOK_PAGED
__drv_allocatesMem(Mem)
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
_Success_(return != nullptr)
PPHYSICAL_MEMORY_DESCRIPTOR
DuplicatePhysicalMemoryDescriptor (
    VOID
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
FreePhysicalMemoryDescriptor (
    _Frees_ptr_ PPHYSICAL_MEMORY_DESCRIPTOR Descriptor
    );

VOID
DumpPhysicalMemoryRanges (
    _In_ const PHYSICAL_MEMORY_DESCRIPTOR* Descriptor
    );
