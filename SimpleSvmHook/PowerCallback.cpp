/*!
    @file PowerCallback.cpp

    @brief Power callback functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "PowerCallback.hpp"
#include "Common.hpp"
#include "Virtualization.hpp"

//
// A power state callback handle.
//
static PVOID g_PowerCallbackRegistration;

/*!
    @brief PowerState callback routine.

    @details This function de-virtualize all processors when the system is
        exiting system power state S0 (ie, the system is about to sleep etc),
        and virtualize all processors when the system has just reentered S0 (ie,
        the system has resume from sleep etc).

        Those operations are required because virtualization is cleared during
        sleep.

        For the meanings of parameters, see ExRegisterCallback in MSDN.

    @param[in] CallbackContext - Unused.

    @param[in] Argument1 - A PO_CB_XXX constant value.

    @param[in] Argument2 - A value of TRUE or FALSE.
 */
_Function_class_(CALLBACK_FUNCTION)
static
VOID
PowerCallbackRoutine (
    _In_opt_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    UNREFERENCED_PARAMETER(CallbackContext);

    //
    // This function can be called at at IRQL<=DISPATCH_LEVEL.
    // ----
    // When the specified condition occurs, the system calls the registered
    // callback routine at IRQL<=DISPATCH_LEVEL.
    // ----
    //
    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // PO_CB_SYSTEM_STATE_LOCK of Argument1 indicates that a system power state
    // change is imminent.
    //
    if (Argument1 != reinterpret_cast<PVOID>(PO_CB_SYSTEM_STATE_LOCK))
    {
        goto Exit;
    }

    //
    // FIXME: The VirtualizeAllProcessors and DevirtualizeAllProcessors
    // functions can only be executed at below DISPATCH_LEVEL. Fix will be
    // refactoring the ExecuteOnEachProcessor to run at DISPATCH_LEVEL (which
    // will manually iterate the all processors and queue DPCs), and make
    // the DevirtualizeAllProcessors function executable at DISPATCH_LEVEL
    // using that. The VirtualizeAllProcessors function could be the same but
    // it is also an option to queue an work item which calls it at
    // PASSIVE_LEVEL.
    //
    // It is worth noting that PO_CB_SYSTEM_STATE_LOCK callback appears to
    // happen only at the below DISPATCH_LEVEL by looking at the use of
    // nt!ExCbPowerState.
    //
    if (KeGetCurrentIrql() == DISPATCH_LEVEL)
    {
        NT_ASSERT(FALSE);
        LOGGING_LOG_WARN("Power callback invoked at DISPATCH_LEVEL."
                         " Unable to handle it.");
        goto Exit;
    }

    if (Argument2 != FALSE)
    {
        //
        // The system has just reentered S0. Re-virtualize all processors.
        //
        NT_VERIFY(NT_SUCCESS(VirtualizeAllProcessors()));
    }
    else
    {
        //
        // The system is about to exit system power state S0. De-virtualize all
        // processors.
        //
        DevirtualizeAllProcessors();
    }

Exit:
    return;
}

/*!
    @brief Registers the power callback.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
NTSTATUS
InitializePowerCallback (
    VOID
    )
{
    NTSTATUS status;
    UNICODE_STRING objectName;
    OBJECT_ATTRIBUTES objectAttributes;
    PCALLBACK_OBJECT callbackObject;
    PVOID callbackRegistration;

    PAGED_CODE();

    callbackRegistration = nullptr;

    //
    // Registers a power state callback (SvPowerCallbackRoutine) to handle
    // system sleep and resume to manage virtualization state.
    //
    // First, opens the \Callback\PowerState callback object provides
    // notification regarding power state changes. This is a system defined
    // callback object that was already created by Windows. To open a system
    // defined callback object, the Create parameter of ExCreateCallback must be
    // FALSE (and AllowMultipleCallbacks is ignore when the Create parameter is
    // FALSE).
    //
    objectName = RTL_CONSTANT_STRING(L"\\Callback\\PowerState");
    objectAttributes = RTL_CONSTANT_OBJECT_ATTRIBUTES(&objectName,
                                                      OBJ_CASE_INSENSITIVE);
    status = ExCreateCallback(&callbackObject, &objectAttributes, FALSE, TRUE);
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("ExCreateCallback failed : %08x", status);
        goto Exit;
    }

    //
    // Then, registers our callback. The open callback object must be
    // dereferenced.
    //
    callbackRegistration = ExRegisterCallback(callbackObject,
                                              PowerCallbackRoutine,
                                              nullptr);
    ObDereferenceObject(callbackObject);
    if (callbackRegistration == nullptr)
    {
        LOGGING_LOG_ERROR("ExRegisterCallback failed");
        status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    //
    // On success, save the registration handle for un-registration.
    //
    status = STATUS_SUCCESS;
    g_PowerCallbackRegistration = callbackRegistration;

Exit:
    if (!NT_SUCCESS(status))
    {
        if (callbackRegistration != nullptr)
        {
            ExUnregisterCallback(callbackRegistration);
        }
    }
    return status;
}

/*!
    @brief Unregisters the power callback if registered.
 */
SIMPLESVMHOOK_PAGED
_Use_decl_annotations_
VOID
CleanupPowerCallback (
    VOID
    )
{
    PAGED_CODE();

    //
    // Unregister the power state callback.
    //
    ExUnregisterCallback(g_PowerCallbackRegistration);
}
