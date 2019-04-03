/*!
    @file Virtualization.cpp

    @brief Kernel code to virtualize and de-virtualize processors.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018-2019, Satoshi Tanda. All rights reserved.
 */
#include "Virtualization.hpp"
#include "Common.hpp"
#include "x86_64.hpp"
#include "Svm.hpp"
#include "HookKernelProcessorData.hpp"
#include "VmmMain.hpp"

EXTERN_C
VOID
_sgdt (
    _Out_ PVOID Descriptor
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
DECLSPEC_NORETURN
EXTERN_C
VOID
NTAPI
SvLaunchVm (
    _In_ PVOID HostRsp
    );

/*!
    @brief Allocates page aligned, zero filled physical memory.

    @details This function allocates page aligned nonpaged pool. The allocated
        memory is zero filled and must be freed with
        FreePageAlingedPhysicalMemory. On Windows 8 and later versions of
        Windows, the allocated memory is non executable.

    @param[in] NumberOfBytes - A size of memory to allocate in byte. This must
        be equal or greater than PAGE_SIZE.

    @return A pointer to the allocated memory filled with zero; or NULL when
        there is insufficient memory to allocate requested size.
 */
static
__drv_allocatesMem(Mem)
_Post_writable_byte_size_(NumberOfBytes)
_Post_maybenull_
_IRQL_requires_max_(DISPATCH_LEVEL)
_Must_inspect_result_
PVOID
AllocatePageAlingedPhysicalMemory (
    _In_ SIZE_T NumberOfBytes
    )
{
    PVOID memory;

    //
    // The size must be equal or greater than PAGE_SIZE in order to allocate
    // page aligned memory.
    //
    NT_ASSERT(NumberOfBytes >= PAGE_SIZE);

#pragma prefast(suppress : 28118, "DISPATCH_LEVEL is ok as this always allocates NonPagedPool")
    memory = ExAllocatePoolWithTag(NonPagedPool, NumberOfBytes, k_PoolTag);
    if (memory != nullptr)
    {
        NT_ASSERT(PAGE_ALIGN(memory) == memory);
        RtlZeroMemory(memory, NumberOfBytes);
    }
    return memory;
}

/*!
    @brief Frees memory allocated by AllocatePageAlingedPhysicalMemory.

    @param[in] BaseAddress - The address returned by
        AllocatePageAlingedPhysicalMemory.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
FreePageAlingedPhysicalMemory (
    _Frees_ptr_ PVOID BaseAddress
    )
{
    ExFreePoolWithTag(BaseAddress, k_PoolTag);
}

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
static
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
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
FreeContiguousMemory (
    _In_ PVOID BaseAddress
    )
{
    MmFreeContiguousMemory(BaseAddress);
}

/*!
    @brief Executes a callback on all processors one-by-one.

    @details This function execute Callback with Context as a parameter for
        each processor on the current IRQL. If the callback returned
        non-STATUS_SUCCESS value or any error occurred, this function stops
        execution of the callback and returns the error code.

        When NumOfProcessorCompleted is not NULL, this function always set a
        number of processors that successfully executed the callback.

    @param[in] Callback - A function to execute on all processors.

    @param[in] Context - A parameter to pass to the callback.

    @param[out] NumOfProcessorCompleted - A pointer to receive a number of
        processors executed the callback successfully.

    @return STATUS_SUCCESS when Callback executed and returned STATUS_SUCCESS
        on all processors; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_PAGED
static
_IRQL_requires_max_(APC_LEVEL)
_Check_return_
NTSTATUS
ExecuteOnEachProcessor (
    _In_ NTSTATUS (*Callback)(PVOID),
    _In_opt_ PVOID Context,
    _Out_opt_ PULONG NumOfProcessorCompleted
    )
{
    NTSTATUS status;
    ULONG i, numOfProcessors;
    PROCESSOR_NUMBER processorNumber;
    GROUP_AFFINITY affinity, oldAffinity;

    PAGED_CODE();

    status = STATUS_SUCCESS;

    //
    // Get a number of processors on this system.
    //
    numOfProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    for (i = 0; i < numOfProcessors; i++)
    {
        //
        // Convert from an index to a processor number.
        //
        status = KeGetProcessorNumberFromIndex(i, &processorNumber);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }

        //
        // Switch execution of this code to a processor #i.
        //
        affinity.Group = processorNumber.Group;
        affinity.Mask = 1ULL << processorNumber.Number;
        affinity.Reserved[0] = affinity.Reserved[1] = affinity.Reserved[2] = 0;
        KeSetSystemGroupAffinityThread(&affinity, &oldAffinity);

        //
        // Execute the callback.
        //
        status = Callback(Context);

        //
        // Revert the previously executed processor.
        //
        KeRevertToUserGroupAffinityThread(&oldAffinity);

        //
        // Exit if the callback returned error.
        //
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }

Exit:
    //
    // i must be the same as the number of processors on the system when this
    // function returns STATUS_SUCCESS;
    //
    NT_ASSERT(!NT_SUCCESS(status) || (i == numOfProcessors));

    //
    // Set a number of processors that successfully executed callback if the
    // out parameter is present.
    //
    if (ARGUMENT_PRESENT(NumOfProcessorCompleted))
    {
        *NumOfProcessorCompleted = i;
    }
    return status;
}

/*!
    @brief De-virtualize the current processor if virtualized.

    @details This function asks our hypervisor to deactivate itself through
        CPUID with a back-door function id and frees per processor data if it
        is returned. If our hypervisor is not installed, this function
        does nothing.

    @param[in] Context - An out pointer to receive an address of shared data.

    @return Always STATUS_SUCCESS.
 */
static
SIMPLESVMHOOK_PAGED
_IRQL_requires_max_(APC_LEVEL)
_Check_return_
NTSTATUS
DevirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    UINT64 high, low;
    PVIRTUAL_PROCESSOR_DATA vpData;
    PSHARED_VIRTUAL_PROCESSOR_DATA* sharedVpDataPtr;

    PAGED_CODE();

    NT_ASSERT(ARGUMENT_PRESENT(Context));
    _Analysis_assume_(ARGUMENT_PRESENT(Context));

    //
    // Ask our hypervisor to disable all hooks.
    //
    __cpuidex(registers, CPUID_LEAF_SIMPLE_SVM_CALL, CPUID_SUBLEAF_DISABLE_HOOKS);

    //
    // Ask our hypervisor to deactivate itself. If the hypervisor is
    // installed, this ECX is set to 'MVSS', and EDX:EAX indicates an address
    // of per processor data to be freed.
    //
    __cpuidex(registers, CPUID_LEAF_SIMPLE_SVM_CALL, CPUID_SUBLEAF_UNLOAD_SIMPLE_SVM);
    NT_ASSERT(registers[2] == k_PoolTag);
    LOGGING_LOG_INFO("The processor has been de-virtualized.");

    //
    // Get an address of per processor data indicated by EDX:EAX.
    //
    high = registers[3];
    low = registers[0] & MAXUINT32;
    vpData = reinterpret_cast<PVIRTUAL_PROCESSOR_DATA>(high << 32 | low);
    NT_ASSERT(vpData->HostStackLayout.Reserved1 == MAXUINT64);

    //
    // Save an address of shared data, then free per processor data.
    //
    sharedVpDataPtr = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA*>(Context);
    *sharedVpDataPtr = vpData->HostStackLayout.SharedVpData;
    CleanupHookData(vpData->HookData);
    FreePageAlingedPhysicalMemory(vpData);

    return STATUS_SUCCESS;
}

