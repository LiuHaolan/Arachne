/* Copyright (c) 2015-2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ARACHNE_H_
#define ARACHNE_H_

#include <assert.h>
#include <stdio.h>
#include <functional>
#include <vector>
#include <mutex>
#include <deque>
#include <atomic>
#include <queue>
#include <string>


#include "PerfUtils/Cycles.h"
#include "Logger.h"

namespace Arachne {

// A macro to disallow the copy constructor and operator= functions
#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&) = delete; \
    TypeName& operator=(const TypeName&) = delete;
#endif

#define CACHE_LINE_SIZE 64

using PerfUtils::Cycles;

// Forward declare to break circular dependency between ThreadContext and
// ConditionVariable
struct ThreadContext;

// This is used in createThread.
extern std::atomic<uint32_t> numActiveCores;

extern volatile uint32_t minNumCores;
extern volatile uint32_t maxNumCores;

// This is used in createThread.
extern std::atomic<uint32_t> numActiveCores;

extern int stackSize;

// Used in inline functions.
extern FILE* errorStream;
void dispatch();

// Used for user per-core data structure initialization.
extern std::function<void()> initCore;

extern thread_local int kernelThreadId;
extern thread_local ThreadContext *loadedContext;
extern thread_local ThreadContext** localThreadContexts;
extern std::vector<ThreadContext**> allThreadContexts;

extern int* virtualCoreTable;

/**
 * \addtogroup api Arachne Public API
 * Most of the functions in this API, with the exception of Arachne::init(),
 * Arachne::shutDown(), Arachne::waitForTermination(), and
 * Arachne::createThread(), should only be called from within Arachne threads.
 *
 * In order to allow unit tests in non-Arachne threads to run,
 * Arachne::testInit() should be called once before all unit tests run, and
 * Arachne::testDestroy() should be called once after all unit tests finish.
 * @{
 */
/**
  * This structure is used to identify an Arachne thread to methods of the
  * Arachne API.
  */
struct ThreadId {
    /// The storage where this thread's state is held.
    ThreadContext* context;
    /// Differentiates this Arachne thread from others that use the same
    /// context.
    uint32_t generation;

    /// Construct a ThreadId.
    /// \param context
    ///    The location where the thread's metadata currently lives.
    /// \param generation
    ///    Used to differentiate this thread from others that lived at this
    ///    context in the past and future.
    ThreadId(ThreadContext* context, uint32_t generation)
        : context(context)
        , generation(generation) { }

    ThreadId()
        : context(NULL)
        , generation(0) { }

    /// The equality operator is generally used for comparing against
    /// Arachne::NullThread.
    bool
    operator==(const ThreadId& other) const {
        return context == other.context && generation == other.generation;
    }

    /// Negation of the function above.
    bool
    operator!=(const ThreadId& other) const {
        return !(*this == other);
    }

    bool
    operator!() const {
        return *this == ThreadId();
    }
};

void init(int* argcp = NULL, const char** argv = NULL);
void shutDown();
void waitForTermination();
void yield();
void sleep(uint64_t ns);

bool makeExclusiveOnCore();
void makeSharedOnCore();

/**
 * Block the current thread until another thread invokes join() with the
 * current thread's ThreadId.
 */
inline void block() {
    dispatch();
}
void signal(ThreadId id);
void join(ThreadId id);
ThreadId getThreadId();

void setErrorStream(FILE* ptr);
void testInit();
void testDestroy();

/**
 * A resource that can be acquired by only one thread at a time.
 */
class SpinLock {
 public:
    /** Constructor and destructor for spinlock. */
    SpinLock(std::string name, bool shouldYield = true)
        : state(false)
        , name(name)
        , shouldYield(shouldYield) {}
    // Delegating constructor forces const char* to resolve to string instead
    // of bool.
    SpinLock(const char* name, bool shouldYield = true)
        : SpinLock(std::string(name), shouldYield) {}
    SpinLock(bool shouldYield = true)
        : state(false)
        , owner(NULL)
        , name("unnamed")
        , shouldYield(shouldYield) {}
    ~SpinLock(){}

