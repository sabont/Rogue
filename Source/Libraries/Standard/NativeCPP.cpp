//=============================================================================
//  NativeCPP.cpp
//
//  Rogue runtime routines.
//=============================================================================

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <exception>
#include <cstddef>

#if defined(ROGUE_PLATFORM_WINDOWS)
#  include <sys/timeb.h>
#else
#  include <sys/time.h>
#  include <unistd.h>
#  include <signal.h>
#  include <dirent.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#  include <sys/stat.h>
#  include <netdb.h>
#  include <errno.h>
#endif

#if defined(ANDROID)
  #include <netinet/in.h>
#endif

#if defined(_WIN32)
#  include <direct.h>
#  define chdir _chdir
#endif

#if TARGET_OS_IPHONE
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

#if defined(__APPLE__)
  // Prevent dyld from redefining TRUE & FALSE in an incompatible way
  #if !defined(TRUE)
    #define TRUE  1
    #define FALSE 0
  #endif
  #define ENUM_DYLD_BOOL
  #include <mach-o/dyld.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

//-----------------------------------------------------------------------------
//  GLOBAL PROPERTIES
//-----------------------------------------------------------------------------
bool               Rogue_gc_logging   = false;
int                Rogue_gc_threshold = ROGUE_GC_THRESHOLD_DEFAULT;
int                Rogue_gc_count     = 0; // Purely informational
bool               Rogue_gc_requested = false;
bool               Rogue_gc_active    = false; // Are we collecting right now?
RogueLogical       Rogue_configured = 0;
int                Rogue_argc;
const char**       Rogue_argv;
RogueCallbackInfo  Rogue_on_gc_begin;
RogueCallbackInfo  Rogue_on_gc_trace_finished;
RogueCallbackInfo  Rogue_on_gc_end;
char               RogueDebugTrace::buffer[512];
ROGUE_THREAD_LOCAL RogueDebugTrace* Rogue_call_stack = 0;

struct RogueWeakReference;
RogueWeakReference* Rogue_weak_references = 0;

//-----------------------------------------------------------------------------
//  Multithreading
//-----------------------------------------------------------------------------
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS

#define ROGUE_MUTEX_LOCK(_M) pthread_mutex_lock(&(_M))
#define ROGUE_MUTEX_UNLOCK(_M) pthread_mutex_unlock(&(_M))
#define ROGUE_MUTEX_DEF(_N) pthread_mutex_t _N = PTHREAD_MUTEX_INITIALIZER

#define ROGUE_COND_STARTWAIT(_V,_M) ROGUE_MUTEX_LOCK(_M);
#define ROGUE_COND_DOWAIT(_V,_M,_C) while (_C) pthread_cond_wait(&(_V), &(_M));
#define ROGUE_COND_ENDWAIT(_V,_M) ROGUE_MUTEX_UNLOCK(_M);
#define ROGUE_COND_WAIT(_V,_M,_C) \
  ROGUE_COND_STARTWAIT(_V,_M); \
  ROGUE_COND_DOWAIT(_V,_M,_C); \
  ROGUE_COND_ENDWAIT(_V,_M);
#define ROGUE_COND_DEF(_N) pthread_cond_t _N = PTHREAD_COND_INITIALIZER
#define ROGUE_COND_NOTIFY_ONE(_V,_M,_C)    \
  ROGUE_MUTEX_LOCK(_M);                    \
  _C ;                                     \
  pthread_cond_signal(&(_V));              \
  ROGUE_MUTEX_UNLOCK(_M);
#define ROGUE_COND_NOTIFY_ALL(_V,_M,_C)    \
  ROGUE_MUTEX_LOCK(_M);                    \
  _C ;                                     \
  pthread_cond_broadcast(&(_V));           \
  ROGUE_MUTEX_UNLOCK(_M);

#define ROGUE_THREAD_DEF(_N) pthread_t _N
#define ROGUE_THREAD_JOIN(_T) pthread_join(_T, NULL)
#define ROGUE_THREAD_START(_T, _F) pthread_create(&(_T), NULL, _F, NULL)

#elif ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_CPP

#include <exception>
#include <condition_variable>

#define ROGUE_MUTEX_LOCK(_M) _M.lock()
#define ROGUE_MUTEX_UNLOCK(_M) _M.unlock()
#define ROGUE_MUTEX_DEF(_N) std::mutex _N

#define ROGUE_COND_STARTWAIT(_V,_M) { std::unique_lock<std::mutex> LK(_M);
#define ROGUE_COND_DOWAIT(_V,_M,_C) while (_C) (_V).wait(LK);
#define ROGUE_COND_ENDWAIT(_V,_M) }
#define ROGUE_COND_WAIT(_V,_M,_C) \
  ROGUE_COND_STARTWAIT(_V,_M); \
  ROGUE_COND_DOWAIT(_V,_M,_C); \
  ROGUE_COND_ENDWAIT(_V,_M);
#define ROGUE_COND_DEF(_N) std::condition_variable _N
#define ROGUE_COND_NOTIFY_ONE(_V,_M,_C) {  \
  std::unique_lock<std::mutex> LK2(_M);    \
  _C ;                                     \
  (_V).notify_one(); }
#define ROGUE_COND_NOTIFY_ALL(_V,_M,_C) {  \
  std::unique_lock<std::mutex> LK2(_M);    \
  _C ;                                     \
  (_V).notify_all(); }

#define ROGUE_THREAD_DEF(_N) std::thread _N
#define ROGUE_THREAD_JOIN(_T) (_T).join()
#define ROGUE_THREAD_START(_T, _F) (_T = std::thread([] () {_F(NULL);}),0)

#endif

#if ROGUE_THREAD_MODE != ROGUE_THREAD_MODE_NONE

// Thread mutex locks around creation and destruction of threads
static ROGUE_MUTEX_DEF(Rogue_mt_thread_mutex);
static int Rogue_mt_tc = 0; // Thread count.  Always set under above lock.
static std::atomic_bool Rogue_mt_terminating(false); // True when terminating.

static void Rogue_thread_register ()
{
  ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
  int n = (int)Rogue_mt_tc;
#endif
  ++Rogue_mt_tc;
  ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
  char name[64];
  sprintf(name, "Thread-%i", n); // Nice names are good for valgrind

#if ROGUE_PLATFORM_MACOS
  pthread_setname_np(name);
#elif __linux__
  pthread_setname_np(pthread_self(), name);
#endif
// It should be possible to get thread names working on lots of other
// platforms too.  The functions just vary a bit.
#endif
}

static void Rogue_thread_unregister ()
{
  ROGUE_EXIT;
  ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
  ROGUE_ENTER;
  --Rogue_mt_tc;
  ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
}


#define ROGUE_THREADS_WAIT_FOR_ALL Rogue_threads_wait_for_all();

void Rogue_threads_wait_for_all ()
{
  Rogue_mt_terminating = true;
  ROGUE_EXIT;
  int wait = 2; // Initial Xms
  int wait_step = 1;
  while (true)
  {
    ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);
    if (Rogue_mt_tc <= 1) // Shouldn't ever really be less than 1
    {
      ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
      break;
    }
    ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);
    usleep(1000 * wait);
    wait_step++;
    if (!(wait_step % 15) && (wait < 500)) wait *= 2; // Max backoff ~500ms
  }
  ROGUE_ENTER;
}

#else

#define ROGUE_THREADS_WAIT_FOR_ALL /* no-op if there's only one thread! */

static void Rogue_thread_register ()
{
}
static void Rogue_thread_unregister ()
{
}

#endif

// Singleton handling
#if ROGUE_THREAD_MODE
#define ROGUE_GET_SINGLETON(_S) (_S)->_singleton.load()
#define ROGUE_SET_SINGLETON(_S,_V) (_S)->_singleton.store(_V);
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
pthread_mutex_t Rogue_thread_singleton_lock;
#define ROGUE_SINGLETON_LOCK ROGUE_MUTEX_LOCK(Rogue_thread_singleton_lock);
#define ROGUE_SINGLETON_UNLOCK ROGUE_MUTEX_UNLOCK(Rogue_thread_singleton_lock);
#else
std::recursive_mutex Rogue_thread_singleton_lock;
#define ROGUE_SINGLETON_LOCK Rogue_thread_singleton_lock.lock();
#define ROGUE_SINGLETON_UNLOCK Rogue_thread_singleton_lock.unlock();
#endif
#else
#define ROGUE_GET_SINGLETON(_S) (_S)->_singleton
#define ROGUE_SET_SINGLETON(_S,_V) (_S)->_singleton = _V;
#define ROGUE_SINGLETON_LOCK
#define ROGUE_SINGLETON_UNLOCK
#endif

