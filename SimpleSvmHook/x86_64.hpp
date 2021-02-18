/*!
    @file x86_64.hpp

    @brief x86-64 defined piece.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018-2021, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <basetsd.h>

static constexpr ULONG k_MaxInsturctionLength = 15;

#define IA32_APIC_BASE  0x0000001b
#define IA32_MSR_PAT    0x00000277
#define IA32_MSR_EFER   0xc0000080

#define EFER_SVME       (1UL << 12)

#define RPL_MASK        3
#define DPL_SYSTEM      0

#define CPUID_FN8000_0001_ECX_SVM                   (1UL << 2)
#define CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT    (1UL << 31)
#define CPUID_FN8000_000A_EDX_NP                    (1UL << 0)

#define CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING          0x00000000
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS       0x00000001
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX    0x80000001
#define CPUID_SVM_FEATURES                                      0x8000000a

//
// See: IA32_APIC_BASE MSR Supporting x2APIC
//
typedef struct _APIC_BASE
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Reserved1 : 8;           // [0:7]
            UINT64 BootstrapProcessor : 1;  // [8]
            UINT64 Reserved2 : 1;           // [9]
            UINT64 EnableX2ApicMode : 1;    // [10]
            UINT64 EnableXApicGlobal : 1;   // [11]
            UINT64 ApicBase : 24;           // [12:35]
        } Fields;
    };
} APIC_BASE, *PAPIC_BASE;

//
// See "4-Kbyte PML4E-Long Mode", "4-Kbyte PDPE-Long Mode",
// "4-Kbyte PDE-Long Mode",
//
typedef struct _PML4_ENTRY_4KB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Reserved1 : 3;           // [6:8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 PageFrameNumber : 40;    // [12:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PML4_ENTRY_4KB, *PPML4_ENTRY_4KB,
PDP_ENTRY_4KB, *PPDP_ENTRY_4KB,
PD_ENTRY_4KB, *PPD_ENTRY_4KB;
static_assert(sizeof(PML4_ENTRY_4KB) == 8, "size mismatch");

//
// See "4-Kbyte PTE-Long Mode".
//
typedef struct _PT_ENTRY_4KB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Dirty : 1;               // [6]
            UINT64 Pat : 1;                 // [7]
            UINT64 Global : 1;              // [8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 PageFrameNumber : 40;    // [12:51]
            UINT64 Reserved1 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PT_ENTRY_4KB, *PPT_ENTRY_4KB;
static_assert(sizeof(PT_ENTRY_4KB) == 8, "size mismatch");

//
// See "2-Mbyte PML4E-Long Mode" and "2-Mbyte PDPE-Long Mode".
//
typedef struct _PML4_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Reserved1 : 3;           // [6:8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 PageFrameNumber : 40;    // [12:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PML4_ENTRY_2MB, *PPML4_ENTRY_2MB,
  PDP_ENTRY_2MB, *PPDP_ENTRY_2MB;
static_assert(sizeof(PML4_ENTRY_2MB) == 8,
              "PML4_ENTRY_2MB size mismatch");

//
// See "2-Mbyte PDE-Long Mode".
//
typedef struct _PD_ENTRY_2MB
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT64 Valid : 1;               // [0]
            UINT64 Write : 1;               // [1]
            UINT64 User : 1;                // [2]
            UINT64 WriteThrough : 1;        // [3]
            UINT64 CacheDisable : 1;        // [4]
            UINT64 Accessed : 1;            // [5]
            UINT64 Dirty : 1;               // [6]
            UINT64 LargePage : 1;           // [7]
            UINT64 Global : 1;              // [8]
            UINT64 Avl : 3;                 // [9:11]
            UINT64 Pat : 1;                 // [12]
            UINT64 Reserved1 : 8;           // [13:20]
            UINT64 PageFrameNumber : 31;    // [21:51]
            UINT64 Reserved2 : 11;          // [52:62]
            UINT64 NoExecute : 1;           // [63]
        } Fields;
    };
} PD_ENTRY_2MB, *PPD_ENTRY_2MB;
static_assert(sizeof(PD_ENTRY_2MB) == 8,
              "PD_ENTRY_2MB size mismatch");

//
// See "GDTR and IDTR Format-Long Mode"
//
#include <pshpack1.h>
typedef struct _DESCRIPTOR_TABLE_REGISTER
{
    UINT16 Limit;
    ULONG_PTR Base;
} DESCRIPTOR_TABLE_REGISTER, *PDESCRIPTOR_TABLE_REGISTER;
static_assert(sizeof(DESCRIPTOR_TABLE_REGISTER) == 10,
              "DESCRIPTOR_TABLE_REGISTER size mismatch");
#include <poppack.h>

//
// See "Long-Mode Segment Descriptors" and some of definitions
// (eg, "Code-Segment Descriptor-Long Mode")
//
typedef struct _SEGMENT_DESCRIPTOR
{
    union
    {
        UINT64 AsUInt64;
        struct
        {
            UINT16 LimitLow;        // [0:15]
            UINT16 BaseLow;         // [16:31]
            UINT32 BaseMiddle : 8;  // [32:39]
            UINT32 Type : 4;        // [40:43]
            UINT32 System : 1;      // [44]
            UINT32 Dpl : 2;         // [45:46]
            UINT32 Present : 1;     // [47]
            UINT32 LimitHigh : 4;   // [48:51]
            UINT32 Avl : 1;         // [52]
            UINT32 LongMode : 1;    // [53]
            UINT32 DefaultBit : 1;  // [54]
            UINT32 Granularity : 1; // [55]
            UINT32 BaseHigh : 8;    // [56:63]
        } Fields;
    };
} SEGMENT_DESCRIPTOR, *PSEGMENT_DESCRIPTOR;
static_assert(sizeof(SEGMENT_DESCRIPTOR) == 8,
              "SEGMENT_DESCRIPTOR size mismatch");

typedef struct _SEGMENT_ATTRIBUTE
{
    union
    {
        UINT16 AsUInt16;
        struct
        {
            UINT16 Type : 4;        // [0:3]
            UINT16 System : 1;      // [4]
            UINT16 Dpl : 2;         // [5:6]
            UINT16 Present : 1;     // [7]
            UINT16 Avl : 1;         // [8]
            UINT16 LongMode : 1;    // [9]
            UINT16 DefaultBit : 1;  // [10]
            UINT16 Granularity : 1; // [11]
            UINT16 Reserved1 : 4;   // [12:15]
        } Fields;
    };
} SEGMENT_ATTRIBUTE, *PSEGMENT_ATTRIBUTE;
static_assert(sizeof(SEGMENT_ATTRIBUTE) == 2,
              "SEGMENT_ATTRIBUTE size mismatch");
