/*!
    @file HookVmmCommon.cpp

    @brief VMM code to support hooking.

    @details Hooks are implemented by switching permissions and backing physical
        pages of pages and implemented as a state machine. States, page types,
        permission of them, as well as backing physical pages types are briefly
        summarized as below.
        ----
            State                     : Page Type
                                      : Current : Hooked : Other
            0)NptDefault              : RWX(O)  : RWX(O) : RWX(O)
            1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
            2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)

                Current= The page the processor is currently executing on.
                Hooked = The pages hooks are installed into and not being
                         executed by the processor.
                Other  = The rest of pages.

                (O)= The page is backed by the original physical page where no
                     hook exists.
                (E)= The page is backed by the exec physical page where hooks
                     exist.
        ----

        This also notes when those states change.
        ----
            Transition:
            0 -> 1 on enabling hooks (via CPUID)

            1 -> 1 on any read or write access (no #VMEXIT)
              -> 2 on execution access against any of hooked pages
              -> 0 on disabling hooks (via CPUID)

            2 -> 2 on any read or write access (no #VMEXIT)
              -> 2 on execution access against another hooked page
              -> 1 on execution access against any of non hooked pages
              -> 0 on disabling hooks (via CPUID)
        ---

    @author Satoshi Tanda

    @copyright  Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookVmmCommon.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"
#include "HookVmmAlwaysOptimized.hpp"

/*!
    @brief Finds HOOK_ENTRY associated with the physical memory page.

    @param[in] PhysicalAddress - The physical memory page to search HOOK_ENTRY.

    @return The address to the associated HOOK_ENTRY on success, or NULL.
 */
static
_Check_return_
const HOOK_ENTRY*
FindHookEntryByPhysicalPage (
    _In_ ULONG64 PhysicalAddress
    )
{
    for (const auto& registration : g_HookRegistrationEntries)
    {
        if (PAGE_ALIGN(PhysicalAddress) ==
            PAGE_ALIGN(registration.HookEntry.PhyPageBase))
        {
            return &registration.HookEntry;
        }
    }
    return nullptr;
}

/*!
    @brief Finds HOOK_ENTRY associated with the virtual address.

    @param[in] VirtualAddress - The virtual address to search HOOK_ENTRY.

    @return The address to the associated HOOK_ENTRY on success, or NULL.
 */
static
_Check_return_
const HOOK_ENTRY*
FindHookEntryByAddress (
    _In_ PVOID VirtualAddress
    )
{
    for (const auto& registration : g_HookRegistrationEntries)
    {
        if (VirtualAddress == registration.HookEntry.HookAddress)
        {
            return &registration.HookEntry;
        }
    }
    return nullptr;
}

/*!
    @brief Transition the NPT state 1 to 2.

    @details Hooks found on this page, which means the processor is attempt to
        execute a page where hooks are installed. Move to the state 3.

    @param[in,out] HookData - The processor associated hook data.

    @param[in] CurrentHookEntry - Information associated with the page contains
        hook(s) and  the processor has been executing on.
 */
static
VOID
TransitionNptState1To2 (
    _Inout_ PHOOK_DATA HookData,
    _In_ const HOOK_ENTRY* CurrentHookEntry
    )
{
    PPT_ENTRY_4KB nptEntry;

    NT_ASSERT(HookData->NptState == NptHookEnabledInvisible);
    NT_ASSERT(HookData->ActiveHookEntry == nullptr);

    PERFORMANCE_MEASURE_THIS_SCOPE();

    //
    // Make all pages non-executable.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
    //  v
    //  *)Transitioning           : RW-(O)  : RW-(O) : RW-(O)    << transitioning to here
    //  v
    //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
    //
    ChangePermissionsOfAllPages(HookData->Pml4Table,
                                0,
                                TRUE,
                                HookData->MaxNptPdpEntriesUsed);

    //
    // Switch the current page to the executable, exec page backed page.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
    //  v
    //  *)Transitioning           : RW-(O)  : RW-(O) : RW-(O)
    //  v
    //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)    << transitioning to here

    //
    // Get a NPT entry associated with the page the processor has been
    // executing on. The page should be backed by the original at this point.
    //
    nptEntry = GetNestedPageTableEntry(HookData->Pml4Table,
                                       CurrentHookEntry->PhyPageBase);
    NT_ASSERT(nptEntry != nullptr);
    NT_ASSERT(nptEntry->Fields.NoExecute != FALSE);
    NT_ASSERT(nptEntry->Fields.PageFrameNumber == GetPfnFromPa(
                                                CurrentHookEntry->PhyPageBase));

    //
    // Switch to the exec physical page so hooks can be executed, and make the
    // page executable.
    //
    nptEntry->Fields.PageFrameNumber = GetPfnFromPa(
                                CurrentHookEntry->PhyPageBaseForExecution);
    ChangePermissionOfPage(HookData->Pml4Table,
                           CurrentHookEntry->PhyPageBase,
                           FALSE);

    //
    // Transition completed.
    //
    HookData->ActiveHookEntry = CurrentHookEntry;
    HookData->NptState = NptHookEnabledVisible;
}

