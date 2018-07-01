/*!
    @file HookVmmAlwaysOptimized.cpp

    @brief VMM code that is always compiled with optimization due to slowness.

    @author Satoshi Tanda

    @copyright  Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookVmmAlwaysOptimized.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"

/*!
    @brief Change the permission of the page for execute access.

    @param[in,out] Pml4Table - The address of the NPT PML4 table.

    @param[in] PhysicalAddress - The physical memory address to change the
        permission.

    @param[in] DisallowExecution - TRUE to make the page non-executable.
 */
_Use_decl_annotations_
VOID
ChangePermissionOfPage (
    PPML4_ENTRY_4KB Pml4Table,
    ULONG64 PhysicalAddress,
    BOOLEAN DisallowExecution
    )
{
    PPML4_ENTRY_4KB pml4Entry;
    PPDP_ENTRY_4KB pageDirectoryPointerTable, pdptEntry;
    PPD_ENTRY_4KB pageDirectoryTable, pdtEntry;
    PPT_ENTRY_4KB pageTable, ptEntry;
    ULONG64 pxeIndex, ppeIndex, pdeIndex, pteIndex;

    //
    // Get an index of PML4 entry for the specified physical address, and then,
    // get the Page Directory Pointer Table (PDPT) from the entry. Note that
    // do not need to change permission of PML4 entry since we keep it
    // executable and never change it. The top level table we change permission
    // is PDPT.
    //
    pxeIndex = GetPxeIndex(PhysicalAddress);
    pml4Entry = &Pml4Table[pxeIndex];
    NT_ASSERT(pml4Entry->Fields.Valid != FALSE);
    pageDirectoryPointerTable = reinterpret_cast<PPDP_ENTRY_4KB>(GetVaFromPfn(
                                            pml4Entry->Fields.PageFrameNumber));

    //
    // Get an index of PDPT entry for the specified physical address, and then,
    // get the Page Directory Table (PDT) from the entry.
    //
    ppeIndex = GetPpeIndex(PhysicalAddress);
    pdptEntry = &pageDirectoryPointerTable[ppeIndex];
    NT_ASSERT(pdptEntry->Fields.Valid != FALSE);
    pageDirectoryTable = reinterpret_cast<PPD_ENTRY_4KB>(GetVaFromPfn(
                                            pdptEntry->Fields.PageFrameNumber));

    //
    // If the request is to make the page executable, and when this entire 1GB
    // range is configured to be non-executable, change this 1GB range to
    // executable first, then, make all sub-tables but ones for the specified
    // address non-executable later. The below diagram shows how NPTs are
    // updated in such a scenario.
    //
    //  Before                       After
    //
    //   PDPT                         PDPT
    //  +----+                       +----+
    //  | NX |                       | NX |
    //  +----+                       +----+
    //  | NX |    PDT                | NX |    PDT
    //  +----+---+----+              +----+---+----+
    //  | NX |   | EX |    PT        |*EX*|   | NX |    PT
    //  +----+   +----+---+----+     +----+   +----+---+----+
    //  | NX |\  | EX |   | EX | ==> | NX |\  |*EX*|   | NX |
    //  +----+ | +----+   +----+     +----+ | +----+   +----+
    //  | NX | | | EX |\  | EX |     | NX | | | NX |\  | NX |
    //  +----+ | +----+ | +----+     +----+ | +----+ | +----+
    //         | | EX | | | EX |            | | NX | | |*EX*|  << executable
    //         | +----+ | +----+            | +----+ | +----+
    //          \| EX | | | EX |             \| NX | | | NX |
    //           +----+ | +----+              +----+ | +----+
    //                   \| EX |                      \| NX |
    //                    +----+                       +----+
    //
    // This nested changes are required because 1) making only the leaf NTP
    // entry executable does not work because the page is still non-executable
    // if any of parent NPT entries are set to non-executable, and 2) entire
    // 1GB could become executable if PDPT NTP is changed to executable and
    // sub tables are also executable. This, unfortunately, requires two times
    // of 512 iterations, which making this function VERY slow.
    //
    if ((DisallowExecution == FALSE) &&
        (pdptEntry->Fields.NoExecute != FALSE))
    {
        pdptEntry->Fields.NoExecute = FALSE;

        //
        // Change all entries of permission in the sub-table (PDT) to
        // non-executable for this entire 1GB range to inherit the settings of
        // the parent PDPT.
        //
        PERFORMANCE_MEASURE_THIS_SCOPE();
        for (pdeIndex = 0; pdeIndex < 512; ++pdeIndex)
        {
            pdtEntry = &pageDirectoryTable[pdeIndex];
            pdtEntry->Fields.NoExecute = TRUE;
        }
    }

    //
    // Get an index of PDT entry for the specified physical address, and then,
    // get the Page Table (PDT) from the entry.
    //
    pdeIndex = GetPdeIndex(PhysicalAddress);
    pdtEntry = &pageDirectoryTable[pdeIndex];
    NT_ASSERT(pdtEntry->Fields.Valid != FALSE);
    pageTable = reinterpret_cast<PPT_ENTRY_4KB>(GetVaFromPfn(
                                            pdtEntry->Fields.PageFrameNumber));

    if ((DisallowExecution == FALSE) &&
        (pdtEntry->Fields.NoExecute != FALSE))
    {
        //
        // Do the same thing as we did for PDPT.
        //
        pdtEntry->Fields.NoExecute = FALSE;

        PERFORMANCE_MEASURE_THIS_SCOPE();
        for (pteIndex = 0; pteIndex < 512; ++pteIndex)
        {
            ptEntry = &pageTable[pteIndex];
            ptEntry->Fields.NoExecute = TRUE;
        }
    }

    //
    // Get an index of PDT entry for the specified physical address, and then,
    // change the permission of the page as requested.
    //
    pteIndex = GetPteIndex(PhysicalAddress);
    ptEntry = &pageTable[pteIndex];
    NT_ASSERT(pdtEntry->Fields.Valid != FALSE);
    ptEntry->Fields.NoExecute = DisallowExecution;
}

