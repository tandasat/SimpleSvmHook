/*!
    @file Logging.hpp

    @brief Declares interfaces to logging functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <fltKernel.h>

#define LOGGING_INIT  __declspec(code_seg("INIT"))
#define LOGGING_PAGED __declspec(code_seg("PAGE"))

EXTERN_C_START

/*!
    @brief Logs a message as respective severity.

    @details Debug prints or buffers a log message with information about current
        execution context such as time, PID and TID as respective severity. Here
        are some guidelines to decide which level is appropriate:
        @li DEBUG: info for only developers.
        @li INFO: info for all users.
        @li WARN: info may require some attention but does not prevent the program
            working properly.
        @li ERROR: info about issues may stop the program working properly.

        A message should not exceed 512 bytes after all string construction is
        done; otherwise this macro fails to log and returns non STATUS_SUCCESS.

    @param[in] Format - A format string.

    @return STATUS_SUCCESS on success.
*/
#define LOGGING_LOG_DEBUG(Format, ...) \
    LogpPrint(k_LogpLevelDebug, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG
*/
#define LOGGING_LOG_INFO(Format, ...) \
    LogpPrint(k_LogpLevelInfo, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG
*/
#define LOGGING_LOG_WARN(Format, ...) \
    LogpPrint(k_LogpLevelWarn, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG
*/
#define LOGGING_LOG_ERROR(Format, ...) \
    LogpPrint(k_LogpLevelError, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @brief Buffers a message as respective severity.

    @details Buffers the log to buffer and neither calls DbgPrint() nor writes
        to a file. It is strongly recommended to use it when a status of a
        system is not expectable in order to avoid system instability.

    @param[in] Format - A format string.

    @return STATUS_SUCCESS on success.

    @see LOGGING_LOG_DEBUG
*/
#define LOGGING_LOG_DEBUG_SAFE(Format, ...) \
    LogpPrint(k_LogpLevelDebug | k_LogpLevelOptSafe, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG_SAFE
*/
#define LOGGING_LOG_INFO_SAFE(Format, ...) \
    LogpPrint(k_LogpLevelInfo | k_LogpLevelOptSafe, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG_SAFE
*/
#define LOGGING_LOG_WARN_SAFE(Format, ...) \
    LogpPrint(k_LogpLevelWarn | k_LogpLevelOptSafe, __FUNCTION__, (Format), __VA_ARGS__)

/*!
    @see LOGGING_LOG_DEBUG_SAFE
*/
#define LOGGING_LOG_ERROR_SAFE(Format, ...) \
    LogpPrint(k_LogpLevelError | k_LogpLevelOptSafe, __FUNCTION__, (Format),  __VA_ARGS__)

//
// Save this log to buffer and not try to write to a log file.
//
static const ULONG k_LogpLevelOptSafe = 0x1;

//
// Bit mask for DEBUG, INFO, WARN and ERROR level logs respectively.
//
static const ULONG k_LogpLevelDebug = 0x10;
static const ULONG k_LogpLevelInfo = 0x20;
static const ULONG k_LogpLevelWarn = 0x40;
static const ULONG k_LogpLevelError = 0x80;

//
// For InitializeLogging(). Enables all levels of logs.
//
static const ULONG k_LogPutLevelDebug =
    k_LogpLevelError | k_LogpLevelWarn | k_LogpLevelInfo | k_LogpLevelDebug;

//
// For InitializeLogging(). Enables ERROR, WARN and INFO levels of logs.
//
static const ULONG k_LogPutLevelInfo =
    k_LogpLevelError | k_LogpLevelWarn | k_LogpLevelInfo;

//
// For InitializeLogging(). Enables ERROR and WARN levels of logs.
//
static const ULONG k_LogPutLevelWarn = k_LogpLevelError | k_LogpLevelWarn;

//
// For InitializeLogging(). Enables an ERROR level of logs.
//
static const ULONG k_LogPutLevelError = k_LogpLevelError;

//
// For InitializeLogging(). Disables all levels of logs.
//
static const ULONG k_LogPutLevelDisable = 0x0;

//
// For InitializeLogging(). Do not log a current time.
//
static const ULONG k_LogOptDisableTime = 0x100;

//
// For InitializeLogging(). Do not log a current function name.
//
static const ULONG k_LogOptDisableFunctionName = 0x200;

//
// For InitializeLogging(). Do not log a current processor number.
//
static const ULONG k_LogOptDisableProcessorNumber = 0x400;

//
// For InitializeLogging(). Do not log to debug buffer.
//
static const ULONG k_LogOptDisableDbgPrint = 0x800;

LOGGING_INIT
_Check_return_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
InitializeLogging (
    _In_ ULONG Flag,
    _In_opt_ PCWSTR LogFilePath,
    _Out_ PBOOLEAN ReinitRequired
    );

LOGGING_INIT
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LogRegisterReinitialization (
    _In_ PDRIVER_OBJECT DriverObject
    );

LOGGING_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LogIrpShutdownHandler (
    VOID
    );

LOGGING_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
CleanupLogging (
    VOID
    );

NTSTATUS
LogpPrint (
    _In_ ULONG Level,
    _In_ PCSTR FunctionName,
    _In_ _Printf_format_string_ PCSTR Format,
    ...
    );

EXTERN_C_END
