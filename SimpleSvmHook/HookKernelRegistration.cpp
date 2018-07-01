/*!
    @file HookKernelRegistration.cpp

    @brief Kernel mode code to initialize hooks.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookKernelRegistration.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"
#include "HookKernelHandlers.hpp"

//
// A byte array that represents the below x64 code.
//  90               nop
//  ff2500000000     jmp     qword ptr cs:jmp_addr
// jmp_addr:
//  0000000000000000 dq 0
//
#include <pshpack1.h>
typedef struct _JMP_CODE
{
    UCHAR Nop;
    UCHAR Jmp[6];
    PVOID Address;
} JMP_CODE, *PJMP_CODE;
static_assert(sizeof(JMP_CODE) == 15, "Size check");
#include <poppack.h>

//
// Memory related resources allocated for a hook. This data structure is defined
// separately from HOOK_ENTRY because identical set of those data is shared with
// more than one HOOK_ENTRY when there are two or more hooks on the same page.
//
typedef struct _SHARED_MEMORY_ENTRY
{
    //
    // The page-size aligned virtual address where this entry represents to.
    //
    PVOID HookAddressBase;

    //
    // The virtual address of the execution page for HookAddressBase.
    //
    PVOID ExecPage;

    //
    // The MDL for HookAddressBase.
    //
    PMDL HookAddressMdl;
} SHARED_MEMORY_ENTRY, *PSHARED_MEMORY_ENTRY;

//
// Memory resource can be shared. It has the same number of elements as that of
// g_HookRegistrationEntries to handle cases when all hooks are installed on
// different pages (hence; no SHARED_MEMORY_ENTRY is shared).
//
static SHARED_MEMORY_ENTRY g_HookSharedMemoryEntries[
    RTL_NUMBER_OF(g_HookRegistrationEntries)];

/*!
    @brief Gets the SHARED_MEMORY_ENTRY entry to use for the specified address.

    @param[in] HookAddress - A virtual address where the hook is installed.

    @param[out] SharedMemoryEntry - An address to store the SHARED_MEMORY_ENTRY
        entry use.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
GetSharedMemoryEntry (
    _In_ PVOID HookAddress,
    _Outptr_result_nullonfailure_ PSHARED_MEMORY_ENTRY* SharedMemoryEntry
    )
{
    NTSTATUS status;
    PVOID hookAddressBase;
    PSHARED_MEMORY_ENTRY memoryEntry;
    PVOID execPage;
    PMDL mdl;

    *SharedMemoryEntry = nullptr;
    hookAddressBase = PAGE_ALIGN(HookAddress);
    memoryEntry = nullptr;
    execPage = nullptr;

    //
    // Enumerate all SHARED_MEMORY_ENTRY-ies.
    //
    for (auto& sharedMemoryEntry : g_HookSharedMemoryEntries)
    {
        //
        // Use the entry if it manages the same page as HookAddress.
        //
        if (sharedMemoryEntry.HookAddressBase == hookAddressBase)
        {
            *SharedMemoryEntry = &sharedMemoryEntry;
            status = STATUS_SUCCESS;
            goto Exit;
        }

        //
        // Use the first empty entry if such one is found before finding a
        // matched entry. This means there is no SHARED_MEMORY_ENTRY that
        // manages the specified address, and we need to create that.
        //
        if (sharedMemoryEntry.HookAddressBase == nullptr)
        {
            //
            // Let us initialize this empty entry.
            //
            memoryEntry = &sharedMemoryEntry;
            break;
        }
    }

    NT_ASSERT(memoryEntry != nullptr);
    _Analysis_assume_(memoryEntry != nullptr);

    //
    // No entry found. Create a new one for the hook address and copy the
    // original contents.
    //
#pragma prefast(suppress : 28118, "DISPATCH_LEVEL is ok as this always allocates NonPagedPool")
    execPage = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, k_PerformancePoolTag);
    if (execPage == nullptr)
    {
        LOGGING_LOG_ERROR("ExAllocatePoolWithTag failed : %lu", PAGE_SIZE);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    NT_ASSERT(execPage == PAGE_ALIGN(execPage));
    RtlCopyMemory(execPage, hookAddressBase, PAGE_SIZE);

    //
    // Lock the virtual address. The specified hook address can/will be tradable
    // pagable memory or where its physical address can be changed by the Memory
    // Manager at any time. We need to prevent that because we assume permanent
    // 1:1 mapping of the hook virtual and physical addresses.
    //
    mdl = IoAllocateMdl(hookAddressBase, PAGE_SIZE, FALSE, FALSE, nullptr);
    if (mdl == nullptr)
    {
        LOGGING_LOG_ERROR("IoAllocateMdl failed : %p", HookAddress);
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    __try
    {
        MmProbeAndLockPages(mdl, KernelMode, IoReadAccess);
    }
#pragma prefast(suppress : __WARNING_EXCEPTIONEXECUTEHANDLER, "Always want to handle exception")
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        LOGGING_LOG_ERROR("MmProbeAndLockPages failed : %08x", status);
        IoFreeMdl(mdl);
        goto Exit;
    }

    status = STATUS_SUCCESS;
    memoryEntry->HookAddressBase = hookAddressBase;
    memoryEntry->ExecPage = execPage;
    memoryEntry->HookAddressMdl = mdl;
    *SharedMemoryEntry = memoryEntry;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (execPage != nullptr)
        {
            ExFreePoolWithTag(execPage, k_PerformancePoolTag);
        }
    }
    return status;
}

/*!
    @brief Creates a code byte array for an absolute jump instruction.

    @param[in] Destination - A virtual address to jump to.

    @return A code byte array for an absolute jump instruction.
 */