//-----------------------------------------------------------------------------
//  GC
//-----------------------------------------------------------------------------
#if ROGUE_GC_MODE_AUTO_MT
// See the Rogue MT GC diagram for an explanation of some of this.

#define ROGUE_GC_VAR static volatile int
// (Curiously, volatile seems to help performance slightly.)

static thread_local bool Rogue_mtgc_is_gc_thread = false;

#define ROGUE_MTGC_BARRIER asm volatile("" : : : "memory");

// Atomic LL insertion
#define ROGUE_LINKED_LIST_INSERT(__OLD,__NEW,__NEW_NEXT)            \
  for(;;) {                                                         \
    auto tmp = __OLD;                                               \
    __NEW_NEXT = tmp;                                               \
    if (__sync_bool_compare_and_swap(&(__OLD), tmp, __NEW)) break;  \
  }

// We assume malloc is safe, but the SOA needs safety if it's being used.
#if ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT >= 0
static ROGUE_MUTEX_DEF(Rogue_mtgc_soa_mutex);
#define ROGUE_GC_SOA_LOCK    ROGUE_MUTEX_LOCK(Rogue_mtgc_soa_mutex);
#define ROGUE_GC_SOA_UNLOCK  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_soa_mutex);
#else
#define ROGUE_GC_SOA_LOCK
#define ROGUE_GC_SOA_UNLOCK
#endif

static inline void Rogue_collect_garbage_real ();
void Rogue_collect_garbage_real_noinline ()
{
  Rogue_collect_garbage_real();
}

#if ROGUE_THREAD_MODE
#if ROGUE_THREAD_MODE_PTHREADS
#elif ROGUE_THREAD_MODE_CPP
#else
#error Currently, only --threads=pthreads and --threads=cpp are supported with --gc=auto-mt
#endif
#endif

// This is how unlikely() works in the Linux kernel
#define ROGUE_UNLIKELY(_X) __builtin_expect(!!(_X), 0)

#define ROGUE_GC_CHECK if (ROGUE_UNLIKELY(Rogue_mtgc_w) \
  && !ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread))          \
  Rogue_mtgc_W2_W3_W4(); // W1

 ROGUE_MUTEX_DEF(Rogue_mtgc_w_mutex);
static ROGUE_MUTEX_DEF(Rogue_mtgc_s_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_w_cond);
static ROGUE_COND_DEF(Rogue_mtgc_s_cond);

ROGUE_GC_VAR Rogue_mtgc_w = 0;
ROGUE_GC_VAR Rogue_mtgc_s = 0;

// Only one worker can be "running" (waiting for) the GC at a time.
// To run, set r = 1, and wait for GC to set it to 0.  If r is already
// 1, just wait.
static ROGUE_MUTEX_DEF(Rogue_mtgc_r_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_r_cond);
ROGUE_GC_VAR Rogue_mtgc_r = 0;

static ROGUE_MUTEX_DEF(Rogue_mtgc_g_mutex);
static ROGUE_COND_DEF(Rogue_mtgc_g_cond);
ROGUE_GC_VAR Rogue_mtgc_g = 0; // Should GC

static int Rogue_mtgc_should_quit = 0; // 0:normal 1:should-quit 2:has-quit

static ROGUE_THREAD_DEF(Rogue_mtgc_thread);

static void Rogue_mtgc_W2_W3_W4 (void);
static inline void Rogue_mtgc_W3_W4 (void);

inline void Rogue_mtgc_B1 ()
{
  ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, ++Rogue_mtgc_s);
}

static inline void Rogue_mtgc_B2_etc ()
{
  Rogue_mtgc_W3_W4();
  // We can probably just do GC_CHECK here rather than this more expensive
  // locking version.
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  auto w = Rogue_mtgc_w;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
  if (ROGUE_UNLIKELY(w)) Rogue_mtgc_W2_W3_W4(); // W1
}

static inline void Rogue_mtgc_W3_W4 ()
{
  // W3
  ROGUE_COND_WAIT(Rogue_mtgc_w_cond, Rogue_mtgc_w_mutex, Rogue_mtgc_w != 0);

  // W4
  ROGUE_MUTEX_LOCK(Rogue_mtgc_s_mutex);
  --Rogue_mtgc_s;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_s_mutex);
}

static void Rogue_mtgc_W2_W3_W4 ()
{
  // W2
  ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, ++Rogue_mtgc_s);
  Rogue_mtgc_W3_W4();
}


static thread_local int Rogue_mtgc_entered = 1;

inline void Rogue_mtgc_enter()
{
  if (ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread)) return;
  if (ROGUE_UNLIKELY(Rogue_mtgc_entered))
#ifdef ROGUE_MTGC_DEBUG
  {
    ROGUE_LOG_ERROR("ALREADY ENTERED\n");
    exit(1);
  }
#else
  {
    ++Rogue_mtgc_entered;
    return;
  }
#endif

  Rogue_mtgc_entered = 1;
  Rogue_mtgc_B2_etc();
}

inline void Rogue_mtgc_exit()
{
  if (ROGUE_UNLIKELY(Rogue_mtgc_is_gc_thread)) return;
  if (ROGUE_UNLIKELY(Rogue_mtgc_entered <= 0))
  {
    ROGUE_LOG_ERROR("Unabalanced Rogue enter/exit\n");
    exit(1);
  }

  --Rogue_mtgc_entered;
  Rogue_mtgc_B1();
}

static void Rogue_mtgc_M1_M2_GC_M3 (int quit)
{
  // M1
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  Rogue_mtgc_w = 1;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);

  // M2
#if (ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS) && ROGUE_MTGC_DEBUG
  ROGUE_MUTEX_LOCK(Rogue_mtgc_s_mutex);
  while (Rogue_mtgc_s != Rogue_mt_tc)
  {
    if (Rogue_mtgc_s > Rogue_mt_tc || Rogue_mtgc_s < 0)
    {
      ROGUE_LOG_ERROR("INVALID VALUE OF S %i %i\n", Rogue_mtgc_s, Rogue_mt_tc);
      exit(1);
    }

    pthread_cond_wait(&Rogue_mtgc_s_cond, &Rogue_mtgc_s_mutex);
  }
  // We should actually be okay holding the S lock until the
  // very end of the function if we want, and this would prevent
  // threads that were blocking from ever leaving B2.  But
  // We should be okay anyway, though S may temporarily != TC.
  //ROGUE_MUTEX_UNLOCK(Rogue_mtgc_s_mutex);
#else
  ROGUE_COND_STARTWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex);
  ROGUE_COND_DOWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex, Rogue_mtgc_s != Rogue_mt_tc);
#endif

#if ROGUE_MTGC_DEBUG
  ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
  Rogue_mtgc_w = 2;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
#endif

  // GC
  // Grab the SOA lock for symmetry.  It should actually never
  // be held by another thread since they're all in GC sleep.
  ROGUE_GC_SOA_LOCK;
  Rogue_collect_garbage_real();

  //NOTE: It's possible (Rogue_mtgc_s != Rogue_mt_tc) here, if we gave up the S
  //      lock, though they should quickly go back to equality.

  if (quit)
  {
    // Run a few more times to finish up
    Rogue_collect_garbage_real_noinline();
    Rogue_collect_garbage_real_noinline();

    // Free from the SOA
    RogueAllocator_free_all();
  }
  ROGUE_GC_SOA_UNLOCK;

  // M3
  ROGUE_COND_NOTIFY_ALL(Rogue_mtgc_w_cond, Rogue_mtgc_w_mutex, Rogue_mtgc_w = 0);

  // Could have done this much earlier
  ROGUE_COND_ENDWAIT(Rogue_mtgc_s_cond, Rogue_mtgc_s_mutex);
}

