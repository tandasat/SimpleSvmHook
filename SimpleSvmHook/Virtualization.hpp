/*!
    @file Virtualization.hpp

    @brief Kernel code to virtualize and de-virtualize processors.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include "Common.hpp"

SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(APC_LEVEL)
_Check_return_
NTSTATUS
VirtualizeAllProcessors (
    VOID
    );

SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(APC_LEVEL)
VOID
DevirtualizeAllProcessors (
    VOID
    );
