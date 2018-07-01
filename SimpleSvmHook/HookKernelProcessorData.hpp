/*!
    @file HookKernelProcessorData.hpp

    @brief Kernel mode code to initialize and cleanup hook data.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include "Common.hpp"
#include "HookCommon.hpp"

_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
InitializeHookData (
    _Outptr_result_nullonfailure_ PHOOK_DATA* HookData
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
CleanupHookData (
    _Inout_ PHOOK_DATA HookData
    );

_Check_return_
PHYSICAL_ADDRESS
GetPml4PhysicalAddress (
    _In_ const HOOK_DATA* HookData
    );