static void * Rogue_mtgc_threadproc (void *)
{
  Rogue_mtgc_is_gc_thread = true;
  int quit = 0;
  while (quit == 0)
  {
    ROGUE_COND_STARTWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    ROGUE_COND_DOWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex, !Rogue_mtgc_g && !Rogue_mtgc_should_quit);
    Rogue_mtgc_g = 0;
    quit = Rogue_mtgc_should_quit;
    ROGUE_COND_ENDWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);

    ROGUE_MUTEX_LOCK(Rogue_mt_thread_mutex);

    Rogue_mtgc_M1_M2_GC_M3(quit);

    ROGUE_MUTEX_UNLOCK(Rogue_mt_thread_mutex);

    ROGUE_COND_NOTIFY_ALL(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex, Rogue_mtgc_r = 0);
  }

  ROGUE_MUTEX_LOCK(Rogue_mtgc_g_mutex);
  Rogue_mtgc_should_quit = 2;
  Rogue_mtgc_g = 0;
  ROGUE_MUTEX_UNLOCK(Rogue_mtgc_g_mutex);
  return NULL;
}

// Cause GC to run and wait for a GC to complete.
void Rogue_mtgc_run_gc_and_wait ()
{
  bool again;
  do
  {
    again = false;
    ROGUE_COND_STARTWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex);
    if (Rogue_mtgc_r == 0)
    {
      Rogue_mtgc_r = 1;

      // Signal GC to run
      ROGUE_COND_NOTIFY_ONE(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex, Rogue_mtgc_g = 1);
    }
    else
    {
      // If one or more simultaneous requests to run the GC came in, run it
      // again.
      again = (Rogue_mtgc_r == 1);
      ++Rogue_mtgc_r;
    }
    ROGUE_EXIT;
    ROGUE_COND_DOWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex, Rogue_mtgc_r != 0);
    ROGUE_COND_ENDWAIT(Rogue_mtgc_r_cond, Rogue_mtgc_r_mutex);
    ROGUE_ENTER;
  }
  while (again);
}

static void Rogue_mtgc_quit_gc_thread ()
{
  //NOTE: This could probably be simplified (and the quit behavior removed
  //      from Rogue_mtgc_M1_M2_GC_M3) since we now wait for all threads
  //      to stop before calling this.
  // This doesn't quite use the normal condition variable pattern, sadly.
  ROGUE_EXIT;
  timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000000 * 10; // 10ms
  while (true)
  {
    bool done = true;
    ROGUE_COND_STARTWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    if (Rogue_mtgc_should_quit != 2)
    {
      done = false;
      Rogue_mtgc_g = 1;
      Rogue_mtgc_should_quit = 1;
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
      pthread_cond_signal(&Rogue_mtgc_g_cond);
#elif ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_CPP
      Rogue_mtgc_g_cond.notify_one();
#endif
    }
    ROGUE_COND_ENDWAIT(Rogue_mtgc_g_cond, Rogue_mtgc_g_mutex);
    if (done) break;
    nanosleep(&ts, NULL);
  }
  ROGUE_THREAD_JOIN(Rogue_mtgc_thread);
  ROGUE_ENTER;
}

void Rogue_configure_gc()
{
  int c = ROGUE_THREAD_START(Rogue_mtgc_thread, Rogue_mtgc_threadproc);
  if (c != 0)
  {
    exit(77); //TODO: Do something better in this (hopefully) rare case.
  }
}

// Used as part of the ROGUE_BLOCKING_CALL macro.
template<typename RT> RT Rogue_mtgc_reenter (RT expr)
{
  ROGUE_ENTER;
  return expr;
}

#include <atomic>

// We do all relaxed operations on this.  It's possible this will lead to
// something "bad", but I don't think *too* bad.  Like an extra GC.
// And I think that'll be rare, since the reset happens when all the
// threads are synced.  But I could be wrong.  Should probably think
// about this harder.
std::atomic_int Rogue_allocation_bytes_until_gc(Rogue_gc_threshold);
#define ROGUE_GC_COUNT_BYTES(__x) Rogue_allocation_bytes_until_gc.fetch_sub(__x, std::memory_order_relaxed);
#define ROGUE_GC_AT_THRESHOLD (Rogue_allocation_bytes_until_gc.load(std::memory_order_relaxed) <= 0)
#define ROGUE_GC_RESET_COUNT Rogue_allocation_bytes_until_gc.store(Rogue_gc_threshold, std::memory_order_relaxed);

#else // Anything besides auto-mt

#define ROGUE_GC_CHECK /* Does nothing in non-auto-mt modes */

#define ROGUE_GC_SOA_LOCK
#define ROGUE_GC_SOA_UNLOCK

int Rogue_allocation_bytes_until_gc = Rogue_gc_threshold;
#define ROGUE_GC_COUNT_BYTES(__x) Rogue_allocation_bytes_until_gc -= (__x);
#define ROGUE_GC_AT_THRESHOLD (Rogue_allocation_bytes_until_gc <= 0)
#define ROGUE_GC_RESET_COUNT Rogue_allocation_bytes_until_gc = Rogue_gc_threshold;


#define ROGUE_MTGC_BARRIER
#define ROGUE_LINKED_LIST_INSERT(__OLD,__NEW,__NEW_NEXT) do {__NEW_NEXT = __OLD; __OLD = __NEW;} while(false)

#endif

//-----------------------------------------------------------------------------
//  Misc Utility
//-----------------------------------------------------------------------------
void Rogue_define_literal_string( int index, const char* st, int count=-1 );

void Rogue_define_literal_string( int index, const char* st, int count )
{
  Rogue_literal_strings[index] = (RogueString*) RogueObject_retain( RogueString_create_from_utf8( st, count ) );
}

//-----------------------------------------------------------------------------
//  RogueDebugTrace
//-----------------------------------------------------------------------------
RogueDebugTrace::RogueDebugTrace( const char* method_signature, const char* filename, int line )
  : method_signature(method_signature), filename(filename), line(line), previous_trace(0)
{
  previous_trace = Rogue_call_stack;
  Rogue_call_stack = this;
}

RogueDebugTrace::~RogueDebugTrace()
{
  Rogue_call_stack = previous_trace;
}

int RogueDebugTrace::count()
{
  int n = 1;
  RogueDebugTrace* current = previous_trace;
  while (current)
  {
    ++n;
    current = current->previous_trace;
  }
  return n;
}

char* RogueDebugTrace::to_c_string()
{
  snprintf( buffer, 512, "[%s %s:%d]", method_signature, filename, line );
  return buffer;
}

//-----------------------------------------------------------------------------
//  RogueType
//-----------------------------------------------------------------------------
RogueArray* RogueType_create_array( int count, int element_size, bool is_reference_array, int element_type_index )
{
  if (count < 0) count = 0;
  int data_size  = count * element_size;
  int total_size = sizeof(RogueArray) + data_size;

  RogueArray* array = (RogueArray*) RogueAllocator_allocate_object( RogueTypeArray->allocator, RogueTypeArray, total_size, element_type_index);

  array->count = count;
  array->element_size = element_size;
  array->is_reference_array = is_reference_array;

  return array;
}

RogueObject* RogueType_create_object( RogueType* THIS, RogueInt32 size )
{
  ROGUE_DEF_LOCAL_REF_NULL(RogueObject*, obj);
  RogueInitFn  fn;
#if ROGUE_GC_MODE_BOEHM_TYPED
  ROGUE_DEBUG_STATEMENT(assert(size == 0 || size == THIS->object_size));
#endif

  obj = RogueAllocator_allocate_object( THIS->allocator, THIS, size ? size : THIS->object_size );

  if ((fn = THIS->init_object_fn)) return fn( obj );
  else                             return obj;
}

RogueLogical RogueType_instance_of( RogueType* THIS, RogueType* ancestor_type )
{
  if (THIS == ancestor_type)
  {
    return true;
  }

  int count = THIS->base_type_count;
  RogueType** base_type_ptr = THIS->base_types - 1;
  while (--count >= 0)
  {
    if (ancestor_type == *(++base_type_ptr))
    {
      return true;
    }
  }

  return false;
}

RogueString* RogueType_name( RogueType* THIS )
{
  return Rogue_literal_strings[ THIS->name_index ];
}

