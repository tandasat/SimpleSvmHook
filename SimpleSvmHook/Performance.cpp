/*!
    @file Performance.cpp

    @brief Performance measurement functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018-2019, Satoshi Tanda. All rights reserved.
 */
#include "Performance.hpp"
#include "Logging.hpp"

//
// Error annotation: Must succeed pool allocations are forbidden.
// Allocation failures cause a system crash.
//
#pragma prefast(disable : __WARNING_ERROR, "This is completely bogus.")

static const ULONG k_PerformancePoolTag = 'freP';

/*!
    @brief Responsible for collecting and saving data supplied by PerfCounter.

    @details We normally put declaration and implementation of a class into a
        header file and a source file. But we put all here because we want to
        keep those performance measurement functions into just a pair of header
        and source files
*/
class PerfCollector
{
    public:
        //
        // A function type for printing out a header line of results.
        //
        typedef
        VOID
        (*INITIAL_OUTPUT_ROUTINE) (
            _In_opt_ PVOID OutputContext
            );

        //
        // A function type for printing out a footer line of results.
        //
        typedef
        VOID
        (*FINAL_OUTPUT_ROUTINE) (
            _In_opt_ PVOID OutputContext
            );

        //
        // A function type for printing out results.
        //
        typedef
        VOID
        (*OUTPUT_ROUTINE) (
            _In_ PCSTR LocationName,
            _In_ ULONG64 TotalExecutionCount,
            _In_ ULONG64 TotalElapsedTime,
            _In_opt_ PVOID OutputContext
            );

        //
        // A function type for acquiring and releasing a lock.
        //
        typedef
        VOID
        (*LOCK_ROUTINE) (
            _In_opt_ PVOID LockContext
            );

        VOID
        Initialize (
            _In_ OUTPUT_ROUTINE OutputRoutine,
            _In_opt_ INITIAL_OUTPUT_ROUTINE InitialOutputRoutine = [](PVOID){},
            _In_opt_ FINAL_OUTPUT_ROUTINE FinalOutputRoutine = [](PVOID){},
            _In_opt_ PVOID OutputContext = nullptr,
            _In_opt_ LOCK_ROUTINE LockEnterRoutine = [](PVOID){},
            _In_opt_ LOCK_ROUTINE LockLeaveRoutine = [](PVOID){},
            _In_opt_ PVOID LockContext = nullptr
            );

        VOID
        Cleanup (
            VOID
            );

        VOID
        AddData (
            _In_ PCSTR LocationName,
            _In_ ULONG64 ElapsedTime
            );

    private:
        static const ULONG k_InvalidDataIndex = MAXULONG;
        static const ULONG k_MaxNumberOfDataEntries = 200;

        /*!
            @brief Represents performance data for each location.
        */
        typedef struct _PERFORMANCE_DATA_ENTRY
        {
            PCSTR Key;                      // Identifies a subject matter location.
            ULONG64 TotalExecutionCount;    // How many times executed.
            ULONG64 TotalElapsedTime;       // An accumulated elapsed time.
        } PERFORMANCE_DATA_ENTRY, *PPERFORMANCE_DATA_ENTRY;

        _Check_return_
        ULONG
        GetPerfDataIndex (
            _In_ PCSTR Key
            );

        INITIAL_OUTPUT_ROUTINE m_InitialOutputRoutine;
        FINAL_OUTPUT_ROUTINE m_FinalOutputRoutine;
        OUTPUT_ROUTINE m_OutputRoutine;
        PVOID m_OutputContext;

        LOCK_ROUTINE m_LockEnterRoutine;
        LOCK_ROUTINE m_LockLeaveRoutine;
        PVOID m_LockContext;

        PERFORMANCE_DATA_ENTRY m_PerformanceData[k_MaxNumberOfDataEntries];
};