/*!
    @brief De-virtualize all virtualized processors.

    @details This function execute a callback to de-virtualize a processor on
        all processors, and frees shared data when the callback returned its
        pointer from a hypervisor.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
VOID
DevirtualizeAllProcessors (
    VOID
    )
{
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;

    PAGED_CODE();

    sharedVpData = nullptr;

    LOGGING_LOG_INFO("Start de-virtualizing the all processors.");

    //
    // De-virtualize all processors and free shared data when returned.
    //
    NT_VERIFY(NT_SUCCESS(ExecuteOnEachProcessor(DevirtualizeProcessor,
                                                &sharedVpData,
                                                nullptr)));
    if (sharedVpData != nullptr)
    {
        FreeContiguousMemory(sharedVpData->MsrPermissionsMap);
        FreePageAlingedPhysicalMemory(sharedVpData);
    }

    LOGGING_LOG_INFO("The all processors have been de-virtualized.");
}

/*!
    @brief Returns attributes of a segment specified by the segment selector.

    @details This function locates a segment descriptor from the segment
        selector and the GDT base, extracts attributes of the segment, and
        returns it. The returned value is the same as what the "dg" command of
        Windbg showed as "Flags". Here is an example output with 0x18 of the
        selector:
        ----
        0: kd> dg 18
        P Si Gr Pr Lo
        Sel        Base              Limit          Type    l ze an es ng Flags
        ---- ----------------- ----------------- ---------- - -- -- -- -- --------
        0018 00000000`00000000 00000000`00000000 Data RW Ac 0 Bg By P  Nl 00000493
        ----

    @param[in] SegmentSelector - A segment selector to get attributes of a
        corresponding descriptor.

    @param[in] GdtBase - A base address of GDT.

    @return Attributes of the segment.
 */