bool RogueType_name_equals( RogueType* THIS, const char* name )
{
  // For debugging purposes
  RogueString* st = Rogue_literal_strings[ THIS->name_index ];
  if ( !st ) return false;

  return (0 == strcmp((char*)st->utf8,name));
}

void RogueType_print_name( RogueType* THIS )
{
  RogueString* st = Rogue_literal_strings[ THIS->name_index ];
  if (st)
  {
    ROGUE_LOG( "%s", st->utf8 );
  }
}

RogueType* RogueType_retire( RogueType* THIS )
{
  if (THIS->base_types)
  {
#if !ROGUE_GC_MODE_BOEHM
    delete [] THIS->base_types;
#endif
    THIS->base_types = 0;
    THIS->base_type_count = 0;
  }

  return THIS;
}

RogueObject* RogueType_singleton( RogueType* THIS )
{
  RogueInitFn fn;
  RogueObject * r = ROGUE_GET_SINGLETON(THIS);
  if (r) return r;

  ROGUE_SINGLETON_LOCK;

#if ROGUE_THREAD_MODE // Very minor optimization: don't check twice if unthreaded
  // We probably need to initialize the singleton, but now that we have the
  // lock, we double check.
  r = ROGUE_GET_SINGLETON(THIS);
  if (r)
  {
    // Ah, someone else just initialized it.  We'll use that.
    ROGUE_SINGLETON_UNLOCK;
  }
  else
#endif
  {
    // Yes, we'll be the one doing the initializing.

    // NOTE: _singleton must be assigned before calling init_object()
    // so we can't just call RogueType_create_object().
    r = RogueAllocator_allocate_object( THIS->allocator, THIS, THIS->object_size );

    ROGUE_SET_SINGLETON(THIS, r);

    if ((fn = THIS->init_object_fn)) r = fn( ROGUE_ARG(r) );

    ROGUE_SINGLETON_UNLOCK;

    if ((fn = THIS->init_fn)) r = fn( THIS->_singleton );
  }

  return r;
}

//-----------------------------------------------------------------------------
//  RogueObject
//-----------------------------------------------------------------------------
RogueObject* RogueObject_as( RogueObject* THIS, RogueType* specialized_type )
{
  if (RogueObject_instance_of(THIS,specialized_type)) return THIS;
  return 0;
}

RogueLogical RogueObject_instance_of( RogueObject* THIS, RogueType* ancestor_type )
{
  if ( !THIS )
  {
    return false;
  }

  return RogueType_instance_of( THIS->type, ancestor_type );
}

RogueLogical RogueObject_is_type( RogueObject* THIS, RogueType* ancestor_type )
{
  return THIS ? (THIS->type == ancestor_type) : false;
}

void* RogueObject_retain( RogueObject* THIS )
{
  ROGUE_INCREF(THIS);
  return THIS;
}

void* RogueObject_release( RogueObject* THIS )
{
  ROGUE_DECREF(THIS);
  return THIS;
}

RogueString* RogueObject_to_string( RogueObject* THIS )
{
  RogueToStringFn fn = THIS->type->to_string_fn;
  if (fn) return fn( THIS );

  return Rogue_literal_strings[ THIS->type->name_index ];
}

void RogueObject_trace( void* obj )
{
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
}

void RogueString_trace( void* obj )
{
  if ( !obj || ((RogueObject*)obj)->object_size < 0 ) return;
  ((RogueObject*)obj)->object_size = ~((RogueObject*)obj)->object_size;
}

void RogueArray_trace( void* obj )
{
  int count;
  RogueObject** src;
  RogueArray* array = (RogueArray*) obj;

  if ( !array || array->object_size < 0 ) return;
  array->object_size = ~array->object_size;

  if ( !array->is_reference_array ) return;

  count = array->count;
  src = array->as_objects + count;
  while (--count >= 0)
  {
    RogueObject* cur = *(--src);
    if (cur && cur->object_size >= 0)
    {
      cur->type->trace_fn( cur );
    }
  }
}

//-----------------------------------------------------------------------------
//  RogueString
//-----------------------------------------------------------------------------
RogueString* RogueString_create_with_byte_count( int byte_count )
{
  if (byte_count < 0) byte_count = 0;

#if ROGUE_GC_MODE_BOEHM_TYPED
  RogueString* st = (RogueString*) RogueAllocator_allocate_object( RogueTypeString->allocator, RogueTypeString, RogueTypeString->object_size );
  char * data = (char *)GC_malloc_atomic_ignore_off_page( byte_count + 1 );
  data[0] = 0;
  data[byte_count] = 0;
  st->utf8 = (RogueByte*)data;
#else
  int total_size = sizeof(RogueString) + (byte_count+1);

  RogueString* st = (RogueString*) RogueAllocator_allocate_object( RogueTypeString->allocator, RogueTypeString, total_size );
#endif
  st->byte_count = byte_count;

  return st;
}