    /** Repeatedly try to acquire this resource until success. */
    inline void
    lock() {
        uint64_t startOfContention = 0;
        while (state.exchange(true, std::memory_order_acquire) != false) {
            if (startOfContention == 0) {
                startOfContention = Cycles::rdtsc();
            } else {
                uint64_t now = Cycles::rdtsc();
                if (Cycles::toSeconds(now - startOfContention) > 1.0) {
                    LOG(WARNING,
                            "%s SpinLock locked for one second; deadlock?\n",
                            name.c_str());
                    startOfContention = now;
                }
            }
            if (shouldYield) yield();
        }
        owner = loadedContext;
    }

    /**
     * Attempt to acquire this resource once.
     * \return
     *    Whether or not the acquisition succeeded.  inline bool
     */
    inline bool
    try_lock() {
        // If the original value was false, then we successfully acquired the
        // lock. Otherwise we failed.
        if (!state.exchange(true, std::memory_order_acquire)) {
            owner = loadedContext;
            return true;
        }
        return false;
    }

    /** Release resource. */
    inline void
    unlock() {
        state.store(false, std::memory_order_release);
    }

    /** Set the label used for deadlock warning. */
    inline void
    setName(std::string name) {
        this->name = name;
    }

 private:
    // Implements the lock: false means free, true means locked
    std::atomic<bool> state;

    // Used to identify the owning context for this lock.
    ThreadContext* owner;

    // Descriptive name for this SpinLock. Used to identify the purpose of
    // the lock, what it protects, where it exists in the codebase, etc.
    //
    // Used to identify the lock when reporting a potential deadlock.
    std::string name;

    // Controls whether the acquiring thread should yield control of the core
    // each time it fails to acquire this SpinLock.
    //
    // For short critical sections that hold onto a core throughout, it will
    // minimize latency to set this to false.
    // For longer critical sections that may relinguish a core, it is necessary
    // to set this to true to avoid deadlock.
    bool shouldYield;
};

/**
  * A resource which blocks the current thread until it is available.
  * This resources should not be acquired from non-Arachne threads.
  */
class SleepLock {
 public:
    /** Constructor and destructor for sleepLock. */
    SleepLock() : blockedThreads(), blockedThreadsLock(false), owner(NULL) {}
    ~SleepLock(){}
    void lock();
    bool try_lock();
    void unlock();

 private:
    // Ordered collection of threads that are waiting on this condition
    // variable. Threads are processed from this list in FIFO order when a
    // notifyOne() is called.
    std::deque<ThreadId> blockedThreads;

    // A SpinLock to protect the blockedThreads data structure.
    SpinLock blockedThreadsLock;

    // Used to identify the owning context for this lock, and also indicates
    // whether the lock is held or not.
    ThreadContext* owner;
};

/**
  * This class enables one or more threads to block until a condition is true,
  * and then be awoken when the condition might be true.
  */
class ConditionVariable {
 public:
    ConditionVariable();
    ~ConditionVariable();
    void notifyOne();
    void notifyAll();
    template <typename LockType> void wait(LockType& lock);
    template <typename LockType> void waitFor(LockType& lock, uint64_t ns);
 private:
    // Ordered collection of threads that are waiting on this condition
    // variable. Threads are processed from this list in FIFO order when a
    // notifyOne() is called.
    std::deque<ThreadId> blockedThreads;
    DISALLOW_COPY_AND_ASSIGN(ConditionVariable);
};

/**
  * This value represents the non-existence of a thread and can be returned by
  * any Arachne function that would normally return a ThreadId.
  *
  * One example is createThread when there are not enough resources to create a
  * new thread.
  */
const Arachne::ThreadId NullThread;
/**@}*/

////////////////////////////////////////////////////////////////////////////////
// The declarations in following section are private to the thread library.
////////////////////////////////////////////////////////////////////////////////

/**
  * We need to invoke a ThreadInvocation with unknown template types, which has
  * been stored in a character array, and this class enables us to do this.
  */
struct ThreadInvocationEnabler {
    /// This function allows us to invoke the templated subtype, without
    /// casting to a pointer of unknown type.
    virtual void runThread() = 0;
    virtual ~ThreadInvocationEnabler() { }
};

/**
  * This structure is used during thread creation to pass the function and
  * arguments for the new thread's top-level function from a creating thread to
  * the core that runs the new thread. It also ensures that the arguments will
  * fit in a single cache line, since they will be stored in a single cache
  * line.
  *
  * \tparam F
  *     The type of the return value of std::bind, which is a value type of
  *     unspecified class.
  *
  * This wrapper enables us to bypass the dynamic memory allocation that is
  * sometimes performed by std::function.
  */