/*!
    @brief Transition the NPT state 2 to 1.

    @param[in,out] HookData - The processor associated hook data.
 */
static
VOID
TransitionNtpState2To1 (
    _Inout_ PHOOK_DATA HookData
    )
{
    PPT_ENTRY_4KB nptEntry;

    NT_ASSERT(HookData->NptState == NptHookEnabledVisible);
    NT_ASSERT(HookData->ActiveHookEntry != nullptr);

    //
    // Move 2 to 1. There is an active hook and no hooks on the page going to
    // be executed. This must mean the processor is on the state 2, ie, running
    // the page with hooks, and jumping out to outside of it.
    //
    PERFORMANCE_MEASURE_THIS_SCOPE();

    //
    // Make all pages executable.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
    //  v
    //  *)Transitioning           : RWX(E)  : RWX(O) : RWX(O)    << transitioning to here
    //  v
    //  *)Transitioning           : RWX(E)  : RW-(O) : RWX(O)
    //  v
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
    //
    ChangePermissionsOfAllPages(HookData->Pml4Table,
                                HookData->ActiveHookEntry->PhyPageBase,
                                FALSE,
                                HookData->MaxNptPdpEntriesUsed);

    //
    // Make all hooked pages non-executable.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
    //  v
    //  *)Transitioning           : RWX(E)  : RWX(O) : RWX(O)
    //  v
    //  *)Transitioning           : RWX(E)  : RW-(O) : RWX(O)    << transitioning to here
    //  v
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
    //
    for (const auto& registration : g_HookRegistrationEntries)
    {
        PERFORMANCE_MEASURE_THIS_SCOPE();
        ChangePermissionOfPage(HookData->Pml4Table,
                               registration.HookEntry.PhyPageBase,
                               TRUE);
    }

    //
    // Switch the current page to be backed by the original physical page.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
    //  v
    //  *)Transitioning           : RWX(E)  : RWX(O) : RWX(O)
    //  v
    //  *)Transitioning           : RWX(E)  : RW-(O) : RWX(O)
    //  v
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)    << transitioning to here
    //

    //
    // Get a NPT entry associated with the page the processor has been
    // executing on. The page should be backed by the exec page at this point.
    //
    nptEntry = GetNestedPageTableEntry(HookData->Pml4Table,
                                       HookData->ActiveHookEntry->PhyPageBase);
    NT_ASSERT(nptEntry != nullptr);
    NT_ASSERT(nptEntry->Fields.NoExecute != FALSE);
    NT_ASSERT(nptEntry->Fields.PageFrameNumber == GetPfnFromPa(
                            HookData->ActiveHookEntry->PhyPageBaseForExecution));

    //
    // Switch to the original physical page so it looks as if there were no
    // hooks.
    //
    nptEntry->Fields.PageFrameNumber = GetPfnFromPa(
                                        HookData->ActiveHookEntry->PhyPageBase);

    //
    // Transition completed.
    //
    HookData->ActiveHookEntry = nullptr;
    HookData->NptState = NptHookEnabledInvisible;
}

/*!
    @brief Transition the NPT state according with where the NPT fault occurred.

    @param[in,out] HookData - The processor associated hook data.

    @param[in] FaultPhysicalAddress - The physical address caused the NTP fault.
 */
static
VOID
TransitionNptState (
    _Inout_ PHOOK_DATA HookData,
    _In_ ULONG64 FaultPhysicalAddress
    )
{
    const HOOK_ENTRY* hookEntry;

    NT_ASSERT(HookData->NptState != NptDefault);

    hookEntry = FindHookEntryByPhysicalPage(FaultPhysicalAddress);
    if (hookEntry != nullptr)
    {
        //
        // Hook(s) found on this faulting page. This means the processor is
        // either on the state 1, ie, running the page without hooks, and
        // jumping into the page with hook(s), or the processor is already
        // on the state 2, ie, running the page with hooks, and jumping into
        // another page with hook(s). Those can be differentiated by whether
        // there is an active hook entry. If not, the former, if so, the latter.
        //
        if (HookData->ActiveHookEntry == nullptr)
        {
            TransitionNptState1To2(HookData, hookEntry);
        }
        else
        {
            //
            // Need to transition to 2 to 2 *for the other page*. The simplest
            // way to do this is to transit to 1 first and back to 2. This may
            // not be the most optimized way, but still run fast enough it seems.
            //
            TransitionNtpState2To1(HookData);
            TransitionNptState1To2(HookData, hookEntry);
        }
    }
    else
    {
        //
        // No hooks on this faulting page. This must mean the processor is
        // on the state 2, ie, running the page with hooks, and jumping out to
        // outside of it. Move to the state 1.
        //
        TransitionNtpState2To1(HookData);
    }
}

