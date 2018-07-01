/*!
    @file HookCommon.cpp

    @brief Kernel mode and VMM shared code.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookCommon.hpp"
#include "Common.hpp"
#include "HookKernelHandlers.hpp"

//
// The list of functions to hook and their handlers. Must be NT kernel exported
// functions.
//
HOOK_REGISTRATION_ENTRY g_HookRegistrationEntries[] =
{
#if (SIMPLESVMHOOK_SINGLE_HOOK == 0)
    {
        RTL_CONSTANT_STRING(L"ZwQuerySystemInformation"),
        HandleZwQuerySystemInformation,
    },
    {
        RTL_CONSTANT_STRING(L"ExAllocatePoolWithTag"),
        HandleExAllocatePoolWithTag,
    },
    {
        RTL_CONSTANT_STRING(L"ExFreePoolWithTag"),
        HandleExFreePoolWithTag,
    },
    {
        RTL_CONSTANT_STRING(L"ExFreePool"),
        HandleExFreePool,
    },
#else
    //
    // Only one hook is installed when SIMPLESVMHOOK_SINGLE_HOOK is enabled.
    // This is for testing on VMwaere where SimpleSvmHook runs very slow.
    //
    {
        RTL_CONSTANT_STRING(L"ZwQuerySystemInformation"),
        HandleZwQuerySystemInformation,
    },
#endif
};

/*!
    @brief Returns an empty NPT entry to be used by the caller.

    @details This function allocates a new entry if HookData is NULL and can
        return NULL when allocation failed, or returns an entry from the
        pre-allocated entries if HookData is not NULL. In this case, this
        function should never return NULL.

    @param[in,out] HookData - Hook data to retrieve the pre-allocated entries.
        NULL when entries should be newly allocated.

    @return The address of an empty NPT entry.
 */
_Use_decl_annotations_
PVOID
AllocateNptEntry (
    PHOOK_DATA HookData
    )
{
    PVOID entry;

    if (ARGUMENT_PRESENT(HookData))
    {
        ULONG usedCount;

        usedCount = InterlockedIncrement(&HookData->UsedPreAllocatedEntriesCount);
        if (usedCount > RTL_NUMBER_OF(HookData->PreAllocatedNptEntries))
        {
            SIMPLESVMHOOK_BUG_CHECK();
        }
        entry = HookData->PreAllocatedNptEntries[usedCount - 1];
    }
    else
    {
        entry = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, k_PerformancePoolTag);
        if (entry == nullptr)
        {
            goto Exit;
        }
        RtlZeroMemory(entry, PAGE_SIZE);
    }

Exit:
    return entry;
}

/*!
    @brief Initializes a NPT entry.

    @param[out] Entry - The address of the NTP entry to initialize.

    @param[in] PhysicalAddress - The physical address where this entry
        refers to. MAXULONG64 when no such address exists due to missing the
        sub table and the sub table needs to be allocated.

    @param[in,out] HookData - Hook data to retrieve the pre-allocated entries.
        NULL when entries should be newly allocated.

    @return TRUE if the entry is successfully initialize; otherwise FALSE.
 */
template<typename EntryType>
static
_Success_(return)
_Check_return_
BOOLEAN
BuildNestedPageTableEntry (
    _Out_ EntryType* Entry,
    _In_ ULONG64 PhysicalAddress,
    _Inout_opt_ PHOOK_DATA HookData
    )
{
    BOOLEAN ok;
    PVOID subTable;
    PFN_NUMBER pageFrameNumber;

    if (PhysicalAddress == MAXULONG64)
    {
        //
        // No physical address to point to from this entry yet. This happens
        // when we are traversing the NTPs, but some entry is still empty and
        // needs new sub table to be allocated and assigned.
        //
#pragma prefast(suppress : __WARNING_MEMORY_LEAK, "Ownership is passed to NPTs.")
        subTable = AllocateNptEntry(HookData);
        if (subTable == nullptr)
        {
            ok = FALSE;
            goto Exit;
        }
        pageFrameNumber = GetPfnFromVa(subTable);
    }
    else
    {
        pageFrameNumber = GetPfnFromPa(PhysicalAddress);
    }

    Entry->Fields.Valid = TRUE;
    Entry->Fields.Write = TRUE;
    Entry->Fields.User = TRUE;
    Entry->Fields.PageFrameNumber = pageFrameNumber;

    ok = TRUE;

Exit:
    return ok;
}

typedef enum _NESTED_PAGE_TABLES_OPERATION
{
    //
    // Used to get the leaf NTP entry. No allocation is made.
    //
    FindOperation,

    //
    // Used to build NTP entries. Allocation may be made,
    //
    BuildOperation,
} NESTED_PAGE_TABLES_OPERATION, *PNESTED_PAGE_TABLES_OPERATION;

/*!
    @brief Operates on the NPTs for the specified address.

    @param[in,out] Pml4Table - The address of NPT PML4 to build the entries.

    @param[in] PhysicalAddress - The physical address of build the entries.

    @param[in] Operation - The type of operation against the NPTs.

    @param[in,out] HookData - Hook data to retrieve the pre-allocated entries.
         NULL when entries should be newly allocated.

    @return The address of the leaf NPT entry for the address if successful;
        otherwise, NULL.
 */