RogueString* RogueString_create_from_utf8( const char* utf8, int count )
{
  if (count == -1) count = (int) strlen( utf8 );

  if (count >= 3 && (unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF)
  {
    // Skip Byte Order Mark (BOM)
    utf8  += 3;
    count -= 3;
  }

  RogueString* st = RogueString_create_with_byte_count( count );
  memcpy( st->utf8, utf8, count );
  return RogueString_validate( st );
}

RogueString* RogueString_create_from_characters( RogueCharacter_List* characters )
{
  if ( !characters ) return RogueString_create_with_byte_count(0);

  RogueCharacter* data = characters->data->as_characters;
  int count = characters->count;
  int utf8_count = 0;
  for (int i=count; --i>=0; )
  {
    RogueCharacter ch = data[i];
    if (ch <= 0x7F)         ++utf8_count;
    else if (ch <= 0x7FF)   utf8_count += 2;
    else if (ch <= 0xFFFF)  utf8_count += 3;
    else                    utf8_count += 4;
  }

  RogueString* result = RogueString_create_with_byte_count( utf8_count );
  char*   dest = result->utf8;
  for (int i=0; i<count; ++i)
  {
    RogueCharacter ch = data[i];
    if (ch < 0)
    {
      *(dest++) = 0;
    }
    else if (ch <= 0x7F)
    {
      *(dest++) = (RogueByte) ch;
    }
    else if (ch <= 0x7FF)
    {
      dest[0] = (RogueByte) (0xC0 | ((ch >> 6) & 0x1F));
      dest[1] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 2;
    }
    else if (ch <= 0xFFFF)
    {
      dest[0] = (RogueByte) (0xE0 | ((ch >> 12) & 0xF));
      dest[1] = (RogueByte) (0x80 | ((ch >> 6) & 0x3F));
      dest[2] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 3;
    }
    else
    {
      dest[0] = (RogueByte) (0xF0 | ((ch >> 18) & 0x7));
      dest[1] = (RogueByte) (0x80 | ((ch >> 12) & 0x3F));
      dest[2] = (RogueByte) (0x80 | ((ch >> 6) & 0x3F));
      dest[3] = (RogueByte) (0x80 | (ch & 0x3F));
      dest += 4;
    }
  }

  result->character_count = count;

  return RogueString_validate( result );
}

void RogueString_print_string( RogueString* st )
{
  if (st)
  {
    RogueString_print_utf8( st->utf8, st->byte_count );
  }
  else
  {
    ROGUE_LOG( "null" );
  }
}

void RogueString_print_characters( RogueCharacter* characters, int count )
{
  if (characters)
  {
    RogueCharacter* src = characters - 1;
    while (--count >= 0)
    {
      int ch = *(++src);

      if (ch < 0)
      {
        putchar( 0 );
      }
      else if (ch < 0x80)
      {
        // %0xxxxxxx
        putchar( ch );
      }
      else if (ch < 0x800)
      {
        // %110xxxxx 10xxxxxx
        putchar( ((ch >> 6) & 0x1f) | 0xc0 );
        putchar( (ch & 0x3f) | 0x80 );
      }
      else if (ch <= 0xFFFF)
      {
        // %1110xxxx 10xxxxxx 10xxxxxx
        putchar( ((ch >> 12) & 15) | 0xe0 );
        putchar( ((ch >> 6) & 0x3f) | 0x80 );
        putchar( (ch & 0x3f) | 0x80 );
      }
      else
      {
        // Assume 21-bit
        // %11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        putchar( 0xf0 | ((ch>>18) & 7) );
        putchar( 0x80 | ((ch>>12) & 0x3f) );
        putchar( 0x80 | ((ch>>6)  & 0x3f) );
        putchar( (ch & 0x3f) | 0x80 );
      }
    }
  }
  else
  {
    ROGUE_LOG( "null" );
  }
}

void RogueString_print_utf8( char* utf8, int count )
{
  --utf8;
  while (--count >= 0)
  {
    putchar( *(++utf8) );
  }
}

RogueCharacter RogueString_character_at( RogueString* THIS, int index )
{
  if (THIS->is_ascii) return (RogueCharacter) THIS->utf8[ index ];

  RogueInt32 offset = RogueString_set_cursor( THIS, index );
  char* utf8 = THIS->utf8;

  RogueCharacter ch = utf8[ offset ];
  if (ch & 0x80)
  {
    if (ch & 0x20)
    {
      if (ch & 0x10)
      {
        return ((ch&7)<<18)
            | ((utf8[offset+1] & 0x3F) << 12)
            | ((utf8[offset+2] & 0x3F) << 6)
            | (utf8[offset+3] & 0x3F);
      }
      else
      {
        return ((ch&15)<<12)
            | ((utf8[offset+1] & 0x3F) << 6)
            | (utf8[offset+2] & 0x3F);
      }
    }
    else
    {
      return ((ch&31)<<6)
          | (utf8[offset+1] & 0x3F);
    }
  }
  else
  {
    return ch;
  }
}

RogueInt32 RogueString_set_cursor( RogueString* THIS, int index )
{
  // Sets this string's cursor_offset and cursor_index and returns cursor_offset.
  if (THIS->is_ascii)
  {
    return THIS->cursor_offset = THIS->cursor_index = index;
  }

  char* utf8 = THIS->utf8;

  RogueInt32 c_offset;
  RogueInt32 c_index;
  if (index == 0)
  {
    THIS->cursor_index = 0;
    return THIS->cursor_offset = 0;
  }
  else if (index >= THIS->character_count - 1)
  {
    c_offset = THIS->byte_count;
    c_index = THIS->character_count;
  }
  else
  {
    c_offset  = THIS->cursor_offset;
    c_index = THIS->cursor_index;
  }

  while (c_index < index)
  {
    while ((utf8[++c_offset] & 0xC0) == 0x80) {}
    ++c_index;
  }

  while (c_index > index)
  {
    while ((utf8[--c_offset] & 0xC0) == 0x80) {}
    --c_index;
  }

  THIS->cursor_index = c_index;
  return THIS->cursor_offset = c_offset;
}

RogueString* RogueString_validate( RogueString* THIS )
{
  // Trims any invalid UTF-8, counts the number of characters, and sets the hash code
  THIS->is_ascii = 1;  // assumption

  int character_count = 0;
  int byte_count = THIS->byte_count;
  int i;
  char* utf8 = THIS->utf8;
  for (i=0; i<byte_count; ++character_count)
  {
    int b = utf8[ i ];
    if (b & 0x80)
    {
      THIS->is_ascii = 0;
      if ( !(b & 0x40) ) { break;}  // invalid UTF-8

      if (b & 0x20)
      {
        if (b & 0x10)
        {
          // %11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
          if (b & 0x08) { break;}
          if (i + 4 > byte_count || ((utf8[i+1] & 0xC0) != 0x80) || ((utf8[i+2] & 0xC0) != 0x80)
              || ((utf8[i+3] & 0xC0) != 0x80)) { break;}
          i += 4;
        }
        else
        {
          // %1110xxxx 10xxxxxx 10xxxxxx
          if (i + 3 > byte_count || ((utf8[i+1] & 0xC0) != 0x80) || ((utf8[i+2] & 0xC0) != 0x80)) { break;}
          i += 3;
        }
      }
      else
      {
        // %110x xxxx 10xx xxxx
        if (i + 2 > byte_count || ((utf8[i+1] & 0xC0) != 0x80)) { break; }
        i += 2;
      }
    }
    else
    {
      ++i;
    }
  }

  //if (i != byte_count)
  //{
  //  ROGUE_LOG_ERROR( "*** RogueString validation error - invalid UTF8 (%d/%d):\n", i, byte_count );
  //  ROGUE_LOG_ERROR( "%02x\n", utf8[0] );
  //  ROGUE_LOG_ERROR( "%s\n", utf8 );
  //  utf8[ i ] = 0;
  //  Rogue_print_stack_trace();
  //}

  THIS->byte_count = i;
  THIS->character_count = character_count;

  int code = 0;
  int len = THIS->byte_count;
  char* src = THIS->utf8 - 1;
  while (--len >= 0)
  {
    code = ((code<<3) - code) + *(++src);
  }
  THIS->hash_code = code;
  return THIS;
}

//-----------------------------------------------------------------------------
//  RogueArray
//-----------------------------------------------------------------------------
RogueArray* RogueArray_set( RogueArray* THIS, RogueInt32 dest_i1, RogueArray* src_array, RogueInt32 src_i1, RogueInt32 copy_count )
{
  int element_size;
  int dest_i2, src_i2;

  if ( !src_array || dest_i1 >= THIS->count ) return THIS;
  if (THIS->is_reference_array ^ src_array->is_reference_array) return THIS;

  if (copy_count == -1) src_i2 = src_array->count - 1;
  else                  src_i2 = (src_i1 + copy_count) - 1;

  if (dest_i1 < 0)
  {
    src_i1 -= dest_i1;
    dest_i1 = 0;
  }

  if (src_i1 < 0) src_i1 = 0;
  if (src_i2 >= src_array->count) src_i2 = src_array->count - 1;
  if (src_i1 > src_i2) return THIS;

  copy_count = (src_i2 - src_i1) + 1;
  dest_i2 = dest_i1 + (copy_count - 1);
  if (dest_i2 >= THIS->count)
  {
    dest_i2 = (THIS->count - 1);
    copy_count = (dest_i2 - dest_i1) + 1;
  }
  if ( !copy_count ) return THIS;


#if defined(ROGUE_ARC)
  if (THIS != src_array || dest_i1 >= src_i1 + copy_count || (src_i1 + copy_count) <= dest_i1 || dest_i1 < src_i1)
  {
    // no overlap
    RogueObject** src  = src_array->as_objects + src_i1 - 1;
    RogueObject** dest = THIS->as_objects + dest_i1 - 1;
    while (--copy_count >= 0)
    {
      RogueObject* src_obj, dest_obj;
      if ((src_obj = *(++src))) ROGUE_INCREF(src_obj);
      if ((dest_obj = *(++dest)) && !(ROGUE_DECREF(dest_obj)))
      {
        // TODO: delete dest_obj
        *dest = src_obj;
      }
    }
  }
  else
  {
    // Copying earlier data to later data; copy in reverse order to
    // avoid accidental overwriting
    if (dest_i1 > src_i1)  // if they're equal then we don't need to copy anything!
    {
      RogueObject** src  = src_array->as_objects + src_i2 + 1;
      RogueObject** dest = THIS->as_objects + dest_i2 + 1;
      while (--copy_count >= 0)
      {
        RogueObject* src_obj, dest_obj;
        if ((src_obj = *(--src))) ROGUE_INCREF(src_obj);
        if ((dest_obj = *(--dest)) && !(ROGUE_DECREF(dest_obj)))
        {
          // TODO: delete dest_obj
          *dest = src_obj;
        }
      }
    }
  }
  return THIS;
#endif

  element_size = THIS->element_size;
  RogueByte* src = src_array->as_bytes + src_i1 * element_size;
  RogueByte* dest = THIS->as_bytes + (dest_i1 * element_size);
  int copy_bytes = copy_count * element_size;

  if (src == dest) return THIS;

  if (src >= dest + copy_bytes || (src + copy_bytes) <= dest)
  {
    // Copy region does not overlap
    memcpy( dest, src, copy_count * element_size );
  }
  else
  {
    // Copy region overlaps
    memmove( dest, src, copy_count * element_size );
  }

  return THIS;
}

//-----------------------------------------------------------------------------
//  RogueAllocationPage
//-----------------------------------------------------------------------------
RogueAllocationPage* RogueAllocationPage_create( RogueAllocationPage* next_page )
{
  RogueAllocationPage* result = (RogueAllocationPage*) ROGUE_NEW_BYTES(sizeof(RogueAllocationPage));
  result->next_page = next_page;
  result->cursor = result->data;
  result->remaining = ROGUEMM_PAGE_SIZE;
  return result;
}

#if 0 // This is currently done statically.  Code likely to be removed.
RogueAllocationPage* RogueAllocationPage_delete( RogueAllocationPage* THIS )
{
  if (THIS) ROGUE_DEL_BYTES( THIS );
  return 0;
};
#endif

void* RogueAllocationPage_allocate( RogueAllocationPage* THIS, int size )
{
  // Round size up to multiple of 8.
  if (size > 0) size = (size + 7) & ~7;
  else          size = 8;

  if (size > THIS->remaining) return 0;

  //ROGUE_LOG( "Allocating %d bytes from page.\n", size );
  void* result = THIS->cursor;
  THIS->cursor += size;
  THIS->remaining -= size;
  ((RogueObject*)result)->reference_count = 0;

  //ROGUE_LOG( "%d / %d\n", ROGUEMM_PAGE_SIZE - remaining, ROGUEMM_PAGE_SIZE );
  return result;
}


//-----------------------------------------------------------------------------
//  RogueAllocator
//-----------------------------------------------------------------------------
#if 0 // This is currently done statically.  Code likely to be removed.
RogueAllocator* RogueAllocator_create()
{
  RogueAllocator* result = (RogueAllocator*) ROGUE_NEW_BYTES( sizeof(RogueAllocator) );

  memset( result, 0, sizeof(RogueAllocator) );

  return result;
}

RogueAllocator* RogueAllocator_delete( RogueAllocator* THIS )
{
  while (THIS->pages)
  {
    RogueAllocationPage* next_page = THIS->pages->next_page;
    RogueAllocationPage_delete( THIS->pages );
    THIS->pages = next_page;
  }
  return 0;
}
#endif

void* RogueAllocator_allocate( RogueAllocator* THIS, int size )
{
#if ROGUE_GC_MODE_AUTO_MT
#if ROGUE_MTGC_DEBUG
    ROGUE_MUTEX_LOCK(Rogue_mtgc_w_mutex);
    if (Rogue_mtgc_w == 2)
    {
      ROGUE_LOG_ERROR("ALLOC DURING GC!\n");
      exit(1);
    }
    ROGUE_MUTEX_UNLOCK(Rogue_mtgc_w_mutex);
#endif
#endif

#if ROGUE_GC_MODE_AUTO_ANY
  Rogue_collect_garbage();
#endif
  if (size > ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT)
  {
    ROGUE_GC_COUNT_BYTES(size);
    void * mem = ROGUE_NEW_BYTES(size);
#if ROGUE_GC_MODE_AUTO_ANY
    if (!mem)
    {
      // Try hard!
      Rogue_collect_garbage(true);
      mem = ROGUE_NEW_BYTES(size);
    }
#endif
    return mem;
  }

  ROGUE_GC_SOA_LOCK;

  size = (size > 0) ? (size + ROGUEMM_GRANULARITY_MASK) & ~ROGUEMM_GRANULARITY_MASK : ROGUEMM_GRANULARITY_SIZE;

  ROGUE_GC_COUNT_BYTES(size);

  int slot;
  ROGUE_DEF_LOCAL_REF(RogueObject*, obj, THIS->available_objects[(slot=(size>>ROGUEMM_GRANULARITY_BITS))]);

  if (obj)
  {
    //ROGUE_LOG( "found free object\n");
    THIS->available_objects[slot] = obj->next_object;
    ROGUE_GC_SOA_UNLOCK;
    return obj;
  }

  // No free objects for requested size.

  // Try allocating a new object from the current page.
  if (THIS->pages )
  {
    obj = (RogueObject*) RogueAllocationPage_allocate( THIS->pages, size );
    if (obj)
    {
      ROGUE_GC_SOA_UNLOCK;
      return obj;
    }

    // Not enough room on allocation page.  Allocate any smaller blocks
    // we're able to and then move on to a new page.
    int s = slot - 1;
    while (s >= 1)
    {
      obj = (RogueObject*) RogueAllocationPage_allocate( THIS->pages, s << ROGUEMM_GRANULARITY_BITS );
      if (obj)
      {
        //ROGUE_LOG( "free obj size %d\n", (s << ROGUEMM_GRANULARITY_BITS) );
        obj->next_object = THIS->available_objects[s];
        THIS->available_objects[s] = obj;
      }
      else
      {
        --s;
      }
    }
  }

  // New page; this will work for sure.
  THIS->pages = RogueAllocationPage_create( THIS->pages );
  void * r = RogueAllocationPage_allocate( THIS->pages, size );
  ROGUE_GC_SOA_UNLOCK;
  return r;
}

#if ROGUE_GC_MODE_BOEHM
void Rogue_Boehm_Finalizer( void* obj, void* data )
{
  RogueObject* o = (RogueObject*)obj;
  o->type->on_cleanup_fn(o);
}

RogueObject* RogueAllocator_allocate_object( RogueAllocator* THIS, RogueType* of_type, int size, int element_type_index )
{
  // We use the "off page" allocations here, which require that somewhere there's a pointer
  // to something within the first 256 bytes.  Since someone should always be holding a
  // reference to the absolute start of the allocation (a reference!), this should always
  // be true.
#if ROGUE_GC_MODE_BOEHM_TYPED
  RogueObject * obj;
  if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_TYPED)
  {
    obj = (RogueObject*)GC_malloc_explicitly_typed_ignore_off_page(of_type->object_size, of_type->gc_type_descr);
  }
  else if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_ATOMIC)
  {
    obj = (RogueObject*)GC_malloc_atomic_ignore_off_page( of_type->object_size );
    if (obj) memset( obj, 0, of_type->object_size );
  }
  else
  {
    obj = (RogueObject*)GC_malloc_ignore_off_page( of_type->object_size );
  }
  if (!obj)
  {
    Rogue_collect_garbage( true );
    obj = (RogueObject*)GC_MALLOC( of_type->object_size );
  }
  obj->object_size = of_type->object_size;