static
_Check_return_
UINT16
GetSegmentAccessRight (
    _In_ UINT16 SegmentSelector,
    _In_ ULONG_PTR GdtBase
    )
{
    PSEGMENT_DESCRIPTOR descriptor;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Get a segment descriptor corresponds to the specified segment selector.
    //
    descriptor = reinterpret_cast<PSEGMENT_DESCRIPTOR>(
                                        GdtBase + (SegmentSelector & ~RPL_MASK));

    //
    // Extract all attribute fields in the segment descriptor to a structure
    // that describes only attributes (as opposed to the segment descriptor
    // consists of multiple other fields).
    //
    attribute.Fields.Type = descriptor->Fields.Type;
    attribute.Fields.System = descriptor->Fields.System;
    attribute.Fields.Dpl = descriptor->Fields.Dpl;
    attribute.Fields.Present = descriptor->Fields.Present;
    attribute.Fields.Avl = descriptor->Fields.Avl;
    attribute.Fields.LongMode = descriptor->Fields.LongMode;
    attribute.Fields.DefaultBit = descriptor->Fields.DefaultBit;
    attribute.Fields.Granularity = descriptor->Fields.Granularity;
    attribute.Fields.Reserved1 = 0;

    return attribute.AsUInt16;
}

/*!
    @brief Tests whether our hypervisor is installed.

    @details This function checks a result of CPUID leaf 40000000h, which should
        return a vendor name of the hypervisor if any of those who implement
        the Microsoft Hypervisor interface is installed. If our hypervisor is
        installed, this should return "SimpleSvm", and if no hypervisor is
        installed, it the result of CPUID is undefined. For more details of the
        interface, see "Requirements for implementing the Microsoft Hypervisor
        interface"
        https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx

    @return TRUE when our hypervisor is installed; otherwise, FALSE.
 */
static
_Check_return_
BOOLEAN
IsOurHypervisorInstalled (
    VOID
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    char vendorId[13];

    //
    // When our hypervisor is installed, CPUID leaf 40000000h will return
    // "SimpleSvm   " as the vendor name.
    //
    __cpuid(registers, CPUID_HV_VENDOR_AND_MAX_FUNCTIONS);
    RtlCopyMemory(vendorId + 0, &registers[1], sizeof(registers[1]));
    RtlCopyMemory(vendorId + 4, &registers[2], sizeof(registers[2]));
    RtlCopyMemory(vendorId + 8, &registers[3], sizeof(registers[3]));
    vendorId[12] = ANSI_NULL;

    return (strcmp(vendorId, "SimpleSvm   ") == 0);
}

