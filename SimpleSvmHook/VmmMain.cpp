/*!
    @file VmmMain.cpp

    @brief VMM code to handle #VMEXIT.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "VmmMain.hpp"
#include "Common.hpp"
#include "x86_64.hpp"
#include "Svm.hpp"
#include "HookVmmCommon.hpp"

/*!
    @brief Injects #GP with 0 of error code into the guest.

    @param[inout] VpData - The address of per processor data.
 */
static
VOID
InjectGeneralProtectionException (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData
    )
{
    EVENTINJ event;

    //
    // Inject #GP(vector = 13, type = 3 = exception) with a valid error code.
    // An error code are always zero. See "#GP-General-Protection Exception
    // (Vector 13)" for details about the error code.
    //
    event.AsUInt64 = 0;
    event.Fields.Vector = 13;
    event.Fields.Type = 3;
    event.Fields.ErrorCodeValid = 1;
    event.Fields.Valid = 1;
    VpData->GuestVmcb.ControlArea.EventInj = event.AsUInt64;
}

/*!
    @brief Handles #VMEXIT due to execution of the CPUID instructions.

    @details This function returns unmodified results of the CPUID instruction,
        except for few cases to indicate presence of the hypervisor, and to
        process an unload request.

        CPUID leaf 0x40000000 and 0x40000001 return modified values to conform
        to the hypervisor interface to some extent. See "Requirements for
        implementing the Microsoft Hypervisor interface"
        https://msdn.microsoft.com/en-us/library/windows/hardware/Dn613994(v=vs.85).aspx
        for details of the interface.

    @param[inout] VpData - The address of per processor data.

    @param[inout] GuestContext - The address of the guest GPRs.
 */
static
VOID
HandleCpuid (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    int registers[4];   // EAX, EBX, ECX, and EDX
    int leaf, subLeaf;
    SEGMENT_ATTRIBUTE attribute;

    //
    // Execute CPUID as requested.
    //
    leaf = static_cast<int>(GuestContext->VpRegs->Rax);
    subLeaf = static_cast<int>(GuestContext->VpRegs->Rcx);
    __cpuidex(registers, leaf, subLeaf);

    switch (leaf)
    {
    case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS:
        //
        // Indicate presence of a hypervisor by setting the bit that are
        // reserved for use by hypervisor to indicate guest status. See "CPUID
        // Fn0000_0001_ECX Feature Identifiers".
        //
        registers[3] |= CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;
        break;

    case CPUID_HV_VENDOR_AND_MAX_FUNCTIONS:
        //
        // Return a maximum supported hypervisor CPUID leaf range and a vendor
        // ID signature as required by the spec.
        //
        registers[0] = CPUID_HV_MAX;
        registers[1] = 'pmiS';  // "SimpleSvm   "
        registers[2] = 'vSel';
        registers[3] = '   m';
        break;

    case CPUID_HV_INTERFACE:
        //
        // Return non Hv#1 value. This indicate that our hypervisor does NOT
        // conform to the Microsoft hypervisor interface.
        //
        registers[0] = '0#vH';  // Hv#0
        registers[1] = registers[2] = registers[3] = 0;
        break;

    case CPUID_LEAF_SIMPLE_SVM_CALL:
        //
        // Only accept VMCALLs from the kernel-mode.
        //
        attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
        if (attribute.Fields.Dpl != DPL_SYSTEM)
        {
            break;
        }

        switch (subLeaf)
        {
        case CPUID_SUBLEAF_UNLOAD_SIMPLE_SVM:
            GuestContext->ExitVm = TRUE;
            break;
        case CPUID_SUBLEAF_ENABLE_HOOKS:
            EnableHooks(VpData->HookData);
            break;
        case CPUID_SUBLEAF_DISABLE_HOOKS:
            DisableHooks(VpData->HookData);
            break;
        default:
            NT_ASSERT(FALSE);
            break;
        }

    default:
        break;
    }

    //
    // Update guest's GPRs with results.
    //
    GuestContext->VpRegs->Rax = registers[0];
    GuestContext->VpRegs->Rbx = registers[1];
    GuestContext->VpRegs->Rcx = registers[2];
    GuestContext->VpRegs->Rdx = registers[3];

    //
    // Then, advance RIP to "complete" the instruction.
    //
    VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
}