/*!
    @brief Handles #VMEXIT due to a Nested Page Table (NPT) fault.

    @details This function does either 1) build a NPT entry using the
        pre-allocated entries if the fault is due to MMIO access, or 2)
        transition the NTP state to realize hook.

    @param[in,out] GuestVmcb - The processor associated VMCB.

    @param[in,out] HookData - The processor associated hook data.
 */
_Use_decl_annotations_
VOID
HandleNestedPageFault (
    PVMCB GuestVmcb,
    PHOOK_DATA HookData
    )
{
    NPF_EXITINFO1 exitInfo;
    ULONG64 faultingPa;
    PPT_ENTRY_4KB nptEntry;

    PERFORMANCE_MEASURE_THIS_SCOPE();

    faultingPa = GuestVmcb->ControlArea.ExitInfo2;
    exitInfo.AsUInt64 = GuestVmcb->ControlArea.ExitInfo1;
    if (exitInfo.Fields.Valid == FALSE)
    {
        //
        // The faulting physical page does not have a corresponding NPT entry.
        // It is MMIO access since all physical memory ranges visible from the
        // system already have the NPT entries.
        //
        PERFORMANCE_MEASURE_THIS_SCOPE();
#if DBG
        nptEntry = GetNestedPageTableEntry(HookData->Pml4Table, faultingPa);
        NT_ASSERT((nptEntry == nullptr) || (nptEntry->Fields.Valid == FALSE));
#endif
        nptEntry = BuildSubTables(HookData->Pml4Table, faultingPa, HookData);
        if (nptEntry == nullptr)
        {
            SIMPLESVMHOOK_BUG_CHECK();
        }
        goto Exit;
    }

    //
    // The associated NTP entry existed. This fault must be due to protection
    // violation due to execution attempt. Transition the NTP state to handle
    // this request.
    //
    NT_ASSERT(exitInfo.Fields.Execute != FALSE);
    TransitionNptState(HookData, faultingPa);

Exit:
    return;
}

/*!
    @brief Enables all hooks (transition to the state 1).

    @param[in,out] HookData - The processor associated hook data.
 */
_Use_decl_annotations_
VOID
EnableHooks (
    PHOOK_DATA HookData
    )
{
    NT_ASSERT(HookData->NptState == NptDefault);
    NT_ASSERT(HookData->ActiveHookEntry == nullptr);

    //
    // Move 0 to 1. Make all pages with hooks non-executable.
    //
    //  State                     : Page Type
    //                            : Current : Hooked : Other
    //  0)NptDefault              : RWX(O)  : RWX(O) : RWX(O)
    //  v
    //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)   << transitioning to here
    //
    for (const auto& registration : g_HookRegistrationEntries)
    {
        PERFORMANCE_MEASURE_THIS_SCOPE();
        ChangePermissionOfPage(HookData->Pml4Table,
                               registration.HookEntry.PhyPageBase,
                               TRUE);
    }
    HookData->NptState = NptHookEnabledInvisible;
}

/*!
    @brief Disables all hooks (transition to the state 0).

    @param[in,out] HookData - The processor associated hook data.
 */
