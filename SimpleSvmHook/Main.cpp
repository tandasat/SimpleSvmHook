/*!
    @file Main.cpp

    @brief Driver load and unload routines.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "Common.hpp"
#include "Virtualization.hpp"
#include "PowerCallback.hpp"
#include "HookKernelCommon.hpp"

SIMPLESVMHOOK_INIT EXTERN_C DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD DriverUnload;

/*!
    @brief An entry point of this driver.

    @param[in] DriverObject - A driver object.

    @param[in] RegistryPath - Unused.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
SIMPLESVMHOOK_INIT
_Use_decl_annotations_
EXTERN_C
NTSTATUS
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    BOOLEAN needLogReinitialization;
    BOOLEAN loggingInited, performanceInited, pcInited, hookInited;

    UNREFERENCED_PARAMETER(RegistryPath);

    SIMPLESVMHOOK_DEBUG_BREAK();

    loggingInited = FALSE;
    performanceInited = FALSE;
    pcInited = FALSE;
    hookInited = FALSE;

    DriverObject->DriverUnload = DriverUnload;

    //
    // Opts-in no-execute (NX) nonpaged pool for security when available. By
    // defining POOL_NX_OPTIN as 1 and calling this function, nonpaged pool
    // allocation by the ExAllocatePool family with the NonPagedPool flag
    // automatically allocates NX nonpaged pool on Windows 8 and later versions
    // of Windows, while on Windows 7 where NX nonpaged pool is unsupported,
    // executable nonpaged pool is returned as usual.
    //
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    //
    // Initialize log functions
    //
    status = InitializeLogging(k_LogPutLevelDebug | k_LogOptDisableFunctionName,
                               L"\\SystemRoot\\SimpleSvmHook.log",
                               &needLogReinitialization);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    loggingInited = TRUE;

    //
    // Initialize the performance measurement facility.
    //
    status = InitializePerformance();
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializePerformance failed : %08x", status);
        goto Exit;
    }
    performanceInited = TRUE;

    //
    // Register the power callback.
    //
    status = InitializePowerCallback();
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializePowerCallback failed : %08x", status);
        goto Exit;
    }
    pcInited = TRUE;

    //
    // Initialize hook related general data structures.
    //
    status = InitializeHook();
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializeHook failed : %08x", status);
        goto Exit;
    }
    hookInited = TRUE;

    //
    // Virtualize all processors on the system.
    //
    status = VirtualizeAllProcessors();
    if (!NT_SUCCESS(status))
    {
        LOGGING_LOG_ERROR("InitializeVirtualProcessors failed : %08x", status);
        goto Exit;
    }

    NT_ASSERT(AreAllHooksInvisible() != FALSE);

    //
    // Register re-initialization for the log functions if needed.
    //
    if (needLogReinitialization != FALSE)
    {
        LogRegisterReinitialization(DriverObject);
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        if (hookInited != FALSE)
        {
            CleanupHook();
        }
        if (pcInited != FALSE)
        {
            CleanupPowerCallback();
        }
        if (performanceInited != FALSE)
        {
            CleanupPerformance();
        }
        if (loggingInited != FALSE)
        {
            CleanupLogging();
        }
    }
    return status;
}

/*!
    @brief      Driver unload callback.

    @details    This function de-virtualize all processors on the system.

    @param[in]  DriverObject - Unused.
 */
_Use_decl_annotations_
SIMPLESVMHOOK_PAGED
static
VOID
DriverUnload (
    PDRIVER_OBJECT DriverObject
    )
{
    LARGE_INTEGER interval;

    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();

    SIMPLESVMHOOK_DEBUG_BREAK();

    //
    // De-virtualize all processors on the system.
    //
    DevirtualizeAllProcessors();
    CleanupHook();
    CleanupPowerCallback();
    CleanupPerformance();
    CleanupLogging();

    //
    // Wait for a while before unload the driver. This is workaround for that
    // there may be a thread that will eventually return to our handler function
    // because they called one of hooked functions. This sleep is normally enough
    // to let those threads return to and complete execution of our hook handlers.
    //
    // The right resolution (if there is anythnig *right* about doing hook)
    // would be not to unload this driver on the fly and to require reboot to
    // safely uninstall this driver.
    //
    interval.QuadPart = -(10000 * 1000);  // 1000 msec
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