/*!
    @brief Handles #VMEXIT due to execution of the WRMSR and RDMSR instructions.

    @details This protects EFER.SVME from being cleared by the guest by
        injecting #GP when it is about to be cleared.

    @param[inout] VpData - The address of per processor data.

    @param[inout] GuestContext - The address of the guest GPRs.
 */
static
VOID
HandleMsrAccess (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    UINT64 writeValueLow, writeValueHi, writeValue;

    //
    // #VMEXIT should only occur on write accesses to IA32_MSR_EFER. 1 of
    // ExitInfo1 indicates a write access.
    //
    NT_ASSERT(GuestContext->VpRegs->Rcx == IA32_MSR_EFER);
    NT_ASSERT(VpData->GuestVmcb.ControlArea.ExitInfo1 != 0);

    writeValueLow = GuestContext->VpRegs->Rax & MAXUINT32;
    if ((writeValueLow & EFER_SVME) == 0)
    {
        //
        // Inject #GP if the guest attempts to clear the SVME bit. Protection of
        // this bit is required because clearing the bit while guest is running
        // leads to undefined behavior.
        //
        InjectGeneralProtectionException(VpData);
    }

    //
    // Otherwise, update the MSR as requested. Important to note that the value
    // should be checked not to allow any illegal values, and inject #GP as
    // needed. Otherwise, the hypervisor attempts to resume the guest with an
    // illegal EFER and immediately receives #VMEXIT due to VMEXIT_INVALID,
    // which in our case, results in a bug check. See "Extended Feature Enable
    // Register (EFER)" for what values are allowed.
    //
    // This code does not implement the check intentionally, for simplicity.
    //
    writeValueHi = GuestContext->VpRegs->Rdx & MAXUINT32;
    writeValue = writeValueHi << 32 | writeValueLow;
    VpData->GuestVmcb.StateSaveArea.Efer = writeValue;

    //
    // Then, advance RIP to "complete" the instruction.
    //
    VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
}

/*!
    @brief Handles #VMEXIT due to execution of the VMRUN instruction.

    @details This function always injects #GP to the guest.

    @param[inout] VpData - The address of per processor data.

    @param[inout] GuestContext - The address of the guest GPRs.
 */
static
VOID
HandleVmrun (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_CONTEXT GuestContext
    )
{
    UNREFERENCED_PARAMETER(GuestContext);

    InjectGeneralProtectionException(VpData);
}

/*!
    @brief C-level entry point of the host code called from SvLaunchVm.

    @details This function loads save host state first, and then, handles
        #VMEXIT which may or may not change guest's state via VpData or
        GuestRegisters.

        Interrupts are disabled when this function is called due to the cleared
        GIF. Not all host state are loaded yet, so do it with the VMLOAD
        instruction.

        If the #VMEXIT handler detects a request to unload the hypervisor,
        this function loads guest state, disables SVM and returns to execution
        flow where the #VMEXIT triggered.

    @param[inout] VpData - The address of per processor data.

    @param[inout] GuestRegisters - The address of the guest GPRs.

    @return TRUE when virtualization is terminated; otherwise FALSE.
 */