template<typename F>
struct ThreadInvocation : public ThreadInvocationEnabler {
    /// The top-level function of the Arachne thread.
    F mainFunction;

    /// Construct a threadInvocation from the type that is returned by
    /// std::bind.
    explicit ThreadInvocation(F mainFunction)
        : mainFunction(mainFunction) {
        static_assert(sizeof(ThreadInvocation<F>) <= CACHE_LINE_SIZE,
                "Arachne requires the function and arguments for a thread to "
                "fit within one cache line.");
    }

    /// This is invoked exactly once for each Arachne thread to begin its
    /// execution.
    void
    runThread() {
        mainFunction();
    }
};

/**
 * This class holds all the state for managing an Arachne thread.
 */
struct ThreadContext {
    /// Keep a reference to the original memory allocation for the stack used by
    /// this threadContext so that we can release the memory in shutDown.
    void* stack;

    /// This holds the value that rsp, the stack pointer register, will be set
    /// to when this thread is swapped in.
    void* sp;

    /// This variable holds the minimum value of the cycle counter for which
    /// this thread can run.
    /// 0 is a signal that this thread should run at the next opportunity.
    /// ~0 is used as an infinitely large time: a sleeping thread will not
    /// awaken as long as wakeupTimeInCycles has this value.
    volatile uint64_t wakeupTimeInCycles;

    /// Used as part of ThreadIds to differentiate Arachne threads that use this
    /// ThreadContext; incremented whenever an Arachne thread finishes execution
    /// in this ThreadContext.
    uint32_t generation;

    /// This lock is used for synchronizing threads that attempt to join this
    /// thread.
    SpinLock joinLock;

    /// Threads attempting to join the thread that currently occupies this
    /// context shall wait on this CV.
    ConditionVariable joinCV;

    // Unique identifier for the core that this thread currently lives on.
    // Used to index into global arrays with information per core.
    // This will only change if a ThreadContext is migrated when scaling down
    // the number of cores.
    // This is a not a virtual coreId, which is used only to ensure that thread
    // creation sees a contiguous range of CoreId's.
    uint8_t coreId;

    /// Unique identifier for this thread among those on the same core.
    /// Used to index into various core-specific arrays.
    /// This will only change if a ThreadContext is migrated when scaling down
    /// the number of cores.
    uint8_t idInCore;

    /// \var threadInvocation
    /// Storage for the ThreadInvocation object that contains the function and
    /// arguments for a new thread.
    /// We wrap the char buffer in a struct to enable aligning to a cache line
    /// boundary, which eliminates false sharing of cache lines.

    /// \cond SuppressDoxygen
    struct alignas(CACHE_LINE_SIZE) {
        char data[CACHE_LINE_SIZE];
    }
    /// \endcond
    threadInvocation;

    void initializeStack();
    ThreadContext() = delete;
    ThreadContext(ThreadContext&) = delete;

    explicit ThreadContext(uint8_t coreId, uint8_t idInCore);
};

// Largest number of Arachne threads that can be simultaneously created on each
// core.
const int maxThreadsPerCore = 56;

/**
  * This is the number of bytes needed on the stack to store the callee-saved
  * registers that are defined by the current processor and operating system's
  * calling convention.
  */
const size_t SpaceForSavedRegisters = 48;

/**
  * This value is placed at the lowest allocated address of the stack to detect
  * stack overflows.
  */
const uint64_t StackCanary = 0xDEADBAAD;

/**
  * This is the value for wakeupTimeInCycles when a live thread is blocked.
  */
const uint64_t BLOCKED = ~0L;

/**
  * This is the value for wakeupTimeInCycles when a ThreadContext is not
  * hosting a thread.
  */
const uint64_t UNOCCUPIED = ~0L - 1;

/**
  * Amount of time in nanoseconds to wait for extant threads to finish before
  * commencing migration.
  */
const uint64_t COMPLETION_WAIT_TIME = 100000;

/**
  * Initial value of numOccupied for cores that are exclusive to a thread.
  * This value is sufficiently high that when other threads exit and decrement
  * numOccupied, creation will continue to be blocked on the target core.
  */
const uint8_t EXCLUSIVE = maxThreadsPerCore * 2 + 1;

void schedulerMainLoop();
void swapcontext(void **saved, void **target);

