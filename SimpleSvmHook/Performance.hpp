/*!
    @file Performance.hpp

    @brief Performance measurement functions.

    @author Satoshi Tanda

    @copyright Copyright (c) 2018, Satoshi Tanda. All rights reserved.
 */
#pragma once
#include <fltKernel.h>

#define PERFORMANCE_INIT  __declspec(code_seg("INIT"))
#define PERFORMANCE_PAGED __declspec(code_seg("PAGE"))

PERFORMANCE_INIT
_IRQL_requires_max_(PASSIVE_LEVEL)
_Check_return_
NTSTATUS
InitializePerformance (
    VOID
    );

PERFORMANCE_PAGED
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
CleanupPerformance (
    VOID
    );

ULONG64
GetCurrentTime (
    VOID
    );

class PerfCollector;
extern PerfCollector* g_PerformanceCollector;

#if (SIMPLESVMHOOK_ENABLE_PERFCOUNTER == 0)

#define PERFORMANCE_MEASURE_THIS_SCOPE()

#else

/*!
    @brief Measures an elapsed time from this point to the end of the scope.

    @warning This macro cannot used in the INIT section. See
        #PERFORMANCE_P_MEASURE_TIME() for details.
 */
#define PERFORMANCE_MEASURE_THIS_SCOPE() \
    PERFORMANCE_P_MEASURE_TIME(g_PerformanceCollector, GetCurrentTime)

/*!
    @brief Concatenates two tokens.

    @param[in] First - the first token.

    @param[in] Second - the second token.
 */
#define PERFORMANCE_P_JOIN2(First, Second) First##Second
#define PERFORMANCE_P_JOIN1(First, Second) PERFORMANCE_P_JOIN2(First, Second)

/*!
    @brief Stringifies a token.

    @param[in] Token - The token to stringify.
 */
#define PERFORMANCE_P_STRINGIFY2(Token) #Token
#define PERFORMANCE_P_STRINGIFY1(Token) PERFORMANCE_P_STRINGIFY2(Token)

/*!
    @brief Creates an instance of PerfCounter to measure an elapsed time of this scope.

    @param[in] Collector - The pointer to the PerfCollector instance.

    @param[in] QueryTimeRoutine - The function pointer to get an elapsed time.

    @details This macro creates an instance of PerfCounter named perfObjN where
        N is an unique number starting at 0. The current function name and the
        source line number are converted into a string literal and passed to the
        PerfCounter instance to uniquely identify the current location. The
        instance gets "counters" in its constructor and destructor calculates an
        elapsed time with QueryTimeRoutine and passes it to Collector as well as
        the created string literal. In pseudo code, when you use like this,

    @code{.cpp}
    Hello.cpp:233 | {
    Hello.cpp:234 |     PERFORMANCE_P_MEASURE_TIME(collector, fn);
    Hello.cpp:235 |     // do stuff
    Hello.cpp:236 | }
    @endcode

        It works as if below:

    @code{.cpp}
    {
        begin_time = fn();    //perfObj0.ctor();
        // do stuff
        elapsed_time = fn();  //perfObj0.dtor();
        collector->AddTime(elapsed_time, "Hello.cpp(234)");
    }
    @endcode

    @warning Do not use this macro in where going to be unavailable at the time
        of a call of PerfCollector::Cleanup(). This causes access violation
        because this macro embeds a string literal in the used section, and the
        string is referenced in the PerfCollector::Cleanup(), while it is no
        longer accessible if the section is already destroyed. The primary
        example of such places is the INIT section.
 */
#define PERFORMANCE_P_MEASURE_TIME(Collector, QueryTimeRoutine) \
  const PerfCounter PERFORMANCE_P_JOIN1(perfObj, __COUNTER__)( \
                        (Collector), \
                        (QueryTimeRoutine), \
                        __FUNCTION__ "(" PERFORMANCE_P_STRINGIFY1(__LINE__) ")")

#endif

/*!
    @brief This class is used to measure the elapsed time of the scope.

    @details #PERFORMANCE_P_MEASURE_TIME() should be used to create an instance
        of this class.
*/
class PerfCounter
{
    public:
        typedef
        ULONG64
        (*QUERY_TIME_ROUTINE) (
            VOID
            );

        PerfCounter (
            _In_ PerfCollector* Collector,
            _In_opt_ QUERY_TIME_ROUTINE QueryTimeRoutine,
            _In_ PCSTR LocationName
            );

        ~PerfCounter (
            VOID
            );

    private:
        PerfCollector* m_Collector;
        QUERY_TIME_ROUTINE m_QueryTimeRoutine;
        PCSTR m_LocationName;
        const ULONG64 m_BeforeTime;
};

