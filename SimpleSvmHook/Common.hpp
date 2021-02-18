/*!
    @file Common.hpp

    @brief Common bits across the project.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018-2021, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <intrin.h>
#include <ntifs.h>
#include <stdarg.h>

#include "Logging.hpp"
#include "Performance.hpp"

//
// Error annotation: Must succeed pool allocations are forbidden.
// Allocation failures cause a system crash.
//
#pragma prefast(disable : __WARNING_ERROR, "This is completely bogus.")

//
// SimpleSVM specific CPUID leaf and subleaf values.
//
#define CPUID_LEAF_SIMPLE_SVM_CALL          0x41414141
#define CPUID_SUBLEAF_UNLOAD_SIMPLE_SVM     0x41414141
#define CPUID_SUBLEAF_ENABLE_HOOKS          0x41414142
#define CPUID_SUBLEAF_DISABLE_HOOKS         0x41414143
#define CPUID_HV_MAX                        CPUID_HV_INTERFACE

//
// The pool tag.
//
static const ULONG k_PoolTag = 'MVSS';

//
// The handy macros to specify at which section the code placed.
//
#define SIMPLESVMHOOK_INIT  __declspec(code_seg("INIT"))
#define SIMPLESVMHOOK_PAGED __declspec(code_seg("PAGE"))

/*!
    @brief Breaks into a kernel debugger when it is present.

    @details This macro is emits software breakpoint that only hits when a
        kernel debugger is present. This macro is useful because it does not
        change the current frame unlike the DbgBreakPoint function, and
        breakpoint by this macro can be overwritten with NOP without impacting
        other breakpoints.
 */
#define SIMPLESVMHOOK_DEBUG_BREAK() \
    if (KD_DEBUGGER_NOT_PRESENT) \
    { \
        NOTHING; \
    } \
    else \
    { \
        __debugbreak(); \
    } \
    reinterpret_cast<void*>(0)

/*!
    @brief Breaks into a debugger if present, and then triggers bug check.
 */
#define SIMPLESVMHOOK_BUG_CHECK() \
    SIMPLESVMHOOK_DEBUG_BREAK(); \
    __pragma(warning(push)) \
    __pragma(warning(disable: __WARNING_USE_OTHER_FUNCTION)) \
    KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0) \
    __pragma(warning(pop))

/*!
    @brief Allocates page aligned, zero filled contiguous physical memory.

    @details This function allocates page aligned nonpaged pool where backed
        by contiguous physical pages. The allocated memory is zero filled and
        must be freed with FreeContiguousMemory. The allocated memory is
        executable.

    @param[in] NumberOfBytes - A size of memory to allocate in byte.

    @return A pointer to the allocated memory filled with zero; or NULL when
        there is insufficient memory to allocate requested size.
 */
inline
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
PVOID
AllocateContiguousMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;
    PHYSICAL_ADDRESS boundary, lowest, highest;

    boundary.QuadPart = lowest.QuadPart = 0;
    highest.QuadPart = -1;

#pragma prefast(suppress : 30030, "No alternative API on Windows 7.")
    memory = MmAllocateContiguousMemorySpecifyCacheNode(NumberOfBytes,
                                                        lowest,
                                                        highest,
                                                        boundary,
                                                        MmCached,
                                                        MM_ANY_NODE_OK);
    if (memory != nullptr)
    {
        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief Frees memory allocated by AllocateContiguousMemory.

    @param[in] BaseAddress - The address returned by AllocateContiguousMemory.
 */
inline
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
FreeContiguousMemory (
    _In_ PVOID BaseAddress
    )
{
    MmFreeContiguousMemory(BaseAddress);
}
