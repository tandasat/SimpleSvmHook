/*!
    @file Logging.cpp

    @brief Implements interfaces to logging functions.

    @details This file is migrated from an old codebase and does not always
        align with a coding style used in the rest of files.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "Logging.hpp"
#define NTSTRSAFE_NO_CB_FUNCTIONS
#include <ntstrsafe.h>

#pragma prefast(disable : 30030)
#pragma prefast(disable : __WARNING_ERROR, "This is completely bogus.")

EXTERN_C_START

//
// A size for log buffer in NonPagedPool. Two buffers are allocated with this
// size. Exceeded logs are ignored silently. Make it bigger if a buffered log
// size often reach this size.
//
static const ULONG k_LogpBufferSizeInPages = 16;

//
// An actual log buffer size in bytes.
//
static const ULONG k_LogpBufferSize = PAGE_SIZE * k_LogpBufferSizeInPages;

//
// A size that is usable for logging. Minus one because the last byte is kept
// for \0.
//
static const ULONG k_LogpBufferUsableSize = k_LogpBufferSize - 1;

//
// An interval to flush buffered log entries into a log file.
//
static const ULONG k_LogpLogFlushIntervalMsec = 50;

//
// The pool tag.
//
static const ULONG k_LogpPoolTag = 'rgoL';

//
// The mask value to indicate the message was already printed out to debug buffer.
//
static const UCHAR k_MessagePrinted = 0x80;

typedef struct _LOG_BUFFER_INFO
{
    //
    // A pointer to buffer currently used. It is either LogBuffer1 or LogBuffer2.
    //
    volatile PSTR LogBufferHead;

    //
    // A pointer to where the next log should be written.
    //
    volatile PSTR LogBufferTail;

    PSTR LogBuffer1;
    PSTR LogBuffer2;

    //
    // Holds the biggest buffer usage to determine a necessary buffer size.
    //
    SIZE_T LogMaxUsage;

    HANDLE LogFileHandle;
    KSPIN_LOCK SpinLock;
    ERESOURCE Resource;
    BOOLEAN ResourceInitialized;
    volatile BOOLEAN BufferFlushThreadShouldBeAlive;
    volatile BOOLEAN BufferFlushThreadStarted;
    HANDLE BufferFlushThreadHandle;
    WCHAR LogFilePath[200];
} LOG_BUFFER_INFO, *PLOG_BUFFER_INFO;

NTKERNELAPI
PCHAR
NTAPI
PsGetProcessImageFileName (
    _In_ PEPROCESS Process
    );

static DRIVER_REINITIALIZE LogpReinitializationRoutine;
static KSTART_ROUTINE LogpBufferFlushThreadRoutine;

//
// The enabled log level.
//
static ULONG g_LogpDebugFlag;

//
// The log buffer.
//
static LOG_BUFFER_INFO g_LogpLogBufferInfo;

//
// Whether the driver is verified by Driver Verifier.
//
static BOOLEAN g_LogpDriverVerified;

/*!
    @brief Tests if the printed bit is on.

    @param[in] Message - The log message to test.

    @return TRUE if the message has the printed bit; or FALSE.
 */
static
_Check_return_
BOOLEAN
LogpIsPrinted (
    _In_ PCSTR Message
    )
{
    return BooleanFlagOn(Message[0], k_MessagePrinted);
}

/*!
    @brief Marks the Message as it is already printed out, or clears the printed
        bit and restores it to the original.

    @param[in] Message - The log message to set the bit.

    @param[in] SetBit - TRUE to set the bit; FALSE t clear the bit.
 */
static
VOID
LogpSetPrintedBit (
    _In_ PSTR Message,
    _In_ BOOLEAN SetBit
    )
{
    if (SetBit != FALSE)
    {
        SetFlag(Message[0], k_MessagePrinted);
    }
    else
    {
        ClearFlag(Message[0], k_MessagePrinted);
    }
}

/*!
    @brief Calls DbgPrintEx() while converting \r\n to \n\0.

    @param[in] Message - The log message to print out.
 */
static
VOID
LogpDoDbgPrint (
    _In_ PSTR Message
    )
{
    size_t locationOfCr;

    if (BooleanFlagOn(g_LogpDebugFlag, k_LogOptDisableDbgPrint))
    {
        goto Exit;
    }

    locationOfCr = strlen(Message) - 2;
    Message[locationOfCr] = '\n';
    Message[locationOfCr + 1] = ANSI_NULL;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "%s", Message);