/*!
    @brief Initializes the current instance.

    @details This function is a constructor of the class virtually. It is not
        implemented as a constructor to avoid requirement of new/delete operators.

    @param[in] OutputRoutine - A function pointer for printing out results.

    @param[in] InitialOutputRoutine - A function pointer for printing a header
        line of results.

    @param[in] FinalOutputRoutine - A function pointer for printing a footer
        line of results.

    @param[in] OutputContext - An arbitrary parameter for OutputRoutine,
        InitialOutputRoutine and FinalOutputRoutine.

    @param[in] LockEnterRoutine - A function pointer for acquiring a lock.

    @param[in] LockLeaveRoutine - A function pointer for releasing a lock.

    @param[in] LockContext - An arbitrary parameter for \a LockEnterRoutine and
        LockLeaveRoutine.
*/
_Use_decl_annotations_
VOID
PerfCollector::Initialize (
    OUTPUT_ROUTINE OutputRoutine,
    INITIAL_OUTPUT_ROUTINE InitialOutputRoutine,
    FINAL_OUTPUT_ROUTINE FinalOutputRoutine,
    PVOID OutputContext,
    LOCK_ROUTINE LockEnterRoutine,
    LOCK_ROUTINE LockLeaveRoutine,
    PVOID LockContext
    )
{
    m_InitialOutputRoutine = InitialOutputRoutine;
    m_FinalOutputRoutine = FinalOutputRoutine;
    m_OutputRoutine = OutputRoutine;
    m_OutputContext = OutputContext;
    m_LockEnterRoutine = LockEnterRoutine;
    m_LockLeaveRoutine = LockLeaveRoutine;
    m_LockContext = LockContext;
    RtlZeroMemory(m_PerformanceData, sizeof(m_PerformanceData));
}

/*!
    @brief Destructor; prints out accumulated performance results.
*/
VOID
PerfCollector::Cleanup (
    VOID
    )
{
    if (m_PerformanceData[0].Key != nullptr)
    {
        m_InitialOutputRoutine(m_OutputContext);
    }

    for (auto i = 0ul; i < k_MaxNumberOfDataEntries; ++i)
    {
        if (m_PerformanceData[i].Key == nullptr)
        {
            break;
        }

        m_OutputRoutine(m_PerformanceData[i].Key,
                        m_PerformanceData[i].TotalExecutionCount,
                        m_PerformanceData[i].TotalElapsedTime,
                        m_OutputContext);
    }
    if (m_PerformanceData[0].Key != nullptr)
    {
        m_FinalOutputRoutine(m_OutputContext);
    }
}

/*!
    @brief Saves performance data taken by PerfCounter.

    @param[in] LocationName - The function name where being measured.

    @param[in] ElapsedTime - The elapsed time measured and to be saved.
*/
_Use_decl_annotations_
VOID
PerfCollector::AddData (
    PCSTR LocationName,
    ULONG64 ElapsedTime
    )
{
    ULONG dataIndex;

    m_LockEnterRoutine(m_LockContext);

    dataIndex = GetPerfDataIndex(LocationName);
    if (dataIndex == k_InvalidDataIndex)
    {
        NT_ASSERT(FALSE);
        goto Exit;
    }

    m_PerformanceData[dataIndex].TotalExecutionCount++;
    m_PerformanceData[dataIndex].TotalElapsedTime += ElapsedTime;

Exit:
    m_LockLeaveRoutine(m_LockContext);
}
/*!
    @brief Returns an index of data corresponds to the LocationName.

    @details It adds a new entry when the Key is not found.

    @param[in] Key - A location to get an index of corresponding data entry.

    @return An index of data or k_InvalidDataIndex if a corresponding entry is
        not found and there is no room to add a new entry.
*/
_Use_decl_annotations_
ULONG
PerfCollector::GetPerfDataIndex (
    PCSTR Key
    )
{
    ULONG key;

    key = k_InvalidDataIndex;
    if (Key == nullptr)
    {
        goto Exit;
    }

    for (auto i = 0ul; i < k_MaxNumberOfDataEntries; ++i)
    {
        if (m_PerformanceData[i].Key == Key)
        {
            key = i;
            goto Exit;
        }

        if (m_PerformanceData[i].Key == nullptr)
        {
            m_PerformanceData[i].Key = Key;
            key = i;
            goto Exit;
        }
    }

Exit:
    return key;
}