#else
  RogueObject * obj = (RogueObject*)GC_malloc_ignore_off_page( size );
  if (!obj)
  {
    Rogue_collect_garbage( true );
    obj = (RogueObject*)GC_MALLOC( size );
  }
  obj->object_size = size;
#endif

  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
  ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(of_type) );
  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)obj ) );
  //ROGUE_GCDEBUG_STATEMENT( Rogue_print_stack_trace() );

#if ROGUE_GC_MODE_BOEHM_TYPED
  // In typed mode, we allocate the array object and the actual data independently so that
  // they can have different GC types.
  if (element_type_index != -1)
  {
    RogueType* el_type = &Rogue_types[element_type_index];
    int data_size = size - of_type->object_size;
    int elements = data_size / el_type->object_size;
    void * data;
    if (el_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_TYPED)
    {
      data = GC_calloc_explicitly_typed(elements, el_type->object_size, el_type->gc_type_descr);
    }
    else if (of_type->gc_alloc_type == ROGUE_GC_ALLOC_TYPE_ATOMIC)
    {
      data = GC_malloc_atomic_ignore_off_page( data_size );
      if (data) memset( obj, 0, data_size );
    }
    else
    {
      data = GC_malloc_ignore_off_page( data_size );
    }
    ((RogueArray*)obj)->as_bytes = (RogueByte*)data;
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "  Elements " ) );
    ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(el_type) );
    ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)data ) );
  }
