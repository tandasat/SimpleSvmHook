/*!
    @file HookKernelCommon.cpp

    @brief Kernel mode code for uncategorized functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookKernelCommon.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"
#include "PhysicalMemoryDescriptor.hpp"
#include "HookKernelRegistration.hpp"

//
// Read only physical memory address ranges. Used to build NPT entries.
//
const PHYSICAL_MEMORY_DESCRIPTOR* g_PhysicalMemoryDescriptor;

/*!
    @brief Initializes hook related general data structures.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
NTSTATUS
InitializeHook (
    VOID
    )
{
    NTSTATUS status;
    BOOLEAN registrationEntriesInited;
    PPHYSICAL_MEMORY_DESCRIPTOR descriptor;

    PAGED_CODE();

    registrationEntriesInited = FALSE;

    //
    // Installs hooks without activating them. Activation is done right after
    // a processor is virtualized.
    //
    status = InitializeHookRegistrationEntries();
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializeHookRegistrationEntries failed : %08x",
                          status);
        goto Exit;
    }
    registrationEntriesInited = TRUE;

    //
    // Get physical memory address ranges.
    //
    descriptor = DuplicatePhysicalMemoryDescriptor();
    if (descriptor == nullptr)
    {
        LOGGING_LOG_ERROR("DuplicatePhysicalMemoryDescriptor failed");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    DumpPhysicalMemoryRanges(descriptor);

    status = STATUS_SUCCESS;
    g_PhysicalMemoryDescriptor = descriptor;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (registrationEntriesInited != FALSE)
        {
            CleanupHookRegistrationEntries();
        }
    }
    return status;
}

/*!
    @brief Frees hook related general data structures.
 */
_Use_decl_annotations_
VOID
CleanupHook (
    VOID
    )
{
    FreePhysicalMemoryDescriptor(
        const_cast<PPHYSICAL_MEMORY_DESCRIPTOR>(g_PhysicalMemoryDescriptor));

    CleanupHookRegistrationEntries();
    ReportHookActivities();
}

/*!
    @brief Tests whether all hooks are invisible.

    @return TRUE if all hooks are invisible; otherwise FALSE.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
BOOLEAN
AreAllHooksInvisible (
    VOID
    )
{
    BOOLEAN invisible;

    PAGED_CODE();

    invisible = TRUE;
    for (const auto& registration : g_HookRegistrationEntries)
    {
        NT_ASSERT(registration.HookEntry.HookAddress != nullptr);

        if (*reinterpret_cast<PCUCHAR>(registration.HookEntry.HookAddress) == 0xcc)
        {
            LOGGING_LOG_WARN("Hook at %p for %wZ is visible",
                             registration.HookEntry.HookAddress,
                             &registration.FunctionName);
            invisible = FALSE;
        }
    }

    return invisible;
}