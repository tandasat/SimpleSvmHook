/*!
    @file HookKernelHandlers.hpp

    @brief Kernel mode code implementing hook handlers.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <fltKernel.h>

extern LONG64 g_ZwQuerySystemInformationCounter;
extern LONG64 g_ExAllocatePoolWithTagCounter;
extern LONG64 g_ExFreePoolWithTagCounter;
extern LONG64 g_ExFreePoolCounter;

typedef enum _SYSTEM_INFORMATION_CLASS
{
} SYSTEM_INFORMATION_CLASS, *PSYSTEM_INFORMATION_CLASS;

_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
NTAPI
HandleZwQuerySystemInformation (
    _In_ SYSTEM_INFORMATION_CLASS SystemInformationClass,
    _Inout_ PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
    );

decltype(ExAllocatePoolWithTag) HandleExAllocatePoolWithTag;

decltype(ExFreePoolWithTag) HandleExFreePoolWithTag;

decltype(ExFreePool) HandleExFreePool;