/// This structure tracks the live threads on a single core.
struct MaskAndCount{
    /// Each bit corresponds to a particular ThreadContext which has the
    /// idInCore corresponding to its index.
    /// 0 means this context is available for a new thread.
    /// 1 means this context is in use by a live thread.
    uint64_t occupied : 56;
    /// The number of 1 bits in occupied.
    uint8_t numOccupied : 8;
};

extern std::vector< std::atomic<MaskAndCount> *> occupiedAndCount;
extern thread_local std::atomic<MaskAndCount> *localOccupiedAndCount;

extern std::vector< std::atomic<uint64_t> *> publicPriorityMasks;
extern thread_local uint64_t privatePriorityMask;

#ifdef TEST
static std::deque<uint64_t> mockRandomValues;
#endif
/**
  * A random number generator from the Internet that returns 64-bit integers.
  * It is used for selecting candidate cores to create threads on.
  */
inline uint64_t
random(void) {
#ifdef TEST
    if (!mockRandomValues.empty()) {
        uint64_t returnValue = mockRandomValues.front();
        mockRandomValues.pop_front();
        return returnValue;
    }
#endif

    // This function came from the following site.
    // http://stackoverflow.com/a/1640399/391161
    //
    // It was chosen because it was advertised to be fast, but this fact has
    // not yet been verified or disproved through experiments.
    static uint64_t x = 123456789, y = 362436069, z = 521288629;
    uint64_t t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
}

/**
  * Spawn a thread with main function f invoked with the given args on the
  * kernel thread with id = kId
  * This function should usually only be invoked directly in tests, since it
  * does not perform load balancing.
  *
  * \param kId
  *     The id for the kernel thread to put the new Arachne thread on. Pass in
  *     -1 to use the creator's kernel thread. This can be useful if the
  *     created thread will share a lot of state with the current thread, since
  *     it will improve locality.
  * \param __f
  *     The main function for the new thread.
  * \param __args
  *     The arguments for __f.
  * \return
  *     The return value is an identifier for the newly created thread. If
  *     there are insufficient resources for creating a new thread, then
  *     NullThread will be returned.
  */
template<typename _Callable, typename... _Args>
ThreadId
createThreadOnCore(uint32_t virtualCoreId, _Callable&& __f, _Args&&... __args) {
    if (virtualCoreId >= numActiveCores) {
        LOG(VERBOSE, "createThread failure, virtualCoreId = %u, "
                   "numActiveCores = %d\n", virtualCoreId,
                   numActiveCores.load());
        return Arachne::NullThread;
    }

    int coreId = virtualCoreTable[virtualCoreId];

    auto task = std::bind(
            std::forward<_Callable>(__f), std::forward<_Args>(__args)...);

    ThreadContext* threadContext;
    bool success;
    uint32_t index;
    do {
        // Each iteration through this loop makes one attempt to enqueue the
        // task to the specified core. Multiple iterations are required only if
        // there is contention for the core's state variables.
        MaskAndCount slotMap = *occupiedAndCount[coreId];
        MaskAndCount oldSlotMap = slotMap;

        if (slotMap.numOccupied >= maxThreadsPerCore) {
            LOG(VERBOSE, "createThread failure, virtualCoreId = %u, coreId = %u,"
                       "numOccupied = %d\n", virtualCoreId, coreId,
                       slotMap.numOccupied);
            return NullThread;
        }

        // Search for a non-occupied slot and attempt to reserve the slot
        index = 0;
        while ((slotMap.occupied & (1L << index)) && index < maxThreadsPerCore)
            index++;

        slotMap.occupied =
            (slotMap.occupied | (1L << index)) & 0x00FFFFFFFFFFFFFF;
        slotMap.numOccupied++;
        threadContext = allThreadContexts[coreId][index];
        success = occupiedAndCount[coreId]->compare_exchange_strong(oldSlotMap,
                slotMap);
    } while (!success);

    // Copy the thread invocation into the byte array.
    new (&allThreadContexts[coreId][index]->threadInvocation)
        Arachne::ThreadInvocation<decltype(task)>(task);
    threadContext->wakeupTimeInCycles = 0;
    return ThreadId(allThreadContexts[coreId][index],
            allThreadContexts[coreId][index]->generation);
}

////////////////////////////////////////////////////////////////////////////////
// The ends the private section of the thread library.
////////////////////////////////////////////////////////////////////////////////