/*!
    @brief Initializes per processor data to virtualize the current processor.

    @param[in,out] VpData - The address of per processor data.

    @param[in] SharedVpData - The address of share data.

    @param[in] ContextRecord - The address of CONETEXT to use as an initial
        context of the processor after it is virtualized.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
PrepareForVirtualization (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
    _In_ const CONTEXT* ContextRecord
    )
{
    DESCRIPTOR_TABLE_REGISTER gdtr, idtr;
    PHYSICAL_ADDRESS guestVmcbPa, hostVmcbPa, hostStateAreaPa, pml4BasePa, msrpmPa;

    //
    // Capture the current GDTR and IDTR to use as initial values of the guest
    // mode.
    //
    _sgdt(&gdtr);
    __sidt(&idtr);

    guestVmcbPa = MmGetPhysicalAddress(&VpData->GuestVmcb);
    hostVmcbPa = MmGetPhysicalAddress(&VpData->HostVmcb);
    hostStateAreaPa = MmGetPhysicalAddress(&VpData->HostStateArea);
    pml4BasePa = GetPml4PhysicalAddress(VpData->HookData);
    msrpmPa = MmGetPhysicalAddress(SharedVpData->MsrPermissionsMap);

    //
    // Intercept break point exception. This is required to redirect execution
    // of a hooked address (where a breakpoint is set) to the corresponding
    // hook handler function with our hypervisor.
    //
    VpData->GuestVmcb.ControlArea.InterceptException |= (1UL << 3);

    //
    // Configure to trigger #VMEXIT with CPUID and VMRUN instructions. CPUID is
    // intercepted to present existence of our hypervisor and provide
    // an interface to ask it to unload itself.
    //
    // VMRUN is intercepted because it is required by the processor to enter the
    // guest mode; otherwise, #VMEXIT occurs due to VMEXIT_INVALID when a
    // processor attempts to enter the guest mode. See "Canonicalization and
    // Consistency Checks" on "VMRUN Instruction".
    //
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_CPUID;
    VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMRUN;

    //
    // Also, configure to trigger #VMEXIT on MSR access as configured by the
    // MSRPM. In our case, write to IA32_MSR_EFER is intercepted.
    //
    VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_MSR_PROT;
    VpData->GuestVmcb.ControlArea.MsrpmBasePa = msrpmPa.QuadPart;

    //
    // Specify guest's address space ID (ASID). TLB is maintained by the ID for
    // guests. Use the same value for all processors since all of them run a
    // single guest in our case. Use 1 as the most likely supported ASID by the
    // processor. The actual the supported number of ASID can be obtained with
    // CPUID. See "CPUID Fn8000_000A_EBX SVM Revision and Feature
    // Identification". Zero of ASID is reserved and illegal.
    //
    VpData->GuestVmcb.ControlArea.GuestAsid = 1;

    //
    // Enable Nested Page Tables. By enabling this, the processor performs the
    // nested page walk, that involves with an additional page walk to translate
    // a guest physical address to a system physical address. An address of
    // nested page tables is specified by the NCr3 field of VMCB.
    //
    // We have already build the nested page tables with BuildNestedPageTables.
    //
    // Note that our hypervisor does not trigger any additional #VMEXIT due to
    // the use of Nested Page Tables since all physical addresses from 0-512 GB
    // are configured to be accessible from the guest.
    //
    VpData->GuestVmcb.ControlArea.NpEnable |= SVM_NP_ENABLE_NP_ENABLE;
    VpData->GuestVmcb.ControlArea.NCr3 = pml4BasePa.QuadPart;

    //
    // Set up the initial guest state based on the current system state. Those
    // values are loaded into the processor as guest state when the VMRUN
    // instruction is executed.
    //
    VpData->GuestVmcb.StateSaveArea.GdtrBase = gdtr.Base;
    VpData->GuestVmcb.StateSaveArea.GdtrLimit = gdtr.Limit;
    VpData->GuestVmcb.StateSaveArea.IdtrBase = idtr.Base;
    VpData->GuestVmcb.StateSaveArea.IdtrLimit = idtr.Limit;

    VpData->GuestVmcb.StateSaveArea.CsLimit = GetSegmentLimit(ContextRecord->SegCs);
    VpData->GuestVmcb.StateSaveArea.DsLimit = GetSegmentLimit(ContextRecord->SegDs);
    VpData->GuestVmcb.StateSaveArea.EsLimit = GetSegmentLimit(ContextRecord->SegEs);
    VpData->GuestVmcb.StateSaveArea.SsLimit = GetSegmentLimit(ContextRecord->SegSs);
    VpData->GuestVmcb.StateSaveArea.CsSelector = ContextRecord->SegCs;
    VpData->GuestVmcb.StateSaveArea.DsSelector = ContextRecord->SegDs;
    VpData->GuestVmcb.StateSaveArea.EsSelector = ContextRecord->SegEs;
    VpData->GuestVmcb.StateSaveArea.SsSelector = ContextRecord->SegSs;
    VpData->GuestVmcb.StateSaveArea.CsAttrib = GetSegmentAccessRight(ContextRecord->SegCs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.DsAttrib = GetSegmentAccessRight(ContextRecord->SegDs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.EsAttrib = GetSegmentAccessRight(ContextRecord->SegEs, gdtr.Base);
    VpData->GuestVmcb.StateSaveArea.SsAttrib = GetSegmentAccessRight(ContextRecord->SegSs, gdtr.Base);

    VpData->GuestVmcb.StateSaveArea.Efer = __readmsr(IA32_MSR_EFER);
    VpData->GuestVmcb.StateSaveArea.Cr0 = __readcr0();
    VpData->GuestVmcb.StateSaveArea.Cr2 = __readcr2();
    VpData->GuestVmcb.StateSaveArea.Cr3 = __readcr3();
    VpData->GuestVmcb.StateSaveArea.Cr4 = __readcr4();
    VpData->GuestVmcb.StateSaveArea.Rflags = ContextRecord->EFlags;
    VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
    VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
    VpData->GuestVmcb.StateSaveArea.GPat = __readmsr(IA32_MSR_PAT);

    //
    // Save some of the current state on VMCB. Some of those states are:
    // - FS, GS, TR, LDTR (including all hidden state)
    // - KernelGsBase
    // - STAR, LSTAR, CSTAR, SFMASK
    // - SYSENTER_CS, SYSENTER_ESP, SYSENTER_EIP
    // See "VMSAVE and VMLOAD Instructions" for mode details.
    //
    // Those are restored to the processor right before #VMEXIT with the VMLOAD
    // instruction so that the guest can start its execution with saved state,
    // and also, re-saved to the VMCS with right after #VMEXIT with the VMSAVE
    // instruction so that the host (hypervisor) do not destroy guest's state.
    //
    __svm_vmsave(guestVmcbPa.QuadPart);

    //
    // Store data to stack so that the host (hypervisor) can use those values.
    //
    VpData->HostStackLayout.Reserved1 = MAXUINT64;
    VpData->HostStackLayout.SharedVpData = SharedVpData;
    VpData->HostStackLayout.Self = VpData;
    VpData->HostStackLayout.HostVmcbPa = hostVmcbPa.QuadPart;
    VpData->HostStackLayout.GuestVmcbPa = guestVmcbPa.QuadPart;

    //
    // Set an address of the host state area to VM_HSAVE_PA MSR. The processor
    // saves some of the current state on VMRUN and loads them on #VMEXIT. See
    // "VM_HSAVE_PA MSR (C001_0117h)".
    //
    __writemsr(SVM_MSR_VM_HSAVE_PA, hostStateAreaPa.QuadPart);

    //
    // Also, save some of the current state to VMCB for the host. This is loaded
    // after #VMEXIT to reproduce the current state for the host (hypervisor).
    //
    __svm_vmsave(hostVmcbPa.QuadPart);
}

/*!
    @brief Virtualizes the current processor.

    @details This function enables SVM, initialize VMCB with the current
        processor state, and enters the guest mode on the current processor.

    @param[in] Context - A pointer of share data.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(DISPATCH_LEVEL)
_Check_return_
NTSTATUS
VirtualizeProcessor (
    _In_opt_ PVOID Context
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    PVIRTUAL_PROCESSOR_DATA vpData;
    PCONTEXT contextRecord;
    int registers[4];   // EAX, EBX, ECX, and EDX

    vpData = nullptr;

    NT_ASSERT(ARGUMENT_PRESENT(Context));
    _Analysis_assume_(ARGUMENT_PRESENT(Context));

#pragma prefast(suppress : 28118, "DISPATCH_LEVEL is ok as this always allocates NonPagedPool")
    contextRecord = static_cast<PCONTEXT>(ExAllocatePoolWithTag(
                                                        NonPagedPool,
                                                        sizeof(*contextRecord),
                                                        k_PoolTag));
    if (contextRecord == nullptr)
    {
        LOGGING_LOG_ERROR("ExAllocatePoolWithTag failed : %Iu",
                          sizeof(*contextRecord));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Allocate per processor data.
    //
#pragma prefast(suppress : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    vpData = static_cast<PVIRTUAL_PROCESSOR_DATA>(AllocatePageAlingedPhysicalMemory(
                                                sizeof(VIRTUAL_PROCESSOR_DATA)));
    if (vpData == nullptr)
    {
        LOGGING_LOG_ERROR("AllocatePageAlingedPhysicalMemory failed : %Iu",
                          sizeof(VIRTUAL_PROCESSOR_DATA));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = InitializeHookData(&vpData->HookData);
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializeHookData failed : %08x", status);
        goto Exit;
    }

    //
    // Capture the current RIP, RSP, RFLAGS, and segment selectors. This
    // captured state is used as an initial state of the guest mode; therefore
    // when virtualization starts by the later call of SvLaunchVm, a processor
    // resume its execution at this location and state.
    //
    RtlCaptureContext(contextRecord);

    //
    // First time of this execution, our hypervisor is not installed
    // yet. Therefore, the branch is taken, and virtualization is attempted.
    //
    // At the second execution of here, after SvLaunchVm virtualized the
    // processor, IsOurHypervisorInstalled returns TRUE, and this
    // function exits with STATUS_SUCCESS.
    //
    if (IsOurHypervisorInstalled() == FALSE)
    {
        LOGGING_LOG_INFO("Attempting to virtualize the processor.");
        sharedVpData = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(Context);

        //
        // Enable SVM by setting EFER.SVME. It has already been verified that this
        // bit was writable with IsSvmSupported.
        //
        __writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) | EFER_SVME);

        //
        // Set up VMCB, the structure describes the guest state and what events
        // within the guest should be intercepted, ie, triggers #VMEXIT.
        //
        PrepareForVirtualization(vpData, sharedVpData, contextRecord);

        //
        // Switch to the host RSP to run as the host (hypervisor), and then
        // enters loop that executes code as a guest until #VMEXIT happens and
        // handles #VMEXIT as the host.
        //
        // This function should never return to here.
        //
        SvLaunchVm(&vpData->HostStackLayout.GuestVmcbPa);
        SIMPLESVMHOOK_BUG_CHECK();
    }

    LOGGING_LOG_INFO("The processor has been virtualized.");

    //
    // Ask our hypervisor to activate all hooks.
    //
    __cpuidex(registers, CPUID_LEAF_SIMPLE_SVM_CALL, CPUID_SUBLEAF_ENABLE_HOOKS);

    status = STATUS_SUCCESS;

Exit:
    if (contextRecord != nullptr)
    {
        ExFreePoolWithTag(contextRecord, k_PoolTag);
    }
    if (!NT_SUCCESS(status))
    {
        //
        // clean up per processor data when this function was unsuccessful.
        //
        if (vpData != nullptr)
        {
            if (vpData->HookData != nullptr)
            {
                CleanupHookData(vpData->HookData);
            }
            FreePageAlingedPhysicalMemory(vpData);
        }
    }
    return status;
}

/*!
    @brief Builds the MSR permissions map (MSRPM).

    @details This function sets up MSRPM to intercept to IA32_MSR_EFER, as
        suggested in "Extended Feature Enable Register (EFER)"
        ----
        Secure Virtual Machine Enable (SVME) Bit
        Bit 12, read/write. Enables the SVM extensions. (...) The effect of
        turning off EFER.SVME while a guest is running is undefined; therefore,
        the VMM should always prevent guests from writing EFER.
        ----

        Each MSR is controlled by two bits in the MSRPM. The LSB of the two
        bits controls read access to the MSR and the MSB controls write access.
        A value of 1 indicates that the operation is intercepted. This function
        locates an offset for IA32_MSR_EFER and sets the MSB bit. For details
        of logic, see "MSR Intercepts".

    @param[in,out] MsrPermissionsMap - The MSRPM to set up.
 */
