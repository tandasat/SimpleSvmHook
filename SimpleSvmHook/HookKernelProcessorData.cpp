/*!
    @file HookKernelProcessorData.cpp

    @brief Kernel mode code to initialize and cleanup hook data.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018-2021, Satoshi Tanda. All rights reserved.
 */
#include "HookKernelProcessorData.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"
#include "HookKernelCommon.hpp"

/*!
    @brief Frees the specified NPT and all sub tables.

    @param[in] Table - The address of the table to free.

    @param[in] TableLevel - The depth of the table. 4 for PML4, for example.
 */
template<typename TableType>
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
DestructNestedPageTablesInternal (
    _In_reads_(512) TableType* Table,
    _In_ ULONG TableLevel
    )
{
    TableType entry;
    PVOID subTable;

    for (ULONG i = 0; i < 512; ++i)
    {
        entry = Table[i];
        if (entry.Fields.Valid == FALSE)
        {
            continue;
        }

        subTable = GetVaFromPfn(entry.Fields.PageFrameNumber);
        switch (TableLevel)
        {
        case 4:
            //
            // Table == PML4, subTable == PDPT
            //
            DestructNestedPageTablesInternal(static_cast<PPDP_ENTRY_4KB>(subTable), 3);
            break;

        case 3:
            //
            // Table == PDPT, subTable == PDT
            //
            DestructNestedPageTablesInternal(static_cast<PPD_ENTRY_4KB>(subTable), 2);
            break;

        case 2:
            //
            // Table == PDT, subTable == PT
            //
            FreeContiguousMemory(subTable);
            break;

        default:
            NT_ASSERT(false);
        }
    }
    FreeContiguousMemory(Table);
}