//
// Stores all performance data collected by #PERFORMANCE_MEASURE_THIS_SCOPE().
//
PerfCollector* g_PerformanceCollector;

/*!
    @brief Print outs the header of the performance data report.

    @param[in] OutputContext - The context pointer. Unused.
 */
static
VOID
InitialOutputRoutine (
    _In_opt_ PVOID OutputContext
    )
{
    UNREFERENCED_PARAMETER(OutputContext);

    LOGGING_LOG_INFO("%-45s,%-20s,%-20s",
                     "FunctionName(Line)",
                     "Execution Count",
                     "Elapsed Time");
}

/*!
    @brief Print outs performance data of the single location.

    @param[in] LocationName - The name of the location.

    @param[in] TotalExecutionCount - How many times executed.

    @param[in] TotalElapsedTime - An accumulated elapsed time.

    @param[in] OutputContext - The context pointer. Unused.
 */
static
VOID
OutputRoutine (
    _In_ PCSTR LocationName,
    _In_ ULONG64 TotalExecutionCount,
    _In_ ULONG64 TotalElapsedTime,
    _In_opt_ PVOID OutputContext
    )
{
    UNREFERENCED_PARAMETER(OutputContext);

    LOGGING_LOG_INFO("%-45s,%20I64u,%20I64u,",
                     LocationName,
                     TotalExecutionCount,
                     TotalElapsedTime);
}

/*!
    @brief Makes #PERFORMANCE_MEASURE_THIS_SCOPE() ready for use.

    @return STATUS_SUCCESS on success; otherwise, an appropriate error code.
 */
PERFORMANCE_INIT
_Use_decl_annotations_
NTSTATUS
InitializePerformance (
    VOID
    )
{
    NTSTATUS status;
    PerfCollector* collector;

    PAGED_CODE();

    collector = static_cast<PerfCollector*>(ExAllocatePoolWithTag(
                                                            NonPagedPool,
                                                            sizeof(*collector),
                                                            k_PerformancePoolTag));
    if (collector == nullptr)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    collector->Initialize(OutputRoutine, InitialOutputRoutine);

    g_PerformanceCollector = collector;
    status = STATUS_SUCCESS;

Exit:
    return status;
}

/*!
    @brief Ends performance monitoring and outputs collected results.
 */
PERFORMANCE_PAGED
_Use_decl_annotations_
VOID
CleanupPerformance (
    VOID
    )
{
    PAGED_CODE();

    g_PerformanceCollector->Cleanup();
    ExFreePoolWithTag(g_PerformanceCollector, k_PerformancePoolTag);
}

/*!
    @brief Returns the current time using KeQueryPerformanceCounter.

    @return The current time.
 */
ULONG64
GetCurrentTime (
    VOID
    )
{
    return static_cast<ULONG64>(KeQueryPerformanceCounter(nullptr).QuadPart);
}

/*!
    @brief Gets the current time using QueryTimeRoutine.

    @param[in] Collector - The PerfCollector instance to store performance data.

    @param[in] QueryTimeRoutine - The function pointer for getting times.

    @param[in] LocationName - The function name where being measured.
 */
_Use_decl_annotations_
PerfCounter::PerfCounter (
    PerfCollector* Collector,
    QUERY_TIME_ROUTINE QueryTimeRoutine,
    PCSTR LocationName
    ) : m_Collector(Collector),
        m_QueryTimeRoutine((QueryTimeRoutine != nullptr) ? QueryTimeRoutine :
                                                           [](){ return __rdtsc();}),
        m_LocationName(LocationName),
        m_BeforeTime(m_QueryTimeRoutine())
{
}

/*!
    @brief Measures an elapsed time and stores it to PerfCounter::m_Collector.
*/
PerfCounter::~PerfCounter (
    VOID
    )
{
    if (m_Collector != nullptr)
    {
        m_Collector->AddData(m_LocationName, m_QueryTimeRoutine() - m_BeforeTime);
    }
}