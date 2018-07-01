/*!
    @file HookKernelCommon.hpp

    @brief Kernel mode code for uncategorized functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include "Common.hpp"

extern const PHYSICAL_MEMORY_DESCRIPTOR* g_PhysicalMemoryDescriptor;

SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
InitializeHook (
    VOID
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
CleanupHook (
    VOID
    );

SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
BOOLEAN
AreAllHooksInvisible (
    VOID
    );