static
_Check_return_
JMP_CODE
CreateJumpCode (
    _In_ PVOID Destination
    )
{
    //
    //  90               nop
    //  ff2500000000     jmp     qword ptr cs:jmp_addr
    // jmp_addr:
    //  0000000000000000 dq 0
    //
    return {
        0x90,
        {
            0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
        },
        Destination,
    };
}

/*!
    @brief Returns the length of the first instruction at the specified address.

    @details This is a simplistic length disassembler, that is, it takes an
        address, determines the first x86 instruction there, and returns the
        length of the instruction. It, however, only handles known byte patterns
        for simplicity instead of actually disassembling bytes. One can replace
        this implementation with better disassemble code as needed.

    @param[in] HookAddress - The address to install a hook.

    @param[out] InstructionLength - The address to receive the length of the
        instruction.

    @return TRUE when the first instruction is determined; otherwise, FALSE.
 */
static
_Success_(return)
_Check_return_
BOOLEAN
FindFirstInstruction (
    _In_ PVOID HookAddress,
    _Out_ PULONG InstructionLength
    )
{
    BOOLEAN ok;

    typedef struct _BYTE_PATTERN
    {
        //
        // An actual instruction length in bytes.
        //
        ULONG InsructionLength;

        //
        // A length of Bytes to use for match. This can be different from
        // InsructionLength when the last few bytes are variable and unnecessary
        // for determination of the instruction.
        //
        ULONG MatchLength;
        UCHAR Bytes[k_MaxInsturctionLength];
    } BYTE_PATTERN, *PBYTE_PATTERN;

    static const BYTE_PATTERN knownPatterns[] =
    {
        {   // push    rbx
            2, 2, { 0x40, 0x53, },
        },
        {   // push    rbp
            2, 2, { 0x40, 0x55, },
        },
        {   // push    rdi
            2, 2, { 0x40, 0x57, },
        },
        {   // sub     rsp, Imm
            4, 3, { 0x48, 0x83, 0xEC, /*Imm*/ },
        },
        {   // mov     [rsp - 8 + arg_8], rdx
            5, 5, { 0x48, 0x89, 0x54, 0x24, 0x10, },
        },
        {   // mov     [rsp + Offset], rbx
            5, 4, { 0x48, 0x89, 0x5c, 0x24, /*Offset*/ },
        },
        {   // mov     rax, rsp
            3, 3, { 0x48, 0x8B, 0xC4, },
        },
        {   // xor     edx, edx
            2, 2, { 0x33, 0xD2, },
        },
    };

    *InstructionLength = 0;

    ok = FALSE;
    for (auto& pattern : knownPatterns)
    {
        if (RtlEqualMemory(HookAddress, pattern.Bytes, pattern.MatchLength) != FALSE)
        {
            *InstructionLength = pattern.InsructionLength;
            ok = TRUE;
            goto Exit;
        }
    }

Exit:
    return ok;
}