static
VOID
BuildMsrPermissionsMap (
    _Inout_ PVOID MsrPermissionsMap
    )
{
    static const UINT32 bitsPerMsr = 2;
    static const UINT32 secondMsrRangeBase = 0xc0000000;
    static const UINT32 secondMsrpmOffset = 0x800 * CHAR_BIT;

    RTL_BITMAP bitmapHeader;
    ULONG offsetFrom2ndBase, offset;

    //
    // Setup and clear all bits, indicating no MSR access should be intercepted.
    //
    RtlInitializeBitMap(&bitmapHeader,
                        static_cast<PULONG>(MsrPermissionsMap),
                        SVM_MSR_PERMISSIONS_MAP_SIZE * CHAR_BIT);
    RtlClearAllBits(&bitmapHeader);

    //
    // Compute an offset from the second MSR permissions map offset (0x800) for
    // IA32_MSR_EFER in bits. Then, add an offset until the second MSR
    // permissions map.
    //
    offsetFrom2ndBase = (IA32_MSR_EFER - secondMsrRangeBase) * bitsPerMsr;
    offset = secondMsrpmOffset + offsetFrom2ndBase;

    //
    // Set the MSB bit indicating write accesses to the MSR should be intercepted.
    //
    RtlSetBits(&bitmapHeader, offset + 1, 1);
}