/*!
    @brief Change the permission of all NPT entries in the PDT and PT that
        manage the specified address.

    @details This function needs two times of 512 iterations and is VERY slow.

    @param[in,out] PageDirectoryPointerTable - The address of the NPT PML4 table.

    @param[in] ActiveHookPa - The physical memory address of the active hook
        page.
 */
static
VOID
MakeAllSubTablesExecutable (
    _Inout_ PPDP_ENTRY_4KB PageDirectoryPointerTable,
    _In_ ULONG64 ActiveHookPa
    )
{
    ULONG64 ppeIndex, pdeIndex, pteIndex;
    PPDP_ENTRY_4KB pdptEntry;
    PPD_ENTRY_4KB pageDirectoryTable, pdtEntry;
    PPT_ENTRY_4KB pageTable, ptEntry;

    PERFORMANCE_MEASURE_THIS_SCOPE();

    //
    // Get PDT for the specified physical address, and make all entries in the
    // table executable.
    //
    ppeIndex = GetPpeIndex(ActiveHookPa);
    pdptEntry = &PageDirectoryPointerTable[ppeIndex];
    NT_ASSERT(pdptEntry->Fields.Valid != FALSE);
    pageDirectoryTable = reinterpret_cast<PPD_ENTRY_4KB>(GetVaFromPfn(
                                            pdptEntry->Fields.PageFrameNumber));
    for (pdeIndex = 0; pdeIndex < 512; ++pdeIndex)
    {
        pdtEntry = &pageDirectoryTable[pdeIndex];
        pdtEntry->Fields.NoExecute = FALSE;
    }

    //
    // Get PT for the specified physical address, and make all entries in the
    // table executable.
    //
    pdeIndex = GetPdeIndex(ActiveHookPa);
    pdtEntry = &pageDirectoryTable[pdeIndex];
    NT_ASSERT(pdtEntry->Fields.Valid != FALSE);
    pageTable = reinterpret_cast<PPT_ENTRY_4KB>(GetVaFromPfn(
                                            pdtEntry->Fields.PageFrameNumber));
    for (pteIndex = 0; pteIndex < 512; ++pteIndex)
    {
        ptEntry = &pageTable[pteIndex];
        ptEntry->Fields.NoExecute = FALSE;
    }
}

/*!
    @brief Change the permissions of all physical memory pages on the system,
        except of the MMIO regions.

    @param[in,out] Pml4Table - The address of the NPT PML4 table.

    @param[in] ActiveHookPa - The physical memory address of the active hook
        page. Used when DisallowExecution is FALSE to make all necessary NTP
        entries executable, that was changed to non-executable with the
        ChangePermissionOfPage function.

    @param[in] DisallowExecution - TRUE to make the page non-executable.

    @param[in] MaxPpeIndex - The maximum index of PDPT to change the permission.
 */
_Use_decl_annotations_
VOID
ChangePermissionsOfAllPages (
    PPML4_ENTRY_4KB Pml4Table,
    ULONG64 ActiveHookPa,
    BOOLEAN DisallowExecution,
    ULONG MaxPpeIndex
    )
{
    PPML4_ENTRY_4KB pml4Entry;
    PPDP_ENTRY_4KB pageDirectoryPointerTable, pdptEntry;

    //
    // Get the first PML4 entry and change permission of entries in the
    // sub tables up to MaxPpeIndex. We always get the first entry assuming
    // that system has no more than 512 GB of physical memory. Also, we do
    // iterate only up to MaxPpeIndex to ignore physical memory ranges that are
    // unavailable on this system for performance. The below diagram shows how
    // entries are updated.
    //
    //   PML4    PDPT
    //  +----+---+----+
    //  | EX |   | EX |  << Updated
    //  +----+   +----+
    //  | U  |\  | EX |  << Updated
    //  + N -+ | +----+
    //  | U  | | | EX |  << Updated
    //  + S -+ | +----+
    //  | E  | | | U  |  << MaxPpeIndex
    //  + D -+ | + N -+
    //  |    | | | U  |
    //         | + S -+
    //         | | E  |
    //         | + D -+
    //         | |    |
    //
    pml4Entry = &Pml4Table[0];
    pageDirectoryPointerTable = reinterpret_cast<PPD_ENTRY_4KB>(GetVaFromPfn(
                                            pml4Entry->Fields.PageFrameNumber));

    for (ULONG ppeIndex = 0; ppeIndex < MaxPpeIndex; ++ppeIndex)
    {
        pdptEntry = &pageDirectoryPointerTable[ppeIndex];
        pdptEntry->Fields.NoExecute = DisallowExecution;
    }

    //
    // If we are making pages executable, we have to traverse all sub entries
    // and make any of them to executable too, because entries in PDT and PT
    // might be configured to be non-executable as described in the
    // ChangePermissionOfPage function.
    //
    if (DisallowExecution == FALSE)
    {
        MakeAllSubTablesExecutable(pageDirectoryPointerTable, ActiveHookPa);
    }
}