Exit:
    return;
}

/*!
    @brief Sleep the current thread's execution for milliseconds.

    @param[in] Millisecond - The duration to sleep in milliseconds.
 */
LOGGING_PAGED
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LogpSleep (
    _In_ LONG Millisecond
    )
{
    LARGE_INTEGER interval;

    PAGED_CODE();

    interval.QuadPart = -(10000ll * Millisecond);
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
/*!
    @brief Logs the current log entry to and flush the log file.

    @param[in] Message - The log message to buffer.

    @param[in] Info - Log buffer information.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
LogpWriteMessageToFile (
    _In_ PCSTR Message,
    _In_ const LOG_BUFFER_INFO* Info
    )
{
    NTSTATUS status;
    IO_STATUS_BLOCK ioStatus;

    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    status = ZwWriteFile(Info->LogFileHandle,
                         nullptr,
                         nullptr,
                         nullptr,
                         &ioStatus,
                         const_cast<PSTR>(Message),
                         static_cast<ULONG>(strlen(Message)),
                         nullptr,
                         nullptr);
    if (!NT_SUCCESS(status))
    {
        //
        // It could happen when you did not register IRP_SHUTDOWN and call
        // LogIrpShutdownHandler() and the system tried to log to a file after
        // a file system was unmounted.
        //
        goto Exit;
    }

    status = ZwFlushBuffersFile(Info->LogFileHandle, &ioStatus);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

Exit:
    return status;
}

/*!
    @brief Buffers the log entry to the log buffer.

    @param[in] Message - The log message to buffer.

    @param[in,out] Info - Log buffer information.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_Check_return_
NTSTATUS
LogpBufferMessage (
    _In_ PCSTR Message,
    _Inout_ PLOG_BUFFER_INFO Info
    )
{
    NTSTATUS status;
    KLOCK_QUEUE_HANDLE lockHandle;
    KIRQL oldIrql;
    SIZE_T usedBufferSize;

    //
    // Acquire a spin lock to add the log safely.
    //
    oldIrql = KeGetCurrentIrql();
    if (oldIrql < DISPATCH_LEVEL)
    {
        KeAcquireInStackQueuedSpinLock(&Info->SpinLock, &lockHandle);
    }
    else
    {
        KeAcquireInStackQueuedSpinLockAtDpcLevel(&Info->SpinLock, &lockHandle);
    }
    NT_ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    //
    // Copy the current log to the buffer.
    //
    usedBufferSize = Info->LogBufferTail - Info->LogBufferHead;
    status = RtlStringCchCopyA(const_cast<PSTR>(Info->LogBufferTail),
                               k_LogpBufferUsableSize - usedBufferSize,
                               Message);

    //
    // Update Info.LogMaxUsage if necessary.
    //
    if (NT_SUCCESS(status))
    {
        size_t messageLength;

        messageLength = strlen(Message) + 1;
        Info->LogBufferTail += messageLength;
        usedBufferSize += messageLength;
        if (usedBufferSize > Info->LogMaxUsage)
        {
            Info->LogMaxUsage = usedBufferSize;
        }
    }
    else
    {
        Info->LogMaxUsage = k_LogpBufferSize;
    }
    *Info->LogBufferTail = ANSI_NULL;

    if (oldIrql < DISPATCH_LEVEL)
    {
        KeReleaseInStackQueuedSpinLock(&lockHandle);
    }
    else
    {
        KeReleaseInStackQueuedSpinLockFromDpcLevel(&lockHandle);
    }
    return status;
}

/*!
    @brief Processes all buffered log messages.

    @param[in,out] Info - Log buffer information.

    @details This function switches the current log buffer, saves the contents
        of old buffer to the log file, and prints them out as necessary. This
        function does not flush the log file, so code should call
        LogpWriteMessageToFile() or ZwFlushBuffersFile() later.
 */
static
_Check_return_
NTSTATUS
LogpFlushLogBuffer (
    _Inout_ PLOG_BUFFER_INFO Info
    )
{
    NTSTATUS status;
    KLOCK_QUEUE_HANDLE lockHandle;
    PSTR oldLogBuffer;
    IO_STATUS_BLOCK ioStatus;

    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    status = STATUS_SUCCESS;

    //
    // Enter a critical section and acquire a reader lock for Info in order to
    // write a log file safely.
    //
    ExEnterCriticalRegionAndAcquireResourceExclusive(&Info->Resource);

    //
    // Acquire a spin lock for Info.LogBuffer(s) in order to switch its head
    // safely.
    //
    KeAcquireInStackQueuedSpinLock(&Info->SpinLock, &lockHandle);
    oldLogBuffer = Info->LogBufferHead;
    if (oldLogBuffer[0] != ANSI_NULL)
    {
        Info->LogBufferHead = (oldLogBuffer == Info->LogBuffer1) ? Info->LogBuffer2
                                                                 : Info->LogBuffer1;
        Info->LogBufferHead[0] = ANSI_NULL;
        Info->LogBufferTail = Info->LogBufferHead;
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    //
    // Write all log entries in old log buffer.
    //
    for (PSTR currentLogEntry = oldLogBuffer; currentLogEntry[0]; /**/)
    {
        BOOLEAN printedOut;
        size_t entryLength;

        //
        // Check the printed bit and clear it.
        //
        printedOut = LogpIsPrinted(currentLogEntry);
        LogpSetPrintedBit(currentLogEntry, FALSE);

        entryLength = strlen(currentLogEntry);
        status = ZwWriteFile(Info->LogFileHandle,
                             nullptr,
                             nullptr,
                             nullptr,
                             &ioStatus,
                             currentLogEntry,
                             static_cast<ULONG>(entryLength),
                             nullptr,
                             nullptr);
        if (!NT_SUCCESS(status))
        {
            //
            // It could happen when you did not register IRP_SHUTDOWN and call
            // LogIrpShutdownHandler() and the system tried to log to a file after
            // a file system was unmounted.
            //
            NOTHING;
        }

        //
        // Print it out if requested and the Message is not already printed out
        //
        if (printedOut == FALSE)
        {
            LogpDoDbgPrint(currentLogEntry);
        }

        currentLogEntry += entryLength + 1;
    }
    oldLogBuffer[0] = ANSI_NULL;

    ExReleaseResourceAndLeaveCriticalRegion(&Info->Resource);
    return status;
}
/*!
    @brief Returns TRUE when a log file is opened.

    @param[in] Info - Log buffer information.

    @return TRUE when a log file is opened.
 */
static
_Check_return_
BOOLEAN
LogpIsLogFileActivated (
    _In_ const LOG_BUFFER_INFO* Info
    )
{
    BOOLEAN activated;

    if (Info->BufferFlushThreadShouldBeAlive != FALSE)
    {
        NT_ASSERT(Info->BufferFlushThreadHandle != nullptr);
        NT_ASSERT(Info->LogFileHandle != nullptr);
        activated = TRUE;
    }
    else
    {
        NT_ASSERT(Info->BufferFlushThreadHandle == nullptr);
        NT_ASSERT(Info->LogFileHandle == nullptr);
        activated = FALSE;
    }
    return activated;
}

/*!
    @brief Returns TRUE when a log file is enabled.

    @param[in] Info - Log buffer information.

    @return TRUE when a log file is enabled.
 */
static
_Check_return_
BOOLEAN
LogpIsLogFileEnabled (
    _In_ const LOG_BUFFER_INFO* Info
    )
{
    BOOLEAN enabled;

    if (Info->LogBuffer1 != nullptr)
    {
        NT_ASSERT(Info->LogBuffer2 != nullptr);
        NT_ASSERT(Info->LogBufferHead != nullptr);
        NT_ASSERT(Info->LogBufferTail != nullptr);
        enabled = TRUE;
    }
    else
    {
        NT_ASSERT(Info->LogBuffer2 == nullptr);
        NT_ASSERT(Info->LogBufferHead == nullptr);
        NT_ASSERT(Info->LogBufferTail == nullptr);
        enabled = FALSE;
    }
    return enabled;
}

/*!
    @brief Returns the function's base name.

    @param[in] FunctionName - The function name given by the __FUNCTION__ macro.

    @return The function's base name, for example, "MethodName" for
        "NamespaceName::ClassName::MethodName".
 */
static
_Check_return_
PCSTR
LogpFindBaseFunctionName (
    _In_ PCSTR FunctionName
    )
{
    PCSTR ptr;
    PCSTR name;

    name = ptr = FunctionName;
    while (*(ptr++) != ANSI_NULL)
    {
        if (*ptr == ':')
        {
            name = ptr + 1;
        }
    }
    return name;
}

/*!
    @brief Logs the entry according to Attribute and the thread condition.

    @param[in] Message - The log message to print or buffer.

    @param[in] Attribute - The bit mask indicating how this message should be
        printed out.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_Check_return_
NTSTATUS
LogpPut (
    _In_ PSTR Message,
    _In_ ULONG Attribute
    )
{
    NTSTATUS status;
    BOOLEAN callDbgPrint;
    PLOG_BUFFER_INFO info;

    status = STATUS_SUCCESS;

    callDbgPrint = ((!BooleanFlagOn(Attribute, k_LogpLevelOptSafe)) &&
                    (KeGetCurrentIrql() < CLOCK_LEVEL));

    //
    // Log the entry to a file or buffer.
    //
    info = &g_LogpLogBufferInfo;
    if (LogpIsLogFileEnabled(info) != FALSE)
    {
        //
        // Can we log it to a file now?
        //
        if (!BooleanFlagOn(Attribute, k_LogpLevelOptSafe) &&
            (KeGetCurrentIrql() == PASSIVE_LEVEL) &&
            (LogpIsLogFileActivated(info) != FALSE))
        {
#pragma warning(push)
#pragma warning(disable : __WARNING_INFERRED_IRQ_TOO_HIGH)
            if (KeAreAllApcsDisabled() == FALSE)
            {
                //
                // Yes, we can. Do it.
                //
                (VOID)LogpFlushLogBuffer(info);
                status = LogpWriteMessageToFile(Message, info);
            }
#pragma warning(pop)
        }
        else
        {
            //
            // No, we cannot. Set the printed bit if needed, and then buffer it.
            //
            if (callDbgPrint != FALSE)
            {
                LogpSetPrintedBit(Message, TRUE);
            }
            status = LogpBufferMessage(Message, info);
            LogpSetPrintedBit(Message, FALSE);
        }
    }

    //
    // Can it safely be printed?
    //
    if (callDbgPrint != FALSE)
    {
        LogpDoDbgPrint(Message);
    }
    return status;
}

/*!
    @brief Concatenates meta information such as the current time and a process
        ID to the user supplied log message.

    @param[in] Level - The level of this log message.

    @param[in] FunctionName - The name of the function originating this log message.

    @param[in] LogMessage - The user supplied log message.

    @param[out] LogBuffer - Buffer to store the concatenated message.

    @param[in] LogBufferLength - The size of buffer in characters.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
static
_Check_return_
NTSTATUS
LogpMakePrefix (
    _In_ ULONG Level,
    _In_ PCSTR FunctionName,
    _In_ PCSTR LogMessage,
    _Out_writes_z_(LogBufferLength) PSTR LogBuffer,
    _In_ SIZE_T LogBufferLength
    )
{
    NTSTATUS status;
    PCSTR levelString;
    CHAR time[20];
    CHAR functionName[50];
    CHAR processorNumber[10];

    switch (Level)
    {
    case k_LogpLevelDebug:
        levelString = "DBG\t";
        break;
    case k_LogpLevelInfo:
        levelString = "INF\t";
        break;
    case k_LogpLevelWarn:
        levelString = "WRN\t";
        break;
    case k_LogpLevelError:
        levelString = "ERR\t";
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    //
    // Want the current time?
    //
    if (!BooleanFlagOn(g_LogpDebugFlag, k_LogOptDisableTime))
    {
        TIME_FIELDS timeFields;
        LARGE_INTEGER systemTime, localTime;

        KeQuerySystemTime(&systemTime);
        ExSystemTimeToLocalTime(&systemTime, &localTime);
        RtlTimeToTimeFields(&localTime, &timeFields);

        status = RtlStringCchPrintfA(time,
                                     RTL_NUMBER_OF(time),
                                     "%02hd:%02hd:%02hd.%03hd\t",
                                     timeFields.Hour,
                                     timeFields.Minute,
                                     timeFields.Second,
                                     timeFields.Milliseconds);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }
    else
    {
        time[0] = ANSI_NULL;
    }

    //
    // Want the function name?
    //
    if (!BooleanFlagOn(g_LogpDebugFlag, k_LogOptDisableFunctionName))
    {
        PCSTR baseFunctionName;

        baseFunctionName = LogpFindBaseFunctionName(FunctionName);
        status = RtlStringCchPrintfA(functionName,
                                     RTL_NUMBER_OF(functionName),
                                     "%-40s\t",
                                     baseFunctionName);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }
    else
    {
        functionName[0] = ANSI_NULL;
    }

    //
    // Want the processor number?
    //
    if (!BooleanFlagOn(g_LogpDebugFlag, k_LogOptDisableProcessorNumber))
    {
        status = RtlStringCchPrintfA(processorNumber,
                                     RTL_NUMBER_OF(processorNumber),
                                     "#%lu\t",
                                     KeGetCurrentProcessorNumberEx(nullptr));
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }
    else
    {
        processorNumber[0] = ANSI_NULL;
    }

    //
    // It uses PsGetProcessId(PsGetCurrentProcess()) instead of
    // PsGetCurrentThreadProcessId() because the later sometimes returns
    // unwanted value, for example, PID == 4 while its image name is not
    // ntoskrnl.exe. The author is guessing that it is related to attaching
    // processes but not quite sure. The former way works as expected.
    //
    status = RtlStringCchPrintfA(LogBuffer,
                                 LogBufferLength,
                                 "%s%s%s%5Iu\t%5Iu\t%-15s\t%s%s\r\n",
                                 time,
                                 levelString,
                                 processorNumber,
                                 reinterpret_cast<ULONG_PTR>(PsGetProcessId(PsGetCurrentProcess())),
                                 reinterpret_cast<ULONG_PTR>(PsGetCurrentThreadId()),
                                 PsGetProcessImageFileName(PsGetCurrentProcess()),
                                 functionName,
                                 LogMessage);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

Exit:
    return status;
}

/*!
    @brief Terminates a log file related code.

    @param[in] Info - Log buffer information.
 */
LOGGING_PAGED
static
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
LogpCleanupBufferInfo (
    _In_ PLOG_BUFFER_INFO Info
    )
{
    NTSTATUS status;

    PAGED_CODE();

    //
    // Closing the log buffer flush thread.
    //
    if (Info->BufferFlushThreadHandle != nullptr)
    {
        Info->BufferFlushThreadShouldBeAlive = FALSE;
        status = ZwWaitForSingleObject(Info->BufferFlushThreadHandle, FALSE, nullptr);
        NT_ASSERT(NT_SUCCESS(status));
        ZwClose(Info->BufferFlushThreadHandle);
        Info->BufferFlushThreadHandle = nullptr;
    }

    //
    // Cleaning up other things.
    //
    if (Info->LogFileHandle != nullptr)
    {
        ZwClose(Info->LogFileHandle);
        Info->LogFileHandle = nullptr;
    }
    if (Info->LogBuffer2  != nullptr)
    {
        ExFreePoolWithTag(Info->LogBuffer2, k_LogpPoolTag);
        Info->LogBuffer2 = nullptr;
    }
    if (Info->LogBuffer1 != nullptr)
    {
        ExFreePoolWithTag(Info->LogBuffer1, k_LogpPoolTag);
        Info->LogBuffer1 = nullptr;
    }

    if (Info->ResourceInitialized != FALSE)
    {
        ExDeleteResourceLite(&Info->Resource);
        Info->ResourceInitialized = FALSE;
    }
}

/*!
    @brief Initializes a log file and starts a log buffer thread.

    @param[in,out] Info - Log buffer information.

    @param[out] ReinitRequired - A pointer to receive whether re-initialization
        is required.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
LOGGING_PAGED
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
LogpInitializeLogFile (
    _Inout_ PLOG_BUFFER_INFO Info,
    _Out_ PBOOLEAN ReinitRequired
    )
{
    NTSTATUS status;
    UNICODE_STRING logFilePathU;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatus;

    PAGED_CODE();

    *ReinitRequired = FALSE;

    if (Info->LogFileHandle != nullptr)
    {
        status = STATUS_SUCCESS;
        goto Exit;
    }

    //
    // Initialize a log file.
    //
    RtlInitUnicodeString(&logFilePathU, Info->LogFilePath);
    InitializeObjectAttributes(&objectAttributes,
                               &logFilePathU,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               nullptr,
                               nullptr);
    status = ZwCreateFile(&Info->LogFileHandle,
                          FILE_APPEND_DATA | SYNCHRONIZE,
                          &objectAttributes,
                          &ioStatus,
                          nullptr,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ,
                          FILE_OPEN_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                          nullptr,
                          0);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    //
    // Initialize a log buffer flush thread.
    //
    Info->BufferFlushThreadShouldBeAlive = TRUE;
    status = PsCreateSystemThread(&Info->BufferFlushThreadHandle,
                                  THREAD_ALL_ACCESS,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  LogpBufferFlushThreadRoutine,
                                  Info);
    if (!NT_SUCCESS(status))
    {
        ZwClose(Info->LogFileHandle);
        Info->LogFileHandle = nullptr;
        Info->BufferFlushThreadShouldBeAlive = FALSE;
        goto Exit;
    }

    //
    // Wait until the thread starts.
    //
    while (Info->BufferFlushThreadStarted == FALSE)
    {
        LogpSleep(100);
    }

Exit:
    if (status == STATUS_OBJECTID_NOT_FOUND)
    {
        *ReinitRequired = TRUE;
        status = STATUS_SUCCESS;
    }
    return status;
}

/*!
    @brief Initializes a log file related code such as a flushing thread.

    @param[in] LogFilePath - A log file path.

    @param[in,out] Info - Log buffer information.

    @param[out] ReinitRequired - A pointer to receive whether re-initialization
        is required.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
LOGGING_INIT
static
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
LogpInitializeBufferInfo (
    _In_ PCWSTR LogFilePath,
    _Inout_ PLOG_BUFFER_INFO Info,
    _Out_ PBOOLEAN ReinitRequired
    )
{
    NTSTATUS status;

    PAGED_CODE();

    *ReinitRequired = FALSE;

    KeInitializeSpinLock(&Info->SpinLock);

    status = RtlStringCchCopyW(Info->LogFilePath,
                               RTL_NUMBER_OF_FIELD(LOG_BUFFER_INFO, LogFilePath),
                               LogFilePath);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    status = ExInitializeResourceLite(&Info->Resource);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    Info->ResourceInitialized = TRUE;

    //
    // Allocate two log buffers on NonPagedPool.
    //
    Info->LogBuffer1 = reinterpret_cast<PSTR>(ExAllocatePoolWithTag(NonPagedPool,
                                                                    k_LogpBufferSize,
                                                                    k_LogpPoolTag));
    if (Info->LogBuffer1 == nullptr)
    {
        LogpCleanupBufferInfo(Info);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Info->LogBuffer2 = reinterpret_cast<PSTR>(ExAllocatePoolWithTag(NonPagedPool,
                                                                    k_LogpBufferSize,
                                                                    k_LogpPoolTag));
    if (Info->LogBuffer2 == nullptr)
    {
        LogpCleanupBufferInfo(Info);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    //
    // Initialize these buffers. For diagnostic, fill them with some
    // distinguishable bytes and then, ensure it is null-terminated.
    //
    RtlFillMemory(Info->LogBuffer1, k_LogpBufferSize, 0xff);
    Info->LogBuffer1[0] = ANSI_NULL;
    Info->LogBuffer1[k_LogpBufferSize - 1] = ANSI_NULL;

    RtlFillMemory(Info->LogBuffer2, k_LogpBufferSize, 0xff);
    Info->LogBuffer2[0] = ANSI_NULL;
    Info->LogBuffer2[k_LogpBufferSize - 1] = ANSI_NULL;

    //
    // Buffer should be used is LogBuffer1, and location should be written logs
    // is the head of the buffer.
    //
    Info->LogBufferHead = Info->LogBuffer1;
    Info->LogBufferTail = Info->LogBuffer1;

    status = LogpInitializeLogFile(Info, ReinitRequired);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    if (*ReinitRequired != FALSE)
    {
        LOGGING_LOG_INFO("The log file needs to be activated later.");
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        LogpCleanupBufferInfo(Info);
    }
    return status;
}

/*!
    @brief Initializes the log system.

    @param[in] Flag - A OR-ed Flag to control a log level and options. It is an
        OR-ed value of kLogPutLevel* and kLogOpt*. For example,
        k_LogPutLevelDebug | k_LogOptDisableFunctionName.

    @param[in] LogFilePath - A log file path.

    @param[out] ReinitRequired - A pointer to receive whether re-initialization
        is required. When this parameter returns TRUE, the caller must call
        LogRegisterReinitialization() to register re-initialization.

    @return STATUS_SUCCESS on success or else on failure.

    @details Allocates internal log buffers, initializes related resources,
        starts a log flush thread and creates a log file if requested. This
        function returns TRUE to ReinitRequired if a file-system is not
        initialized yet.
*/
LOGGING_INIT
_Use_decl_annotations_
NTSTATUS
InitializeLogging (
    ULONG Flag,
    PCWSTR LogFilePath,
    PBOOLEAN ReinitRequired
    )
{
    NTSTATUS status;

    PAGED_CODE();

    *ReinitRequired = FALSE;

    g_LogpDriverVerified = (MmIsDriverVerifyingByAddress(&InitializeLogging) != FALSE);

    g_LogpDebugFlag = Flag;

    //
    // Initialize a log file if a log file path is specified.
    //
    if (ARGUMENT_PRESENT(LogFilePath))
    {
        status = LogpInitializeBufferInfo(LogFilePath,
                                          &g_LogpLogBufferInfo,
                                          ReinitRequired);
        if (!NT_SUCCESS(status))
        {
            goto Exit;
        }
    }

    //
    // Test the log.
    //
    status = LOGGING_LOG_INFO("Logger was %sinitialized",
                              (*ReinitRequired != FALSE) ? "partially " : "");
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    if (g_LogpDriverVerified != FALSE)
    {
        LOGGING_LOG_WARN("Driver being verified. All *_SAFE logs will be dropped.");
    }

Exit:
    if (!NT_SUCCESS(status))
    {
        if (LogFilePath != nullptr)
        {
            LogpCleanupBufferInfo(&g_LogpLogBufferInfo);
        }
    }
    return status;
}

/*!
    @brief Registers re-initialization.

    @param[in] DriverObject - A driver object being loaded

    @details A driver must call this function, or call CleanupLogging() and
        return non STATUS_SUCCESS from DriverEntry() if InitializeLogging()
        returned TRUE to ReinitRequired.If this function is called,
        DriverEntry() must return STATUS_SUCCESS.
*/
LOGGING_INIT
_Use_decl_annotations_
VOID
LogRegisterReinitialization (
    PDRIVER_OBJECT DriverObject
    )
{
    PAGED_CODE();

    IoRegisterBootDriverReinitialization(DriverObject,
                                         LogpReinitializationRoutine,
                                         &g_LogpLogBufferInfo);
    LOGGING_LOG_INFO("The log file will be activated later.");
}

/*!
    @brief Initializes a log file at the re-initialization phase.

    @param[in] DriverObject - The driver object to initialize.

    @param[in] Context - The context pointer passed to the registration function.

    @param[in] Count - The number indicating how many time this reinitialization
        callback is invoked, starting at 1.
 */
LOGGING_PAGED
static
_Use_decl_annotations_
VOID
LogpReinitializationRoutine (
    PDRIVER_OBJECT DriverObject,
    PVOID Context,
    ULONG Count
    )
{
    NTSTATUS status;
    PLOG_BUFFER_INFO info;
    BOOLEAN reinitRequired;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(Count);

    NT_ASSERT(ARGUMENT_PRESENT(Context));
    _Analysis_assume_(ARGUMENT_PRESENT(Context));

    info = reinterpret_cast<PLOG_BUFFER_INFO>(Context);
    status = LogpInitializeLogFile(info, &reinitRequired);
    if (!NT_SUCCESS(status))
    {
        NT_ASSERT(FALSE);
        goto Exit;
    }

    NT_ASSERT(reinitRequired != FALSE);
    LOGGING_LOG_INFO("The log file has been activated.");

Exit:
    return;
}

/*!
    @brief Terminates the log system. Should be called from an IRP_MJ_SHUTDOWN handler.
*/
LOGGING_PAGED
_Use_decl_annotations_
VOID
LogIrpShutdownHandler (
    VOID
    )
{
    PLOG_BUFFER_INFO info;

    PAGED_CODE();

    LOGGING_LOG_DEBUG("Flushing... (Max log buffer usage = %Iu/%lu bytes)",
                      g_LogpLogBufferInfo.LogMaxUsage,
                      k_LogpBufferSize);
    LOGGING_LOG_INFO("Bye!");

    g_LogpDebugFlag = k_LogPutLevelDisable;

    //
    // Wait until the log buffer is emptied.
    //
    info = &g_LogpLogBufferInfo;
    while (info->LogBufferHead[0] != ANSI_NULL)
    {
        LogpSleep(k_LogpLogFlushIntervalMsec);
    }
}

/*!
    @brief Terminates the log system. Should be called from a DriverUnload routine.
*/
LOGGING_PAGED
_Use_decl_annotations_
VOID
CleanupLogging (
    VOID
    )
{
    PAGED_CODE();

    LOGGING_LOG_DEBUG("Finalizing... (Max log buffer usage = %Iu/%lu bytes)",
                      g_LogpLogBufferInfo.LogMaxUsage,
                      k_LogpBufferSize);
    LOGGING_LOG_INFO("Bye!");

    g_LogpDebugFlag = k_LogPutLevelDisable;
    LogpCleanupBufferInfo(&g_LogpLogBufferInfo);
}

/*!
    @brief Logs a message; use HYPERPLATFORM_LOG_*() macros instead.

    @param[in] Level - Severity of a message.

    @param[in] FunctionName - A name of a function called this function.

    @param[in] Format - A format string.

    @return STATUS_SUCCESS on success.

    @see LOGGING_LOG_DEBUG.

    @see LOGGING_LOG_DEBUG_SAFE.
*/
_Use_decl_annotations_
NTSTATUS
LogpPrint (
    ULONG Level,
    PCSTR FunctionName,
    PCSTR Format,
    ...
    )
{
    NTSTATUS status;
    va_list args;
    CHAR logMessage[412];
    ULONG pureLevel;
    ULONG attributes;
    CHAR message[512];

    if (!BooleanFlagOn(g_LogpDebugFlag, Level))
    {
        status = STATUS_SUCCESS;
        goto Exit;
    }

    if ((g_LogpDriverVerified != FALSE) && BooleanFlagOn(Level, k_LogpLevelOptSafe))
    {
        status = STATUS_SUCCESS;
        goto Exit;
    }

    va_start(args, Format);
    status = RtlStringCchVPrintfA(logMessage,
                                  RTL_NUMBER_OF(logMessage),
                                  Format,
                                  args);
    va_end(args);
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }
    if (logMessage[0] == ANSI_NULL)
    {
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    pureLevel = Level & 0xf0;
    attributes = Level & 0x0f;

    //
    // A single entry of log should not exceed 512 bytes. See
    // Reading and Filtering Debugging Messages in MSDN for details.
    //
    static_assert(RTL_NUMBER_OF(message) <= 512,
                  "One log message should not exceed 512 bytes.");
    status = LogpMakePrefix(pureLevel,
                            FunctionName,
                            logMessage,
                            message,
                            RTL_NUMBER_OF(message));
    if (!NT_SUCCESS(status))
    {
        goto Exit;
    }

    status = LogpPut(message, attributes);
    if (!NT_SUCCESS(status))
    {
        NT_ASSERT(FALSE);
        goto Exit;
    }

Exit:
    return status;
}

/*!
    @brief The entry point of the buffer flush thread.

    @param[in] StartContext - The thread context pointer.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.

    @details A thread runs as long as Info.BufferFlushThreadShouldBeAlive is TRUE
        and flushes a log buffer to a log file every k_LogpLogFlushIntervalMsec msec.
 */
LOGGING_PAGED
static
_Use_decl_annotations_
VOID
LogpBufferFlushThreadRoutine (
    PVOID StartContext
    )
{
    NTSTATUS status;
    PLOG_BUFFER_INFO info;

    PAGED_CODE();

    status = STATUS_SUCCESS;
    info = reinterpret_cast<PLOG_BUFFER_INFO>(StartContext);
    info->BufferFlushThreadStarted = TRUE;
    LOGGING_LOG_DEBUG("Logger thread started");

    while (info->BufferFlushThreadShouldBeAlive != FALSE)
    {
        NT_ASSERT(LogpIsLogFileActivated(info) != FALSE);

        if (info->LogBufferHead[0] != ANSI_NULL)
        {
            NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
            NT_ASSERT(KeAreAllApcsDisabled() == FALSE);

            status = LogpFlushLogBuffer(info);
            //
            // Do not flush the file for overall performance. Even a case of
            // bug check, we should be able to recover logs by looking at both
            // log buffers.
            //
        }
        LogpSleep(k_LogpLogFlushIntervalMsec);
    }
    PsTerminateSystemThread(status);
}

EXTERN_C_END
