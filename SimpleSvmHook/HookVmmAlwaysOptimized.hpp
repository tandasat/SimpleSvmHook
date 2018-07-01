/*!
    @file HookVmmAlwaysOptimized.hpp

    @brief VMM code that is always compiled with optimization due to slowness.

    @author Satoshi Tanda

    @copyright  Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <fltKernel.h>
#include "x86_64.hpp"

VOID
ChangePermissionOfPage (
    _Inout_ PPML4_ENTRY_4KB Pml4Table,
    _In_ ULONG64 PhysicalAddress,
    _In_ BOOLEAN DisallowExecution
    );

VOID
ChangePermissionsOfAllPages (
    _Inout_ PPML4_ENTRY_4KB Pml4Table,
    _When_(DisallowExecution == FALSE, _In_)
    _When_(DisallowExecution != FALSE, _Unreferenced_parameter_)
        ULONG64 ActiveHookPa,
    _In_ BOOLEAN DisallowExecution,
    _In_ ULONG MaxPpeIndex
    );