/*!
    @brief Frees the NPT PML4 built by the BuildNestedPageTables function.

    @param[in] Pml4Table - The address of NPT PML4 to free.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
DestructNestedPageTables (
    _In_reads_(512) PPML4_ENTRY_4KB Pml4Table
    )
{
    DestructNestedPageTablesInternal(Pml4Table, 4);
}

/*!
    @brief Builds all NTP entries necessary to cover the specified physical
        memory address ranges.

    @details This function builds 1:1 pass-through NPT entries for the physical
        memory address ranges. This function also creates an entry for the
        APIC base address to avoid system hang. MMIO regions are not covered
        by this function and are later covered on NPT fault on demand. See the
        HandleNestedPageFault function for this.

    @param[in] MemoryDescriptor - The physical memory address ranges to build
        NTP entries.

    @param[out] Pml4Table - The address to receive built NPT PML4.

    @param[out] MaxPpeIndex - The address to receive the highest PDPT index + 1
        needs to be covered.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
BuildNestedPageTables (
    _In_ const PHYSICAL_MEMORY_DESCRIPTOR* MemoryDescriptor,
    _Outptr_result_nullonfailure_ PPML4_ENTRY_4KB* Pml4Table,
    _Out_ PULONG MaxPpeIndex
    )
{
    static constexpr ULONG oneGigabyte = 1024 * 1024 * 1024;

    NTSTATUS status;
    PPML4_ENTRY_4KB pml4Table;
    PPT_ENTRY_4KB entry;
    APIC_BASE apicBase;
    const PHYSICAL_MEMORY_RUN* currentRun;
    ULONG64 baseAddr;
    ULONG64 maxPpeIndex;

    *Pml4Table = nullptr;
    *MaxPpeIndex = 0;

    //
    // Create a PML4 table which manages up to 512GB of physical address.
    //
    pml4Table = static_cast<PPML4_ENTRY_4KB>(AllocateContiguousMemory(PAGE_SIZE));
    if (pml4Table == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Build all NTP entries based on the specified physical memory ranges.
    //
    for (ULONG runIndex = 0; runIndex < MemoryDescriptor->NumberOfRuns; ++runIndex)
    {
        currentRun = &MemoryDescriptor->Run[runIndex];
        baseAddr = currentRun->BasePage * PAGE_SIZE;
        for (PFN_NUMBER pageIndex = 0; pageIndex < currentRun->PageCount; ++pageIndex)
        {
            ULONG64 indexedAddr;

            indexedAddr = baseAddr + pageIndex * PAGE_SIZE;
            entry = BuildSubTables(pml4Table, indexedAddr, FALSE);
            if (entry == nullptr)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto Exit;
            }
        }
    }

    //
    // Create an entry for the APIC base.
    //
    apicBase.AsUInt64 = __readmsr(IA32_APIC_BASE);
    entry = BuildSubTables(pml4Table, apicBase.Fields.ApicBase * PAGE_SIZE, FALSE);
    if (entry == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Compute the max PDPT index based on the last descriptor entry that
    // describes the address of the highest physical page. The index is rounded
    // up with 1GB since a single PDPT entry manages 1GB. (eg, the index will be
    // 2 if the highest physical address is at 1800MB.)
    //
    currentRun = &MemoryDescriptor->Run[MemoryDescriptor->NumberOfRuns - 1];
    baseAddr = currentRun->BasePage * PAGE_SIZE;
    maxPpeIndex = ROUND_TO_SIZE(baseAddr + currentRun->PageCount * PAGE_SIZE,
                                oneGigabyte) / oneGigabyte;

    status = STATUS_SUCCESS;
    *Pml4Table = pml4Table;
    *MaxPpeIndex = static_cast<ULONG>(maxPpeIndex);

Exit:
    if (!NT_SUCCESS(status))
    {
        if (pml4Table != nullptr)
        {
            DestructNestedPageTables(pml4Table);
        }
    }
    return status;
}

/*!
    @brief Frees all unused pre-allocated NPT entries.

    @details This function only frees unused entries. Used entries are freed by
        the DestructNestedPageTables function as they are already referenced
        from NPT PML4.

    @param[in] Entries - The address of pre-allocated entries to free.

    @param[in] NumberOfEntries - The number of Entries.

    @param[in] NumberOfUsedEntries - The number of entries already used from
        Entries.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
CleanupPreAllocateEntries (
    _In_reads_(NumberOfEntries) PVOID* Entries,
    _In_ ULONG NumberOfEntries,
    _In_ _In_range_(0, NumberOfEntries) ULONG NumberOfUsedEntries
    )
{
    for (ULONG i = NumberOfUsedEntries; i < NumberOfEntries; ++i)
    {
        if (Entries[i] == nullptr)
        {
            break;
        }
        FreeContiguousMemory(Entries[i]);
    }
}

/*!
    @brief Initializes pre-allocated NTP entries.

    @param[out] Entries - The address of pre-allocated entries to initialize.

    @param[in] NumberOfEntries - The number of Entries.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
InitializePreAllocatedEntries (
    _Out_writes_(NumberOfEntries) PVOID* Entries,
    _In_ ULONG NumberOfEntries
    )
{
    NTSTATUS status;
    PVOID entry;

    RtlZeroMemory(Entries, sizeof(Entries[0]) * NumberOfEntries);

    for (ULONG i = 0; i < NumberOfEntries; ++i)
    {
        entry = AllocateNptEntry(nullptr);
        if (entry == nullptr)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        Entries[i] = entry;
    }

    status = STATUS_SUCCESS;

Exit:
    if (!NT_SUCCESS(status))
    {
        CleanupPreAllocateEntries(Entries, NumberOfEntries, 0);
    }
    return status;
}

/*!
    @brief Initializes per processor hook data.

    @details This function builds NTP entries, allocates pre-allocated NTP
        entries as part of initialization.

    @param[out] HookData - The address of hook data to initialize.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
_Use_decl_annotations_
NTSTATUS
InitializeHookData (
    PHOOK_DATA* HookData
    )
{
    NTSTATUS status;
    PHOOK_DATA hookData;

    *HookData = nullptr;

    //
    // Allocate hook data.
    //
#pragma prefast(suppress : 28118, "DISPATCH_LEVEL is ok as this always allocates NonPagedPool")
    hookData = static_cast<PHOOK_DATA>(ExAllocatePoolWithTag(NonPagedPool,
                                                             sizeof(*hookData),
                                                             k_PoolTag));
    if (hookData == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(hookData, sizeof(*hookData));

    //
    // Initialize Pml4Table and MaxNptPdpEntriesUsed.
    //
    status = BuildNestedPageTables(g_PhysicalMemoryDescriptor,
                                   &hookData->Pml4Table,
                                   &hookData->MaxNptPdpEntriesUsed);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    //
    // Initialize PreAllocatedNptEntries.
    //
    status = InitializePreAllocatedEntries(
                            hookData->PreAllocatedNptEntries,
                            RTL_NUMBER_OF(hookData->PreAllocatedNptEntries));
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    *HookData = hookData;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (hookData != nullptr)
        {
            if (hookData->Pml4Table != nullptr)
            {
                DestructNestedPageTables(hookData->Pml4Table);
            }
            ExFreePoolWithTag(hookData, k_PoolTag);
        }
    }
    return status;
}

/*!
    @brief Frees per processor hook data.

    @param[in,out] HookData - The address of hook data to free.
 */
_Use_decl_annotations_
VOID
CleanupHookData (
    PHOOK_DATA HookData
    )
{
    LOGGING_LOG_INFO("Pre-allocated entry usage: %ld / %Iu",
                     HookData->UsedPreAllocatedEntriesCount,
                     RTL_NUMBER_OF(HookData->PreAllocatedNptEntries));

    CleanupPreAllocateEntries(HookData->PreAllocatedNptEntries,
                              RTL_NUMBER_OF(HookData->PreAllocatedNptEntries),
                              HookData->UsedPreAllocatedEntriesCount);
    DestructNestedPageTables(HookData->Pml4Table);
    ExFreePoolWithTag(HookData, k_PoolTag);
}

/*!
    @brief Returns the physical address of NTP PML4.

    @param[in] HookData - The address of hook data to retrieve PML4.
 */
_Use_decl_annotations_
PHYSICAL_ADDRESS
GetPml4PhysicalAddress (
    const HOOK_DATA* HookData
    )
{
    return MmGetPhysicalAddress(HookData->Pml4Table);
}