/*!
    @brief Installs a hook on the exec page and builds the stub to call the
        original function.

    @details This is a simplistic disassembler function, that is, takes an
        address, determines the first x86 instruction there, and return
        information about it. It, however, only handles known byte patterns
        instead of actually disassembling bytes for simplicity. One can replace
        this implementation with better disassemble code as needed.

    @param[in] HookAddress - The address to install a hook.

    @param[in] ExecPage - The address of the page referenced on execution.

    @param[out] OriginalCallStub - The address to receive an address of code
        stub to call the original function.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
InstallHookOnExecPage (
    _In_ PVOID HookAddress,
    _In_ PVOID ExecPage,
    _Outptr_result_nullonfailure_ PVOID* OriginalCallStub
    )
{
    NTSTATUS status;
    JMP_CODE jmpCode;
    PVOID originalCallStub;
    ULONG instrLength;
    PUCHAR hookAddrInExecPage;

    *OriginalCallStub = nullptr;

    //
    // Determine the first instruction at the hook address, so that we can
    // safely replace it with a break point.
    //
    if (FindFirstInstruction(HookAddress, &instrLength) == FALSE)
    {
        PCUCHAR bytes;

        bytes = reinterpret_cast<PCUCHAR>(HookAddress);
        LOGGING_LOG_ERROR("No supported byte pattern found at %p", HookAddress);
        LOGGING_LOG_ERROR("Pattern: "
                          "%02x %02x %02x %02x %02x "
                          "%02x %02x %02x %02x %02x "
                          "%02x %02x %02x %02x %02x",
                          bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                          bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                          bytes[10], bytes[11], bytes[12], bytes[13], bytes[14]);
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    //
    // Bail out if the instruction belongs to two pages. FIXME as needed.
    //
    if (PAGE_ALIGN(Add2Ptr(HookAddress, instrLength - 1)) != PAGE_ALIGN(HookAddress))
    {
        LOGGING_LOG_ERROR("The target instruction at %p belongs to two pages",
                          HookAddress);
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    //
    // Allocate executable memory that is going to contain copy of the first
    // instruction and a jmp instruction to the next instruction of hooked
    // address. Namely, this memory is used as a stub to call an original code
    // after hook is installed.
    //
#pragma prefast(suppress : 30030, "Intentionally executable")
    originalCallStub = ExAllocatePoolWithTag(NonPagedPoolExecute,
                                             instrLength + sizeof(jmpCode),
                                             k_PerformancePoolTag);
    if (originalCallStub == nullptr)
    {
        LOGGING_LOG_ERROR("ExAllocatePoolWithTag failed : %Iu",
                          instrLength + sizeof(jmpCode));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Create a byte array that represents "jmp to the next instruction of
    // hooked address".
    //
    jmpCode = CreateJumpCode(Add2Ptr(HookAddress, instrLength));

    //
    // Copy the first instruction and the jmp code to the stub code memory.
    //
    RtlCopyMemory(originalCallStub, HookAddress, instrLength);
    RtlCopyMemory(Add2Ptr(originalCallStub, instrLength),
                  &jmpCode,
                  sizeof(jmpCode));

    //
    // Install a breakpoint to the exec page so that the hypervisor can tell
    // when it is being executed.
    //
    hookAddrInExecPage = reinterpret_cast<PUCHAR>(Add2Ptr(ExecPage,
                                                          BYTE_OFFSET(HookAddress)));
    *hookAddrInExecPage = 0xcc;
    (VOID)KeInvalidateAllCaches();

    status = STATUS_SUCCESS;
    *OriginalCallStub = originalCallStub;

Exit:
    return status;
}

/*!
    @brief Installs hook on the specified address without activating it, and
        initializes HOOK_ENTRY representing the hook.

    @param[out] HookEntry - The address of HOOK_ENTRY to initialize.

    @param[in] Handler - The address of the function that is executed instead
        of the original after the hook is activated.

    @param[in] HookAddress - The address to install the hook.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
InitializeHookEntry (
    _Out_ PHOOK_ENTRY HookEntry,
    _In_ PVOID Handler,
    _In_ PVOID HookAddress
    )
{
    NTSTATUS status;
    PSHARED_MEMORY_ENTRY sharedMemoryEntry;
    PVOID execPage;
    PVOID originalCallStub;

    RtlZeroMemory(HookEntry, sizeof(*HookEntry));
    execPage = nullptr;
    originalCallStub = nullptr;

    //
    // Get a memory resource for install hook on the address.
    //
    status = GetSharedMemoryEntry(HookAddress, &sharedMemoryEntry);
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("GetSharedMemoryEntry failed : %08x", status);
        goto Exit;
    }

    //
    // ExecPage should already be assigned. Also, it should contain a copy of
    // the page where the hook address belongs to (not verified due to the fact
    // that it is not perfectly true when the same page already had another hook
    // which should embed 0xcc to the ExecPage already).
    //
    NT_ASSERT(sharedMemoryEntry->ExecPage != nullptr);

    //
    // Install a hook (0xcc) to the ExecPage and get a stub to call the original
    // code.
    //
    status = InstallHookOnExecPage(HookAddress,
                                   sharedMemoryEntry->ExecPage,
                                   &originalCallStub);
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InstallHookOnExecPage failed : %08x", status);
        goto Exit;
    }

    HookEntry->Handler = Handler;
    HookEntry->HookAddress = HookAddress;
    HookEntry->PageBaseForExecution = sharedMemoryEntry->ExecPage;
    HookEntry->PhyPageBase = GetPaFromVa(PAGE_ALIGN(HookAddress));
    HookEntry->PhyPageBaseForExecution = GetPaFromVa(HookEntry->PageBaseForExecution);
    HookEntry->OriginalCallStub = originalCallStub;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (originalCallStub != nullptr)
        {
            ExFreePoolWithTag(originalCallStub, k_PerformancePoolTag);
        }
    }
    return status;
}

/*!
    @brief Builds all requested hooks *without* activating them.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
NTSTATUS
InitializeHookRegistrationEntries (
    VOID
    )
{
    NTSTATUS status;

    PAGED_CODE();

    status = STATUS_SUCCESS;

    //
    // Build HOOK_ENTRY for each hook registration entry.
    //
    for (auto& registration : g_HookRegistrationEntries)
    {
        PVOID functionAddr;

        NT_ASSERT(registration.FunctionName.Buffer != nullptr);

        functionAddr = MmGetSystemRoutineAddress(&registration.FunctionName);
        if (functionAddr == nullptr)
        {
            LOGGING_LOG_ERROR("MmGetSystemRoutineAddress failed : %wZ",
                              &registration.FunctionName);
            status = STATUS_PROCEDURE_NOT_FOUND;
            goto Exit;
        }

        status = InitializeHookEntry(&registration.HookEntry,
                                     registration.Handler,
                                     functionAddr);
        if (!NT_SUCCESS(status))
        {
            LOGGING_LOG_ERROR("InitializeHookEntry failed : %08x", status);
            goto Exit;
        }

        LOGGING_LOG_INFO("Hook installed at %p (ExecPage at %p) for %wZ",
                         functionAddr,
                         registration.HookEntry.PageBaseForExecution,
                         &registration.FunctionName);
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        //
        // Initialization of g_HookRegistrationEntries and backing
        // g_HookSharedMemoryEntries failed in the middle. Clean up any already
        // initialized entries.
        //
        for (auto& registration : g_HookRegistrationEntries)
        {
            if (registration.HookEntry.OriginalCallStub != nullptr)
            {
                ExFreePoolWithTag(registration.HookEntry.OriginalCallStub,
                                  k_PerformancePoolTag);
            }
        }
        for (auto& sharedMemoryEntry : g_HookSharedMemoryEntries)
        {
            if (sharedMemoryEntry.ExecPage != nullptr)
            {
                MmUnlockPages(sharedMemoryEntry.HookAddressMdl);
                IoFreeMdl(sharedMemoryEntry.HookAddressMdl);
                ExFreePoolWithTag(sharedMemoryEntry.ExecPage, k_PerformancePoolTag);
            }
        }
    }
    return status;
}

/*!
    @brief Cleans up all resources might be allocated by the
        InitializeHookRegistrationEntries function.
 */