#endif

  if (of_type->on_cleanup_fn)
  {
    GC_REGISTER_FINALIZER_IGNORE_SELF( obj, Rogue_Boehm_Finalizer, 0, 0, 0 );
  }

  obj->type = of_type;

  return obj;
}
#else
RogueObject* RogueAllocator_allocate_object( RogueAllocator* THIS, RogueType* of_type, int size, int element_type_index )
{
  void * mem = RogueAllocator_allocate( THIS, size );
  memset( mem, 0, size );

  ROGUE_DEF_LOCAL_REF(RogueObject*, obj, (RogueObject*)mem);

  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Allocating " ) );
  ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(of_type) );
  ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", (RogueObject*)obj ) );
  //ROGUE_GCDEBUG_STATEMENT( Rogue_print_stack_trace() );

  obj->type = of_type;
  obj->object_size = size;

  ROGUE_MTGC_BARRIER; // Probably not necessary

  if (of_type->on_cleanup_fn)
  {
    ROGUE_LINKED_LIST_INSERT(THIS->objects_requiring_cleanup, obj, obj->next_object);
  }
  else
  {
    ROGUE_LINKED_LIST_INSERT(THIS->objects, obj, obj->next_object);
  }

  return obj;
}
#endif

void* RogueAllocator_free( RogueAllocator* THIS, void* data, int size )
{
  if (data)
  {
    ROGUE_GCDEBUG_STATEMENT(memset(data,0,size));
    if (size > ROGUEMM_SMALL_ALLOCATION_SIZE_LIMIT)
    {
      // When debugging GC, it can be very useful to log the object types of
      // freed objects.  When valgrind points out access to freed memory, you
      // can then see what it was.
      #if 0
      RogueObject* obj = (RogueObject*) data;
      ROGUE_LOG("DEL %i %p ", (int)pthread_self(), data);
      RogueType_print_name( obj-> type );
      ROGUE_LOG("\n");
      #endif
      ROGUE_DEL_BYTES( data );
    }
    else
    {
      // Return object to small allocation pool
      RogueObject* obj = (RogueObject*) data;
      int slot = (size + ROGUEMM_GRANULARITY_MASK) >> ROGUEMM_GRANULARITY_BITS;
      if (slot <= 0) slot = 1;
      obj->next_object = THIS->available_objects[slot];
      THIS->available_objects[slot] = obj;
    }
  }

  // Always returns null, allowing a pointer to be freed and assigned null in
  // a single step.
  return 0;
}


void RogueAllocator_free_objects( RogueAllocator* THIS )
{
  RogueObject* objects = THIS->objects;
  while (objects)
  {
    RogueObject* next_object = objects->next_object;
    RogueAllocator_free( THIS, objects, objects->object_size );
    objects = next_object;
  }

  THIS->objects = 0;
}

void RogueAllocator_free_all( )
{
  for (int i=0; i<Rogue_allocator_count; ++i)
  {
    RogueAllocator_free_objects( &Rogue_allocators[i] );
  }
}

void RogueAllocator_collect_garbage( RogueAllocator* THIS )
{
  // Global program objects have already been traced through.

  // Trace through all as-yet unreferenced objects that are manually retained.
  RogueObject* cur = THIS->objects;
  while (cur)
  {
    if (cur->object_size >= 0 && cur->reference_count > 0)
    {
      cur->type->trace_fn( cur );
    }
    cur = cur->next_object;
  }

  cur = THIS->objects_requiring_cleanup;
  while (cur)
  {
    if (cur->object_size >= 0 && cur->reference_count > 0)
    {
      cur->type->trace_fn( cur );
    }
    cur = cur->next_object;
  }

  // For any unreferenced objects requiring clean-up, we'll:
  //   1.  Reference them and move them to a separate short-term list.
  //   2.  Finish the regular GC.
  //   3.  Call on_cleanup() on each of them, which may create new
  //       objects (which is why we have to wait until after the GC).
  //   4.  Move them to the list of regular objects.
  cur = THIS->objects_requiring_cleanup;
  RogueObject* unreferenced_on_cleanup_objects = 0;
  RogueObject* survivors = 0;  // local var for speed
  while (cur)
  {
    RogueObject* next_object = cur->next_object;
    if (cur->object_size < 0)
    {
      // Referenced.
      cur->next_object = survivors;
      survivors = cur;
    }
    else
    {
      // Unreferenced - go ahead and trace it since we'll call on_cleanup
      // on it.
      cur->type->trace_fn( cur );
      cur->next_object = unreferenced_on_cleanup_objects;
      unreferenced_on_cleanup_objects = cur;
    }
    cur = next_object;
  }
  THIS->objects_requiring_cleanup = survivors;

  // All objects are in a state where a non-negative size means that the object is
  // due to be deleted.
  Rogue_on_gc_trace_finished.call();

  // Now that on_gc_trace_finished() has been called we can reset the "collected" status flag
  // on all objects requiring cleanup.
  cur = THIS->objects_requiring_cleanup;
  while (cur)
  {
    if (cur->object_size < 0) cur->object_size = ~cur->object_size;
    cur = cur->next_object;
  }

  // Reset or delete each general object
  cur = THIS->objects;
  THIS->objects = 0;
  survivors = 0;  // local var for speed

  while (cur)
  {
    RogueObject* next_object = cur->next_object;
    if (cur->object_size < 0)
    {
      cur->object_size = ~cur->object_size;
      cur->next_object = survivors;
      survivors = cur;
    }
    else
    {
      ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( "Freeing " ) );
      ROGUE_GCDEBUG_STATEMENT( RogueType_print_name(cur->type) );
      ROGUE_GCDEBUG_STATEMENT( ROGUE_LOG( " %p\n", cur ) );
      RogueAllocator_free( THIS, cur, cur->object_size );
    }
    cur = next_object;
  }

  THIS->objects = survivors;


  // Call on_cleanup() on unreferenced objects requiring cleanup
  // and move them to the general objects list so they'll be deleted
  // the next time they're unreferenced.  Calling on_cleanup() may
  // create additional objects so THIS->objects may change during a
  // on_cleanup() call.
  cur = unreferenced_on_cleanup_objects;
  while (cur)
  {
    RogueObject* next_object = cur->next_object;

    cur->type->on_cleanup_fn( cur );

    cur->object_size = ~cur->object_size;
    cur->next_object = THIS->objects;
    THIS->objects = cur;

    cur = next_object;
  }

  if (Rogue_gc_logging)
  {
    int byte_count = 0;
    int object_count = 0;

    for (int i=0; i<Rogue_allocator_count; ++i)
    {
      RogueAllocator* allocator = &Rogue_allocators[i];

      RogueObject* cur = allocator->objects;
      while (cur)
      {
        ++object_count;
        byte_count += cur->object_size;
        cur = cur->next_object;
      }

      cur = allocator->objects_requiring_cleanup;
      while (cur)
      {
        ++object_count;
        byte_count += cur->object_size;
        cur = cur->next_object;
      }
    }

    ROGUE_LOG( "Post-GC: %d objects, %d bytes used.\n", object_count, byte_count );
  }
}

void Rogue_print_stack_trace ( bool leading_newline )
{
  RogueDebugTrace* current = Rogue_call_stack;
  if (current && leading_newline) ROGUE_LOG( "\n" );
  while (current)
  {
    ROGUE_LOG( "%s\n", current->to_c_string() );
    current = current->previous_trace;
  }
  ROGUE_LOG("\n");
}