/**
  * Spawn a new thread with a function and arguments.
  * 
  * \param __f
  *     The main function for the new thread.
  * \param __args
  *     The arguments for __f. The total size of the arguments cannot exceed 48
  *     bytes, and arguments are taken by value, so any reference must be
  *     wrapped with std::ref.
  * \return
  *     The return value is an identifier for the newly created thread. If
  *     there are insufficient resources for creating a new thread, then
  *     NullThread will be returned.
  * 
  * \ingroup api
  */
template<typename _Callable, typename... _Args>
ThreadId
createThread(_Callable&& __f, _Args&&... __args) {
    // Find a kernel thread to enqueue to by picking two at random and choosing
    // the one with the fewest Arachne threads.
    uint32_t kId;
    uint32_t choice1 = static_cast<uint32_t>(random()) % numActiveCores;
    uint32_t choice2 = static_cast<uint32_t>(random()) % numActiveCores;
    while (choice2 == choice1 && numActiveCores > 1)
        choice2 = static_cast<uint32_t>(random()) % numActiveCores;

    if (occupiedAndCount[virtualCoreTable[choice1]]->load().numOccupied <
            occupiedAndCount[virtualCoreTable[choice2]]->load().numOccupied)
        kId = choice1;
    else
        kId = choice2;
    return createThreadOnCore(kId, __f, __args...);
}

/**
  * Block the current thread until the condition variable is notified.
  *
  * \param lock
  *     The mutex associated with this condition variable; must be held by
  *     caller before calling wait. This function releases the mutex before
  *     blocking, and re-acquires it before returning to the user.
  */
template <typename LockType> void
ConditionVariable::wait(LockType& lock) {
#if TIME_TRACE
    TimeTrace::record("Wait on Core %d", kernelThreadId);
#endif
    blockedThreads.push_back(
            ThreadId(loadedContext, loadedContext->generation));
    lock.unlock();
    dispatch();
#if TIME_TRACE
    TimeTrace::record("About to acquire lock after waking up");
#endif
    lock.lock();
}

/**
  * Block the current thread until the condition variable is notified or at
  * least ns nanoseconds has passed.
  *
  * \param lock
  *     The mutex associated with this condition variable; must be held by
  *     caller before calling wait. This function releases the mutex before
  *     blocking, and re-acquires it before returning to the user.
  * \param ns
  *     The time in nanoseconds this thread should wait before returning in the
  *     absence of a signal.
  */
template <typename LockType> void
ConditionVariable::waitFor(LockType& lock, uint64_t ns) {
    blockedThreads.push_back(
            ThreadId(loadedContext, loadedContext->generation));
    loadedContext->wakeupTimeInCycles =
        Cycles::rdtsc() + Cycles::fromNanoseconds(ns);
    lock.unlock();
    dispatch();
    lock.lock();
}

/**
  * This class keeps a per-core record of recent idle time and recent total
  * time in Cycles.
  */
struct DispatchTimeKeeper {
    /**
      * Accumulated idle cycles since the last core change completed.
      * Then, when a coreChangeEvent occurs, we can set idleCycles to 0 and lastResetTime to Cycles::rdtsc.
      */
    static thread_local uint64_t* idleCycles;

    /**
      * Cycle counter of the last reset. The difference between the current
      * time in cycles and this value can be used as a denominator in the
      * calculation of percentage of idle time.
      */
    static uint64_t lastResetTime;

    /**
     * Cycle counter at the top of the last call to Arachne::dispatch() on this
     * core, regardless of which user thread made the call.
     */
    static thread_local uint64_t* dispatchStartCycles;

    /**
     * The number of cycles since the last time we reset the hysteresis cycle.
     * This returns a delta against a thread_local variable.
     */
    static uint64_t cyclesSinceLastReset() {
        return Cycles::rdtsc() - lastResetTime;
    }

    DispatchTimeKeeper() {
        *dispatchStartCycles = Cycles::rdtsc();
    }

    ~DispatchTimeKeeper(){
        *idleCycles += Cycles::rdtsc() - *dispatchStartCycles;
    }
};

} // namespace Arachne

// Force instantiation for debugging with operator[]
template class std::vector<Arachne::ThreadContext**>;
template class std::vector<std::atomic<Arachne::MaskAndCount> * >;
template class std::vector< std::atomic<uint64_t> *>;

#endif // ARACHNE_H_
