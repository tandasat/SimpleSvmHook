/*!
    @file HookKernelRegistration.hpp

    @brief Kernel mode code to initialize hooks.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include "Common.hpp"

SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
InitializeHookRegistrationEntries (
    VOID
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
CleanupHookRegistrationEntries (
    VOID
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
ReportHookActivities (
    VOID
    );