static
_Check_return_
PPT_ENTRY_4KB
OperateOnNestedPageTables (
    _Inout_ PPML4_ENTRY_4KB Pml4Table,
    _In_ ULONG64 PhysicalAddress,
    _In_ NESTED_PAGE_TABLES_OPERATION Operation,
    _When_(Operation == FindOperation, _Unreferenced_parameter_)
    _When_(Operation == BuildOperation, _Inout_opt_)
        PHOOK_DATA HookData
    )
{
    PPML4_ENTRY_4KB pml4Entry;
    PPDP_ENTRY_4KB pageDirectoryPointerTable, pdptEntry;
    PPD_ENTRY_4KB pageDirectoryTable, pdtEntry;
    PPT_ENTRY_4KB pageTable, ptEntry;
    ULONG64 pxeIndex, ppeIndex, pdeIndex, pteIndex;

    ptEntry = nullptr;

    //
    // PML4 (512 GB)
    //
    pxeIndex = GetPxeIndex(PhysicalAddress);
    pml4Entry = &Pml4Table[pxeIndex];
    if (pml4Entry->Fields.Valid == FALSE)
    {
        if (Operation != BuildOperation)
        {
            goto Exit;
        }

        if (BuildNestedPageTableEntry(pml4Entry, MAXULONG64, HookData) == FALSE)
        {
            goto Exit;
        }
    }
    pageDirectoryPointerTable = reinterpret_cast<PPDP_ENTRY_4KB>(GetVaFromPfn(
        pml4Entry->Fields.PageFrameNumber));

    //
    // PDPT (1 GB)
    //
    ppeIndex = GetPpeIndex(PhysicalAddress);
    pdptEntry = &pageDirectoryPointerTable[ppeIndex];
    if (pdptEntry->Fields.Valid == FALSE)
    {
        if (Operation != BuildOperation)
        {
            goto Exit;
        }

        if (BuildNestedPageTableEntry(pdptEntry, MAXULONG64, HookData) == FALSE)
        {
            goto Exit;
        }
    }
    pageDirectoryTable = reinterpret_cast<PPD_ENTRY_4KB>(GetVaFromPfn(
        pdptEntry->Fields.PageFrameNumber));

    //
    // PDT (2 MB)
    //
    pdeIndex = GetPdeIndex(PhysicalAddress);
    pdtEntry = &pageDirectoryTable[pdeIndex];
    if (pdtEntry->Fields.Valid == FALSE)
    {
        if (Operation != BuildOperation)
        {
            goto Exit;
        }

        if (BuildNestedPageTableEntry(pdtEntry, MAXULONG64, HookData) == FALSE)
        {
            goto Exit;
        }
    }
    pageTable = reinterpret_cast<PPT_ENTRY_4KB>(GetVaFromPfn(
        pdtEntry->Fields.PageFrameNumber));

    //
    // PT (4 KB)
    //
    pteIndex = GetPteIndex(PhysicalAddress);
    ptEntry = &pageTable[pteIndex];
    if (Operation == FindOperation)
    {
        goto Exit;
    }
    else if (Operation == BuildOperation)
    {
        //
        // Build request should only be made when the entry already exists.
        //
        NT_ASSERT(ptEntry->Fields.Valid == FALSE);
        (VOID)BuildNestedPageTableEntry(ptEntry, PhysicalAddress, HookData);
        // FIXME: set PAT
    }
    else
    {
        NT_ASSERT(FALSE);
    }

Exit:
    return ptEntry;
}

/*!
    @brief Builds all necessary NPT entries to manage the specified address.

    @param[in,out] Pml4Table - The address of NPT PML4 to build the entries.

    @param[in] PhysicalAddress - The physical address of build the entries.

    @param[in,out] HookData - Hook data to retrieve the pre-allocated entries.
        NULL when entries should be newly allocated.

    @return The address of the leaf NPT entry for the address if successful;
        otherwise, NULL.
 */
_Use_decl_annotations_
PPT_ENTRY_4KB
BuildSubTables (
    PPML4_ENTRY_4KB Pml4Table,
    ULONG64 PhysicalAddress,
    PHOOK_DATA HookData
    )
{
    return OperateOnNestedPageTables(Pml4Table,
                                     PhysicalAddress,
                                     BuildOperation,
                                     HookData);
}

/*!
    @brief Returns the leaf NPT entry corresponds to the specified address.

    @param[in,out] Pml4Table - The address of NPT PML4 to get the entry.

    @param[in] PhysicalAddress - The physical address of get the NTP entry.

    @return The address of the NPT entry if exists; otherwise, NULL.
 */
_Use_decl_annotations_
PPT_ENTRY_4KB
GetNestedPageTableEntry (
    PPML4_ENTRY_4KB Pml4Table,
    ULONG64 PhysicalAddress
    )
{
    return OperateOnNestedPageTables(Pml4Table,
                                     PhysicalAddress,
                                     FindOperation,
                                     nullptr);
}
