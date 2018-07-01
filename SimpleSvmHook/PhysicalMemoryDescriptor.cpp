/*!
    @file PhysicalMemoryDescriptor.cpp

    @brief Kernel code to get information about system's physical memory ranges.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "PhysicalMemoryDescriptor.hpp"
#include "Common.hpp"

/*!
    @brief Duplicates the physical memory descriptor.

    @details This function returns duplicated information obtained by the
        MmGetPhysicalMemoryRanges function. This function returns information
        on physical memory ranges available on the system, but not in a format
        that is intuitive to use. This function interprets and converts that
        output into the more usable format.

    @return The duplicated physical memory descriptor, or NULL on failure.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
PPHYSICAL_MEMORY_DESCRIPTOR
DuplicatePhysicalMemoryDescriptor (
    VOID
    )
{
    PPHYSICAL_MEMORY_DESCRIPTOR descriptor;
    PPHYSICAL_MEMORY_RANGE memoryRanges;
    ULONG numberOfRuns;
    PFN_NUMBER numberOfPages;
    ULONG descriptorSize;

    PAGED_CODE();

    descriptor = nullptr;

    //
    // Get PHYSICAL_MEMORY_RANGE.
    //
    memoryRanges = MmGetPhysicalMemoryRanges();
    if (memoryRanges == nullptr)
    {
        goto Exit;
    }

    //
    // Check how many runs (ranges) exist.
    //
    numberOfRuns = 0;
    numberOfPages = 0;
    for (/**/; /**/; ++numberOfRuns)
    {
        PPHYSICAL_MEMORY_RANGE currentRange;

        currentRange = &memoryRanges[numberOfRuns];
        if ((currentRange->BaseAddress.QuadPart == 0) &&
            (currentRange->NumberOfBytes.QuadPart == 0))
        {
            break;
        }
        numberOfPages += static_cast<PFN_NUMBER>(BYTES_TO_PAGES(
                                        currentRange->NumberOfBytes.QuadPart));
    }
    if (numberOfRuns == 0)
    {
        goto Exit;
    }

    //
    // Allocate PHYSICAL_MEMORY_DESCRIPTOR that is returned to the caller.
    //
    descriptorSize = sizeof(PHYSICAL_MEMORY_DESCRIPTOR) +
        sizeof(PHYSICAL_MEMORY_RUN) * (numberOfRuns - 1);
    descriptor = reinterpret_cast<PPHYSICAL_MEMORY_DESCRIPTOR>(
                ExAllocatePoolWithTag(NonPagedPool, descriptorSize, k_PerformancePoolTag));
    if (descriptor == nullptr)
    {
        goto Exit;
    }

    //
    // Convert PHYSICAL_MEMORY_RANGE into PHYSICAL_MEMORY_DESCRIPTOR.
    //
    descriptor->NumberOfRuns = numberOfRuns;
    descriptor->NumberOfPages = numberOfPages;
    for (ULONG runIndex = 0; runIndex < numberOfRuns; ++runIndex)
    {
        PPHYSICAL_MEMORY_RUN currentRun;
        PPHYSICAL_MEMORY_RANGE currentRange;

        currentRun = &descriptor->Run[runIndex];
        currentRange = &memoryRanges[runIndex];
        currentRun->BasePage = static_cast<ULONG_PTR>(
                            currentRange->BaseAddress.QuadPart >> PAGE_SHIFT);
        currentRun->PageCount = static_cast<ULONG_PTR>(BYTES_TO_PAGES(
                                        currentRange->NumberOfBytes.QuadPart));
    }

Exit:
    if (memoryRanges != nullptr)
    {
        ExFreePoolWithTag(memoryRanges, 'hPmM');
    }
    return descriptor;
}

/*!
    @brief Frees the memory descriptor duplicated by the
        DuplicatePhysicalMemoryDescriptor function.

    @param[in] Descriptor - The duplicated memory descriptor to free.
 */
_Use_decl_annotations_
VOID
FreePhysicalMemoryDescriptor (
    PPHYSICAL_MEMORY_DESCRIPTOR Descriptor
    )
{
    ExFreePoolWithTag(Descriptor, k_PerformancePoolTag);
}

/*!
    @brief Debug prints physical memory ranges.

    @param[in] Descriptor - The duplicated memory descriptor to dump.
 */
_Use_decl_annotations_
VOID
DumpPhysicalMemoryRanges (
    const PHYSICAL_MEMORY_DESCRIPTOR* Descriptor
    )
{
    ULONG64 totalPysicalMemorySize;

    for (ULONG runIndex = 0; runIndex < Descriptor->NumberOfRuns; ++runIndex)
    {
        const PHYSICAL_MEMORY_RUN* currentRun;
        ULONG64 baseAddr;

        currentRun = &Descriptor->Run[runIndex];
        baseAddr = currentRun->BasePage * PAGE_SIZE;
        LOGGING_LOG_DEBUG("Physical Memory Range: %016llx - %016llx",
                          baseAddr,
                          baseAddr + currentRun->PageCount * PAGE_SIZE);
    }

    totalPysicalMemorySize = Descriptor->NumberOfPages * PAGE_SIZE;
    LOGGING_LOG_DEBUG("Physical Memory Total: %llu KB",
                      totalPysicalMemorySize / 1024);
}