/*!
    @brief Tests whether the current processor support the SVM feature.

    @details This function tests whether the current processor has enough
        features to run our hypervisor, especially about SVM features.

    @return TRUE if the processor supports the SVM feature; otherwise, FALSE.
 */
static
_Check_return_
BOOLEAN
IsSvmSupported (
    VOID
    )
{
    BOOLEAN svmSupported;
    int registers[4];   // EAX, EBX, ECX, and EDX
    ULONG64 vmcr;

    svmSupported = FALSE;

    //
    // Test if the current processor is AMD one. An AMD processor should return
    // "AuthenticAMD" from CPUID function 0. See "Function 0h-Maximum Standard
    // Function Number and Vendor String".
    //
    __cpuid(registers, CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING);
    if ((registers[1] != 'htuA') ||
        (registers[3] != 'itne') ||
        (registers[2] != 'DMAc'))
    {
        goto Exit;
    }

    //
    // Test if the SVM feature is supported by the current processor. See
    // "Enabling SVM" and "CPUID Fn8000_0001_ECX Feature Identifiers".
    //
    __cpuid(registers, CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX);
    if (!BooleanFlagOn(registers[2], CPUID_FN8000_0001_ECX_SVM))
    {
        goto Exit;
    }

    //
    // Test if the Nested Page Tables feature is supported by the current
    // processor. See "Enabling Nested Paging" and "CPUID Fn8000_000A_EDX SVM
    // Feature Identification".
    //
    __cpuid(registers, CPUID_SVM_FEATURES);
    if (!BooleanFlagOn(registers[3], CPUID_FN8000_000A_EDX_NP))
    {
        goto Exit;
    }

    //
    // Test if the SVM feature can be enabled. When VM_CR.SVMDIS is set,
    // EFER.SVME cannot be 1; therefore, SVM cannot be enabled. When
    // VM_CR.SVMDIS is clear, EFER.SVME can be written normally and SVM can be
    // enabled. See "Enabling SVM".
    //
    vmcr = __readmsr(SVM_MSR_VM_CR);
    if (BooleanFlagOn(vmcr, SVM_VM_CR_SVMDIS))
    {
        goto Exit;
    }

    svmSupported = TRUE;

Exit:
    return svmSupported;
}