_Use_decl_annotations_
VOID
DisableHooks (
    PHOOK_DATA HookData
    )
{
    PPT_ENTRY_4KB entry;

    NT_ASSERT(HookData->NptState != NptDefault);

    if (HookData->NptState == NptHookEnabledInvisible)
    {
        NT_ASSERT(HookData->ActiveHookEntry == nullptr);

        //
        // Move 1 to 0. The processor is not executing on the page where hooks
        // are installed. This means we are at the state 1. Just make all
        // hooked pages executable.
        //
        //  State                     : Page Type
        //                            : Current : Hooked : Other
        //  1)NptHookEnabledInvisible : RWX(O)  : RW-(O) : RWX(O)
        //  v
        //  0)NptDefault              : RWX(O)  : RWX(O) : RWX(O)   << transitioning to here
        //
        for (const auto& registration : g_HookRegistrationEntries)
        {
            PERFORMANCE_MEASURE_THIS_SCOPE();
            ChangePermissionOfPage(HookData->Pml4Table,
                                   registration.HookEntry.PhyPageBase,
                                   FALSE);
        }
    }
    else
    {
        NT_ASSERT(HookData->ActiveHookEntry != nullptr);

        //
        // Move 2 to 0. The processor is executing on the page where hooks are
        // installed. This means we are at the state 2. This should actually not
        // happen unless we install hooks on the page where CPUID with
        // CPUID_SUBLEAF_DISABLE_HOOKS exists (ie, our driver).
        //
        NT_ASSERT(FALSE);
        PERFORMANCE_MEASURE_THIS_SCOPE();

        //
        // Make all pages executable.
        //
        //  State                     : Page Type
        //                            : Current : Hooked : Other
        //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
        //  v
        //  *)Transitioning           : RWX(E)  : RWX(O) : RWX(O)    << transitioning to here
        //  v
        //  0)NptDefault              : RWX(O)  : RWX(O) : RWX(O)
        //
        ChangePermissionsOfAllPages(HookData->Pml4Table,
                                    HookData->ActiveHookEntry->PhyPageBase,
                                    FALSE,
                                    HookData->MaxNptPdpEntriesUsed);

        //
        // Change the backing physical page of the current page to the original
        // physical page.
        //
        //
        //  State                     : Page Type
        //                            : Current : Hooked : Other
        //  2)NptHookEnabledVisible   : RWX(E)  : RW-(O) : RW-(O)
        //  v
        //  *)Transitioning           : RWX(E)  : RWX(O) : RWX(O)
        //  v
        //  0)NptDefault              : RWX(O)  : RWX(O) : RWX(O)    << transitioning to here
        //
        entry = GetNestedPageTableEntry(HookData->Pml4Table,
                                        HookData->ActiveHookEntry->PhyPageBase);

        //
        // The current page should be backed by the exec page,
        //
        NT_ASSERT(entry != nullptr);
        NT_ASSERT(entry->Fields.PageFrameNumber == GetPfnFromPa(
                        HookData->ActiveHookEntry->PhyPageBaseForExecution));

        entry->Fields.PageFrameNumber = GetPfnFromPa(
                                        HookData->ActiveHookEntry->PhyPageBase);
        HookData->ActiveHookEntry = nullptr;
    }

    HookData->NptState = NptDefault;
}

/*!
    @brief Injects #BP into the guest.

    @param[in,out] GuestVmcb - The processor associated VMCB.
 */
static
VOID
InjectBreakPointException (
    _Inout_ PVMCB GuestVmcb
    )
{
    EVENTINJ event;

    //
    // Inject #GP(vector = 13, type = 3 = exception) with no error code.
    // See "#BP - Breakpoint Exception (Vector 3)".
    //
    event.AsUInt64 = 0;
    event.Fields.Vector = 3;
    event.Fields.Type = 3;
    event.Fields.Valid = 1;
    GuestVmcb->ControlArea.EventInj = event.AsUInt64;

    //
    // Advance the guest RIP. When #BP is delivered, RIP must points to the
    // next instruction.
    //
    GuestVmcb->StateSaveArea.Rip = GuestVmcb->ControlArea.NRip;
}

/*!
    @brief Handles #VMEXIT due to #BP.

    @details This function either passes through the exception to the guest or
        redirects guest execution to a hook handler if the #BP happened on the
        address where our hook is already installed.

    @param[in,out] GuestVmcb - The processor associated VMCB.

    @param[in,out] HookData - The processor associated hook data.
 */
_Use_decl_annotations_
VOID
HandleBreakPointException (
    PVMCB GuestVmcb,
    PHOOK_DATA HookData
    )
{
    const HOOK_ENTRY* entry;

    UNREFERENCED_PARAMETER(HookData);

    entry = FindHookEntryByAddress(reinterpret_cast<PVOID>(
                                                GuestVmcb->StateSaveArea.Rip));
    if (entry != nullptr)
    {
        //
        // Transfer to the hook handler if the guest RIP is at where our hook
        // is installed on.
        //
        GuestVmcb->StateSaveArea.Rip = reinterpret_cast<ULONG64>(entry->Handler);
    }
    else
    {
        //
        // Otherwise, it is #BP originated from something else and must be
        // delivered to the guest.
        //
        InjectBreakPointException(GuestVmcb);
    }
}
