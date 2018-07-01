/*!
    @file HookKernelHandlers.cpp

    @brief Kernel mode code implementing hook handlers.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#include "HookKernelHandlers.hpp"
#include "Common.hpp"
#include "HookCommon.hpp"

LONG64 g_ZwQuerySystemInformationCounter;
LONG64 g_ExAllocatePoolWithTagCounter;
LONG64 g_ExFreePoolWithTagCounter;
LONG64 g_ExFreePoolCounter;

EXTERN_C
NTKERNELAPI
PVOID
NTAPI
RtlPcToFileHeader (
    _In_ PVOID PcValue,
    _Out_ PVOID* BaseOfImage
    );

//
// Handy union to convert a pool tag in ULONG to a string (char*).
//
typedef union _POOL_TAG_VALUE
{
    //
    // Make it bigger than ULONG to automatically embed terminating null into
    // AsUchars[4..7];
    //
    ULONGLONG AsUlonglong;
    UCHAR AsUchars[4];
} TAG_VALUE, *PTAG_VALUE;

/*!
    @brief Finds the original call stub for the specified hook handler.

    @param[in] Handler - The address of the hook handler needing the original
        call stub.

    @return The address of the original call stub.
 */
template<typename HandlerType>
static
_Check_return_
HandlerType
GetOriginalCallStub (
    _In_ HandlerType Handler
    )
{
    for (const auto& registration : g_HookRegistrationEntries)
    {
        if (registration.Handler == Handler)
        {
            NT_ASSERT(registration.HookEntry.OriginalCallStub);
            return reinterpret_cast<HandlerType>(registration.HookEntry.OriginalCallStub);
        }
    }
    NT_ASSERT(false);
    return nullptr;
}

/*!
    @brief Logs execution of ZwQuerySystemInformation.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
HandleZwQuerySystemInformation (
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
    )
{
    NTSTATUS status;

    auto zwQuerySystemInformation = GetOriginalCallStub(HandleZwQuerySystemInformation);
    status = zwQuerySystemInformation(SystemInformationClass,
                                      SystemInformation,
                                      SystemInformationLength,
                                      ReturnLength);

    InterlockedIncrement64(&g_ZwQuerySystemInformationCounter);

    LOGGING_LOG_DEBUG("%p: ZwQuerySystemInformation(SystemInformationClass= %3d, ...) => %08x",
                      _ReturnAddress(),
                      SystemInformationClass,
                      status);

    return status;
}

/*!
    @brief Converts a pool tag in integer to a printable string

    @param[in] TagValue - The pool tag value to convert.

    @return The converted tag value.
 */
static
_Check_return_
TAG_VALUE
TagToString (
    _In_ ULONG TagValue
    )
{
    TAG_VALUE tag;

    tag.AsUlonglong = TagValue;
    for (auto& c : tag.AsUchars)
    {
        if ((c == ANSI_NULL) || (isspace(c) != 0))
        {
            c = ' ';
        }
        if (isprint(c) == 0)
        {
            c = '.';
        }
    }
    return tag;
}

/*!
    @brief Logs execution of ExAllocatePoolWithTag.
 */
PVOID
NTAPI
HandleExAllocatePoolWithTag (
    POOL_TYPE PoolType,
    SIZE_T NumberOfBytes,
    ULONG Tag
    )
{
    PVOID pointer;
    PVOID returnAddress;
    PVOID imageBaseAddress;

    auto exAllocatePoolWithTag = GetOriginalCallStub(HandleExAllocatePoolWithTag);
    pointer = exAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);

    InterlockedIncrement64(&g_ExAllocatePoolWithTagCounter);

    //
    // Log only when it is called from outside of any image.
    //
    returnAddress = _ReturnAddress();
    if (RtlPcToFileHeader(returnAddress, &imageBaseAddress) != nullptr)
    {
        goto Exit;
    }

    LOGGING_LOG_INFO("%p: ExAllocatePoolWithTag(PoolType= %08x, "
                     "NumberOfBytes= %08Ix, Tag= %s) => %p",
                     returnAddress,
                     PoolType,
                     NumberOfBytes,
                     TagToString(Tag).AsUchars,
                     pointer);

Exit:
    return pointer;
}

/*!
    @brief Logs execution of ExFreePoolWithTag.
 */
VOID
NTAPI
HandleExFreePoolWithTag (
    PVOID P,
    ULONG Tag
    )
{
    PVOID returnAddress;
    PVOID imageBaseAddress;

    auto exFreePoolWithTag = GetOriginalCallStub(HandleExFreePoolWithTag);
    exFreePoolWithTag(P, Tag);

    InterlockedIncrement64(&g_ExFreePoolWithTagCounter);

    //
    // Log only when it is called from outside of any image.
    //
    returnAddress = _ReturnAddress();
    if (RtlPcToFileHeader(returnAddress, &imageBaseAddress) != nullptr)
    {
        goto Exit;
    }

    LOGGING_LOG_INFO("%p: ExFreePoolWithTag(P= %p, Tag= %s)",
                     returnAddress,
                     P,
                     TagToString(Tag).AsUchars);

Exit:
    return;
}

/*!
    @brief Logs execution of ExFreePool.
 */
VOID
NTAPI
HandleExFreePool (
    PVOID P
    )
{
    PVOID returnAddress;
    PVOID imageBaseAddress;

    auto exFreePool = GetOriginalCallStub(HandleExFreePool);
    exFreePool(P);

    InterlockedIncrement64(&g_ExFreePoolCounter);

    //
    // Log only when it is called from outside of any image.
    //
    returnAddress = _ReturnAddress();
    if (RtlPcToFileHeader(returnAddress, &imageBaseAddress) != nullptr)
    {
        goto Exit;
    }

    LOGGING_LOG_INFO("%p: ExFreePool(P= %p)", returnAddress, P);

Exit:
    return;
}