/*!
    @brief Virtualizes all processors on the system.

    @details This function attempts to virtualize all processors on the system,
        and returns STATUS_SUCCESS if all processors are successfully
        virtualized. If any processor is not virtualized, this function
        de-virtualizes all processors and returns an error code.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
NTSTATUS
VirtualizeAllProcessors (
    VOID
    )
{
    NTSTATUS status;
    PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
    ULONG numOfProcessorsCompleted;

    PAGED_CODE();

    sharedVpData = nullptr;
    numOfProcessorsCompleted = 0;

    LOGGING_LOG_INFO("Start virtualizing the all processors.");

    //
    // Test whether the current processor supports all required SVM features. If
    // not, exit as error.
    //
    if (IsSvmSupported() == FALSE)
    {
        LOGGING_LOG_ERROR("SVM is not fully supported on this processor.");
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto Exit;
    }

    //
    // Allocate a data structure shared across all processors. This data is
    // page tables used for Nested Page Tables.
    //
#pragma prefast(suppress : __WARNING_MEMORY_LEAK, "Ownership is taken on success.")
    sharedVpData = static_cast<PSHARED_VIRTUAL_PROCESSOR_DATA>(ExAllocatePoolWithTag(
                                        NonPagedPool,
                                        sizeof(SHARED_VIRTUAL_PROCESSOR_DATA),
                                        k_PoolTag));
    if (sharedVpData == nullptr)
    {
        LOGGING_LOG_ERROR("ExAllocatePoolWithTag failed : %Iu",
                          sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Allocate MSR permissions map (MSRPM) onto contiguous physical memory.
    //
    sharedVpData->MsrPermissionsMap = AllocateContiguousMemory(
                                                SVM_MSR_PERMISSIONS_MAP_SIZE);
    if (sharedVpData->MsrPermissionsMap == nullptr)
    {
        LOGGING_LOG_ERROR("AllocateContiguousMemory failed : %lu",
                          SVM_MSR_PERMISSIONS_MAP_SIZE);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Build nested page table and MSRPM.
    //
    BuildMsrPermissionsMap(sharedVpData->MsrPermissionsMap);

    //
    // Execute VirtualizeProcessor on and virtualize each processor one-by-one.
    // How many processors were successfully virtualized is stored in the third
    // parameter.
    //
    // STATUS_SUCCESS is returned if all processor are successfully virtualized.
    // When any error occurs while virtualizing processors, this function does
    // not attempt to virtualize the rest of processor. Therefore, only part of
    // processors on the system may have been virtualized on error. In this case,
    // it is a caller's responsibility to clean-up (de-virtualize) such
    // processors.
    //
    status = ExecuteOnEachProcessor(VirtualizeProcessor,
                                    sharedVpData,
                                    &numOfProcessorsCompleted);
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("ExecuteOnEachProcessor failed : %08x", status);
        goto Exit;
    }

    LOGGING_LOG_INFO("The all processors have been virtualized.");

Exit:
    if (!NT_SUCCESS(status))
    {
        //
        // On failure, after successful allocation of shared data.
        //
        if (numOfProcessorsCompleted != 0)
        {
            //
            // If one or more processors have already been virtualized,
            // de-virtualize any of those processors, and free shared data.
            //
            NT_ASSERT(sharedVpData != nullptr);
            DevirtualizeAllProcessors();
        }
        else
        {
            //
            // If none of processors has not been virtualized, simply free
            // shared data.
            //
            if (sharedVpData != nullptr)
            {
                if (sharedVpData->MsrPermissionsMap != nullptr)
                {
                    FreeContiguousMemory(sharedVpData->MsrPermissionsMap);
                }
                FreePageAlingedPhysicalMemory(sharedVpData);
            }
        }
    }
    return status;
}