EXTERN_C
BOOLEAN
NTAPI
HandleVmExit (
    _Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
    _Inout_ PGUEST_REGISTERS GuestRegisters
    )
{
    GUEST_CONTEXT guestContext;
    KIRQL oldIrql;

    //
    // Load some host state that are not loaded on #VMEXIT.
    //
    __svm_vmload(VpData->HostStackLayout.HostVmcbPa);

    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);

    PERFORMANCE_MEASURE_THIS_SCOPE();

    //
    // Raise the IRQL to the DISPATCH_LEVEL level. This has no actual effect since
    // interrupts are disabled at #VMEXI but warrants bug check when some of
    // kernel API that are not usable on this context is called with Driver
    // Verifier. This protects developers from accidentally writing such #VMEXIT
    // handling code. This should actually raise IRQL to HIGH_LEVEL to represent
    // this running context better, but our Logger code is not designed to run at
    // that level unfortunatelly. Finally, note that this API is a thin wrapper
    // of mov-to-CR8 on x64 and safe to call on this context.
    //
    oldIrql = KeGetCurrentIrql();
    if (oldIrql < DISPATCH_LEVEL)
    {
        KeRaiseIrqlToDpcLevel();
    }

    //
    // Guest's RAX is overwritten by the host's value on #VMEXIT and saved in
    // the VMCB instead. Reflect the guest RAX to the context.
    //
    GuestRegisters->Rax = VpData->GuestVmcb.StateSaveArea.Rax;

    guestContext.VpRegs = GuestRegisters;
    guestContext.ExitVm = FALSE;

    //
    // Handle #VMEXIT according with its reason.
    //
    switch (VpData->GuestVmcb.ControlArea.ExitCode)
    {
    case VMEXIT_CPUID:
        HandleCpuid(VpData, &guestContext);
        break;

    case VMEXIT_MSR:
        HandleMsrAccess(VpData, &guestContext);
        break;

    case VMEXIT_VMRUN:
        HandleVmrun(VpData, &guestContext);
        break;

    case VMEXIT_EXCEPTION_BP:
        HandleBreakPointException(&VpData->GuestVmcb, VpData->HookData);
        break;

    case VMEXIT_NPF:
        HandleNestedPageFault(&VpData->GuestVmcb, VpData->HookData);
        break;

    default:
        SIMPLESVMHOOK_BUG_CHECK();
    }

    //
    // Again, no effect to change IRQL but restoring it here since a #VMEXIT
    // handler where the developers most likely call the kernel API inadvertently
    // is already executed.
    //
    if (oldIrql < DISPATCH_LEVEL)
    {
        KeLowerIrql(oldIrql);
    }

    //
    // Cleanup our hypervisor if requested.
    //
    if (guestContext.ExitVm != FALSE)
    {
        NT_ASSERT(VpData->GuestVmcb.ControlArea.ExitCode == VMEXIT_CPUID);

        //
        // Set return values of CPUID instruction as follows:
        //  RBX     = An address to return
        //  RCX     = A stack pointer to restore
        //  EDX:EAX = An address of per processor data to be freed by the caller
        //
        guestContext.VpRegs->Rax = reinterpret_cast<UINT64>(VpData) & MAXUINT32;
        guestContext.VpRegs->Rbx = VpData->GuestVmcb.ControlArea.NRip;
        guestContext.VpRegs->Rcx = VpData->GuestVmcb.StateSaveArea.Rsp;
        guestContext.VpRegs->Rdx = reinterpret_cast<UINT64>(VpData) >> 32;

        //
        // Load guest state (currently host state is loaded).
        //
        __svm_vmload(MmGetPhysicalAddress(&VpData->GuestVmcb).QuadPart);

        //
        // Set the global interrupt flag (GIF) but still disable interrupts by
        // clearing IF. GIF must be set to return to the normal execution, but
        // interruptions are not desirable until SVM is disabled as it would
        // execute random kernel-code in the host context.
        //
        _disable();
        __svm_stgi();

        //
        // Disable SVM, and restore the guest RFLAGS. This may enable interrupts.
        // Some of arithmetic flags are destroyed by the subsequent code.
        //
        __writemsr(IA32_MSR_EFER, __readmsr(IA32_MSR_EFER) & ~EFER_SVME);
        __writeeflags(VpData->GuestVmcb.StateSaveArea.Rflags);
        goto Exit;
    }

    //
    // Reflect potentially updated guest's RAX to VMCB. Again, unlike other GPRs,
    // RAX is loaded from VMCB on VMRUN.
    //
    VpData->GuestVmcb.StateSaveArea.Rax = guestContext.VpRegs->Rax;

Exit:
    NT_ASSERT(VpData->HostStackLayout.Reserved1 == MAXUINT64);
    return guestContext.ExitVm;
}