_Use_decl_annotations_
VOID
CleanupHookRegistrationEntries (
    VOID
    )
{
    for (auto& registration : g_HookRegistrationEntries)
    {
        ExFreePoolWithTag(registration.HookEntry.OriginalCallStub,
                          k_PerformancePoolTag);
    }
    for (auto& sharedMemoryEntry : g_HookSharedMemoryEntries)
    {
        MmUnlockPages(sharedMemoryEntry.HookAddressMdl);
        IoFreeMdl(sharedMemoryEntry.HookAddressMdl);
        ExFreePoolWithTag(sharedMemoryEntry.ExecPage, k_PerformancePoolTag);
    }
}

/*!
    @brief Prints out how many time each hook is called.
 */
_Use_decl_annotations_
VOID
ReportHookActivities (
    VOID
    )
{
    LOGGING_LOG_INFO("ZwQuerySystemInformation called %llu times",
                     static_cast<ULONG64>(g_ZwQuerySystemInformationCounter));
    LOGGING_LOG_INFO("ExAllocatePoolWithTag called %llu times",
                     static_cast<ULONG64>(g_ExAllocatePoolWithTagCounter));
    LOGGING_LOG_INFO("ExFreePoolWithTag called %llu times",
                     static_cast<ULONG64>(g_ExFreePoolWithTagCounter));
    LOGGING_LOG_INFO("ExFreePool called %llu times",
                     static_cast<ULONG64>(g_ExFreePoolCounter));
}