#if defined(ROGUE_PLATFORM_WINDOWS)
void Rogue_segfault_handler( int signal )
{
  ROGUE_LOG_ERROR( "Access violation\n" );
#else
void Rogue_segfault_handler( int signal, siginfo_t *si, void *arg )
{
  if (si->si_addr < (void*)4096)
  {
    // Probably a null pointer dereference.
    ROGUE_LOG_ERROR( "Null reference error (accessing memory at %p)\n",
            si->si_addr );
  }
  else
  {
    if (si->si_code == SEGV_MAPERR)
      ROGUE_LOG_ERROR( "Access to unmapped memory at " );
    else if (si->si_code == SEGV_ACCERR)
      ROGUE_LOG_ERROR( "Access to forbidden memory at " );
    else
      ROGUE_LOG_ERROR( "Unknown segfault accessing " );
    ROGUE_LOG_ERROR("%p\n", si->si_addr);
  }
#endif

  Rogue_print_stack_trace( true );

  exit(1);
}

void Rogue_update_weak_references_during_gc()
{
  RogueWeakReference* cur = Rogue_weak_references;
  while (cur)
  {
    if (cur->value && cur->value->object_size >= 0)
    {
      // The value held by this weak reference is about to be deleted by the
      // GC system; null out the value.
      cur->value = 0;
    }
    cur = cur->next_weak_reference;
  }
}


void Rogue_configure_types()
{
#if ROGUE_THREAD_MODE == ROGUE_THREAD_MODE_PTHREADS
_rogue_init_mutex(&Rogue_thread_singleton_lock);
#endif

  int i;
  const int* next_type_info = Rogue_type_info_table;

#if defined(ROGUE_PLATFORM_WINDOWS)
  // Use plain old signal() instead of sigaction()
  signal( SIGSEGV, Rogue_segfault_handler );
#else
  // Install seg fault handler
  struct sigaction sa;

  memset( &sa, 0, sizeof(sa) );
  sigemptyset( &sa.sa_mask );
  sa.sa_sigaction = Rogue_segfault_handler;
  sa.sa_flags     = SA_SIGINFO;

  sigaction( SIGSEGV, &sa, NULL );
#endif

  // Initialize allocators
  memset( Rogue_allocators, 0, sizeof(RogueAllocator)*Rogue_allocator_count );

#ifdef ROGUE_INTROSPECTION
  int global_property_pointer_cursor = 0;
  int property_offset_cursor = 0;
#endif

  // Initialize types
  for (i=0; i<Rogue_type_count; ++i)
  {
    int j;
    RogueType* type = &Rogue_types[i];
    const int* type_info = next_type_info;
    next_type_info += *(type_info++) + 1;

    memset( type, 0, sizeof(RogueType) );

    type->index = i;
    type->name_index = Rogue_type_name_index_table[i];
    type->object_size = Rogue_object_size_table[i];
#ifdef ROGUE_INTROSPECTION
    type->attributes = Rogue_attributes_table[i];
#endif
    type->allocator = &Rogue_allocators[ *(type_info++) ];
    type->methods = Rogue_dynamic_method_table + *(type_info++);
    type->base_type_count = *(type_info++);
    if (type->base_type_count)
    {
#if ROGUE_GC_MODE_BOEHM
      type->base_types = new (NoGC) RogueType*[ type->base_type_count ];
#else
      type->base_types = new RogueType*[ type->base_type_count ];
#endif
      for (j=0; j<type->base_type_count; ++j)
      {
        type->base_types[j] = &Rogue_types[ *(type_info++) ];
      }
    }

    type->global_property_count = *(type_info++);
    type->global_property_name_indices = type_info;
    type_info += type->global_property_count;
    type->global_property_type_indices = type_info;
    type_info += type->global_property_count;

    type->property_count = *(type_info++);
    type->property_name_indices = type_info;
    type_info += type->property_count;
    type->property_type_indices = type_info;
    type_info += type->property_count;

#if ROGUE_GC_MODE_BOEHM_TYPED
    type->gc_alloc_type = *(type_info++);
#endif

#ifdef ROGUE_INTROSPECTION
    if (((type->attributes & ROGUE_ATTRIBUTE_TYPE_MASK) == ROGUE_ATTRIBUTE_IS_CLASS)
      || ((type->attributes & ROGUE_ATTRIBUTE_TYPE_MASK) == ROGUE_ATTRIBUTE_IS_COMPOUND))
    {
      type->global_property_pointers = Rogue_global_property_pointers + global_property_pointer_cursor;
      global_property_pointer_cursor += type->global_property_count;
      type->property_offsets = Rogue_property_offsets + property_offset_cursor;
      property_offset_cursor += type->property_count;
    }
#endif
    type->method_count = *(type_info++);
    type->global_method_count = *(type_info++);

    type->trace_fn = Rogue_trace_fn_table[i];
    type->init_object_fn = Rogue_init_object_fn_table[i];
    type->init_fn        = Rogue_init_fn_table[i];
    type->on_cleanup_fn  = Rogue_on_cleanup_fn_table[i];
    type->to_string_fn   = Rogue_to_string_fn_table[i];

    ROGUE_DEBUG_STATEMENT(assert(type_info <= next_type_info));
  }

  Rogue_on_gc_trace_finished.add( Rogue_update_weak_references_during_gc );

#if ROGUE_GC_MODE_BOEHM_TYPED
  Rogue_init_boehm_type_info();
#endif
}

#if ROGUE_GC_MODE_BOEHM
static GC_ToggleRefStatus Rogue_Boehm_ToggleRefStatus( void * o )
{
  RogueObject* obj = (RogueObject*)o;
  if (obj->reference_count > 0) return GC_TOGGLE_REF_STRONG;
  return GC_TOGGLE_REF_DROP;
}

static void Rogue_Boehm_on_collection_event( GC_EventType event )
{
  if (event == GC_EVENT_START)
  {
    Rogue_on_gc_begin.call();
  }
  else if (event == GC_EVENT_END)
  {
    Rogue_on_gc_end.call();
  }
}

void Rogue_configure_gc()
{
  // Initialize Boehm collector
  //GC_set_finalize_on_demand(0);
  GC_set_toggleref_func(Rogue_Boehm_ToggleRefStatus);
  GC_set_on_collection_event(Rogue_Boehm_on_collection_event);
  //GC_set_all_interior_pointers(0);
  GC_INIT();
}
#elif ROGUE_GC_MODE_AUTO_MT
// Rogue_configure_gc already defined above.
#else
void Rogue_configure_gc()
{
}
#endif

#if ROGUE_GC_MODE_BOEHM
bool Rogue_collect_garbage( bool forced )
{
  if (forced)
  {
    GC_gcollect();
    return true;
  }

  return GC_collect_a_little();
}
#else // Auto or manual

static inline void Rogue_collect_garbage_real(void);

bool Rogue_collect_garbage( bool forced )
{
  if (!forced && !Rogue_gc_requested & !ROGUE_GC_AT_THRESHOLD) return false;

#if ROGUE_GC_MODE_AUTO_MT
  Rogue_mtgc_run_gc_and_wait();
#else
  Rogue_collect_garbage_real();
#endif

  return true;
}

static inline void Rogue_collect_garbage_real()
{
  Rogue_gc_requested = false;
  if (Rogue_gc_active) return;
  Rogue_gc_active = true;
  ++ Rogue_gc_count;

//ROGUE_LOG( "GC %d\n", Rogue_allocation_bytes_until_gc );
  ROGUE_GC_RESET_COUNT;

  Rogue_on_gc_begin.call();

  Rogue_trace();

  for (int i=0; i<Rogue_allocator_count; ++i)
  {
    RogueAllocator_collect_garbage( &Rogue_allocators[i] );
  }

  Rogue_on_gc_end.call();
  Rogue_gc_active = false;
}

#endif

void Rogue_quit()
{
  int i;

  if ( !Rogue_configured ) return;
  Rogue_configured = 0;

  RogueGlobal__call_exit_functions( (RogueClassGlobal*) ROGUE_SINGLETON(Global) );

  ROGUE_THREADS_WAIT_FOR_ALL;

#if ROGUE_GC_MODE_AUTO_MT
  Rogue_mtgc_quit_gc_thread();
#else
  // Give a few GC's to allow objects requiring clean-up to do so.
  Rogue_collect_garbage( true );
  Rogue_collect_garbage( true );
  Rogue_collect_garbage( true );

  RogueAllocator_free_all();
#endif

  for (i=0; i<Rogue_type_count; ++i)
  {
    RogueType_retire( &Rogue_types[i] );
  }

  Rogue_thread_unregister();

  Rogue_gc_logging = false;
  Rogue_gc_count = 0;
  Rogue_gc_requested = 0;
  Rogue_gc_active = 0;
  Rogue_call_stack = 0;
  Rogue_weak_references = 0;
}

#if ROGUE_GC_MODE_BOEHM

void Rogue_Boehm_IncRef (RogueObject* o)
{
  ++o->reference_count;
  if (o->reference_count == 1)
  {
    GC_toggleref_add(o, 1);
  }
}
void Rogue_Boehm_DecRef (RogueObject* o)
{
  --o->reference_count;
  if (o->reference_count < 0)
  {
    o->reference_count = 0;
  }
}
#endif


//-----------------------------------------------------------------------------
//  Exception handling
//-----------------------------------------------------------------------------
void Rogue_terminate_handler()
{
  ROGUE_LOG_ERROR( "Uncaught exception.\n" );
  exit(1);
}
//=============================================================================
