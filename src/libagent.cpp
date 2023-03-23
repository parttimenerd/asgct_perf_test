
#include "jvmti.h"
#include <algorithm>
#include <assert.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <random>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <array>
#include <atomic>
#include <ucontext.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <sys/resource.h>

/** maximum size of stack trace arrays */
const int MAX_DEPTH = 1024;

static jvmtiEnv *jvmti;
static JavaVM *jvm;
static JNIEnv *env;

typedef void (*SigAction)(int, siginfo_t *, void *);
typedef void (*SigHandler)(int);
typedef void (*TimerCallback)(void *);

static SigAction installSignalHandler(int signo, SigAction action,
                                      SigHandler handler = nullptr) {
  struct sigaction sa;
  struct sigaction oldsa;
  sigemptyset(&sa.sa_mask);

  if (handler != nullptr) {
    sa.sa_handler = handler;
    sa.sa_flags = 0;
  } else {
    sa.sa_sigaction = action;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
  }

  sigaction(signo, &sa, &oldsa);
  return oldsa.sa_sigaction;
}

void ensureSuccess(jvmtiError err, const char *msg) {
  if (err != JVMTI_ERROR_NONE) {
    fprintf(stderr, "Error in %s: %d", msg, err);
    exit(1);
  }
}

template <class T> class JvmtiDeallocator {
public:
  JvmtiDeallocator() { elem_ = nullptr; }

  ~JvmtiDeallocator() {
    if (elem_ != nullptr) {
      jvmti->Deallocate(reinterpret_cast<unsigned char *>(elem_));
    }
  }

  T *get_addr() { return &elem_; }

  T get() { return elem_; }

private:
  T elem_;
};

pthread_t get_thread_id() {
#if defined(__APPLE__) && defined(__MACH__)
  return pthread_self();
#else
  return (pthread_t)syscall(SYS_gettid);
#endif
}

std::recursive_mutex
    threadToJavaIdMutex; // hold this mutex while working with threadToJavaId
std::unordered_map<pthread_t, jlong> threadToJavaId;

struct ThreadState {
  pthread_t thread;
};

jlong obtainJavaThreadIdViaJava(JNIEnv *env, jthread thread) {
  if (env == nullptr) {
    return -1;
  }
  jclass threadClass = env->FindClass("java/lang/Thread");
  jmethodID getId = env->GetMethodID(threadClass, "getId", "()J");
  jlong id = env->CallLongMethod(thread, getId);
  return id;
}

/** returns the jthread for a given Java thread id or null */
jthread getJThreadForPThread(JNIEnv *env, pthread_t threadId) {
  std::lock_guard<std::recursive_mutex> lock(threadToJavaIdMutex);
  std::vector<jthread> threadVec;
  JvmtiDeallocator<jthread *> threads;
  jint thread_count = 0;
  jvmti->GetAllThreads(&thread_count, threads.get_addr());
  for (int i = 0; i < thread_count; i++) {
    jthread thread = threads.get()[i];
    ThreadState *state;
    jvmti->GetThreadLocalStorage(thread, (void **)&state);
    if (state == nullptr) {
      continue;
    }
    if (state->thread == threadId) {
      return thread;
    }
  }
  return nullptr;
}

std::atomic<bool> shouldStop;

static void sampleLoop();

std::thread samplerThread;

void printInfo();

void printInfoIfNeeded();

void onAbort() {
  shouldStop = true;
  if (samplerThread.joinable()) {
    samplerThread.join();
  }
}

void OnThreadStart(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
  {
    std::lock_guard<std::recursive_mutex> lock(threadToJavaIdMutex);
    threadToJavaId.emplace(get_thread_id(),
                           obtainJavaThreadIdViaJava(jni_env, thread));
  }
  jvmti_env->SetThreadLocalStorage(
      thread, new ThreadState({(pthread_t)get_thread_id()}));
}

void OnThreadEnd(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
  std::lock_guard<std::recursive_mutex> lock(threadToJavaIdMutex);
  threadToJavaId.erase(get_thread_id());
  printInfoIfNeeded();
}

static void GetJMethodIDs(jclass klass) {
  jint method_count = 0;
  JvmtiDeallocator<jmethodID *> methods;
  jvmti->GetClassMethods(klass, &method_count, methods.get_addr());
}

// AsyncGetCallTrace needs class loading events to be turned on!
static void JNICALL OnClassLoad(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                jthread thread, jclass klass) {}

static void JNICALL OnClassPrepare(jvmtiEnv *jvmti, JNIEnv *jni_env,
                                   jthread thread, jclass klass) {
  // We need to do this to "prime the pump" and get jmethodIDs primed.
  GetJMethodIDs(klass);
}

static void startSamplerThread();

static void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  env = jni_env;
  jint class_count = 0;

  // Get any previously loaded classes that won't have gone through the
  // OnClassPrepare callback to prime the jmethods for AsyncGetCallTrace.
  // else the jmethods are all nullptr. This might still happen if ASGCT is
  // called at the very beginning, while this code is executed. But this is not
  // a problem in the typical use case.
  JvmtiDeallocator<jclass *> classes;
  jvmtiError err = jvmti->GetLoadedClasses(&class_count, classes.get_addr());
  if (err != JVMTI_ERROR_NONE) {
    return;
  }

  // Prime any class already loaded and try to get the jmethodIDs set up.
  jclass *classList = classes.get();
  for (int i = 0; i < class_count; ++i) {
    GetJMethodIDs(classList[i]);
  }

  startSamplerThread();
}

// A copy of the ASGCT data structures.
typedef struct {
  jint lineno;         // line number in the source file
  jmethodID method_id; // method executed in this frame
} ASGCT_CallFrame;

typedef struct {
  JNIEnv *env_id;          // Env where trace was recorded
  jint num_frames;         // number of frames in this trace
  ASGCT_CallFrame *frames; // frames
} ASGCT_CallTrace;

typedef void (*ASGCTType)(ASGCT_CallTrace *, jint, void *);

ASGCTType asgct;

static void signalHandler(int signum, siginfo_t *info, void *ucontext);

static void startSamplerThread() {
  samplerThread = std::thread(sampleLoop);
  installSignalHandler(SIGPROF, signalHandler);
}

static int maxDepth = MAX_DEPTH;
static int printStatsEveryNthTrace = 30000;
static int sampleIntervalInUs = 1;
static int threadsPerInterval = 10;
static bool checkThreadRunning = false;

void printHelp() {
  printf(R"(Usage: -agentpath:libagent.so=[,options]

Options:

  help
    print this help

  maxDepth=<int> (default: 1024)
    maximum depth of the stack traces to be collected
    has to be smaller than 1024

  printStatsEveryNthTrace=<int> (default: 10000)
    print statistics every n-th stack trace

  sampleIntervalInUs=<int> (default: 100)
    sample interval in microseconds

  threadsPerInterval=<int> (default: 10)
    number of threads to sample per interval

  checkThreadRunning=<bool> (default: false)
    check if the thread is currently running before sampling it, reduces performance
    but is probably broken
  )");
}

void parseOptions(char *options) {
  if (options == nullptr) {
    return;
  }

  char *token = strtok(options, ",");
  std::string tokenStr = token;
  while (token != nullptr) {
    if (tokenStr == "help") {
      printHelp();
      continue;
    }
    auto equalsPos = tokenStr.find("=");
    if (equalsPos == std::string::npos) {
      printf("Invalid option: %s\n", tokenStr.c_str());
      printHelp();
      exit(1);
    }
    auto key = tokenStr.substr(0, equalsPos);
    auto value = tokenStr.substr(equalsPos + 1);
    if (key == "maxDepth") {
      maxDepth = std::stoi(value);
    } else if (key == "printStatsEveryNthTrace") {
      printStatsEveryNthTrace = std::stoi(value);
    } else if (key == "sampleIntervalInUs") {
      sampleIntervalInUs = std::stoi(value);
    } else if (key == "threadsPerInterval") {
      threadsPerInterval = std::stoi(value);
    } else if (key == "checkThreadRunning") {
      checkThreadRunning = value == "true";
    } else {
      printf("Invalid option: %s\n", tokenStr.c_str());
      printHelp();
      exit(1);
    }
    token = strtok(nullptr, ",");
  }
}

static void JNICALL OnVMDeath(jvmtiEnv *jvmti_env, JNIEnv *jni_env) {
  onAbort();
}

extern "C" {

static jint Agent_Initialize(JavaVM *_jvm, char *options, void *reserved) {
  parseOptions(options);
  jvm = _jvm;
  jint res = jvm->GetEnv((void **)&jvmti, JVMTI_VERSION);
  if (res != JNI_OK || jvmti == nullptr) {
    fprintf(stderr, "Error: wrong result of a valid call to GetEnv!\n");
    return JNI_ERR;
  }

  jvmtiError err;
  jvmtiCapabilities caps;
  memset(&caps, 0, sizeof(caps));
  caps.can_get_line_numbers = 1;
  caps.can_get_source_file_name = 1;

  ensureSuccess(jvmti->AddCapabilities(&caps), "AddCapabilities");

  jvmtiEventCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.ClassLoad = &OnClassLoad;
  callbacks.VMInit = &OnVMInit;
  callbacks.ClassPrepare = &OnClassPrepare;
  callbacks.VMDeath = &OnVMDeath;
  callbacks.ThreadStart = &OnThreadStart;
  callbacks.ThreadEnd = &OnThreadEnd;
  ensureSuccess(
      jvmti->SetEventCallbacks(&callbacks, sizeof(jvmtiEventCallbacks)),
      "SetEventCallbacks");
  ensureSuccess(jvmti->SetEventNotificationMode(
                    JVMTI_ENABLE, JVMTI_EVENT_CLASS_LOAD, nullptr),
                "class load");
  ensureSuccess(jvmti->SetEventNotificationMode(
                    JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, nullptr),
                "class prepare");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE,
                                                JVMTI_EVENT_VM_INIT, nullptr),
                "vm init");
  ensureSuccess(jvmti->SetEventNotificationMode(JVMTI_ENABLE,
                                                JVMTI_EVENT_VM_DEATH, nullptr),
                "vm death");
  ensureSuccess(jvmti->SetEventNotificationMode(
                    JVMTI_ENABLE, JVMTI_EVENT_THREAD_START, nullptr),
                "thread start");
  ensureSuccess(jvmti->SetEventNotificationMode(
                    JVMTI_ENABLE, JVMTI_EVENT_THREAD_END, nullptr),
                "thread end");

  asgct = reinterpret_cast<ASGCTType>(dlsym(RTLD_DEFAULT, "AsyncGetCallTrace"));
  if (asgct == nullptr) {
    fprintf(stderr, "AsyncGetCallTrace not found.\n");
    return JNI_ERR;
  }

  asgct = reinterpret_cast<ASGCTType>(dlsym(RTLD_DEFAULT, "AsyncGetCallTrace"));
  if (asgct == nullptr) {
    fprintf(stderr, "AsyncGetCallTrace not found.\n");
    return JNI_ERR;
  }
  return JNI_OK;
}

JNIEXPORT
jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}

JNIEXPORT
jint JNICALL Agent_OnAttach(JavaVM *jvm, char *options, void *reserved) {
  return Agent_Initialize(jvm, options, reserved);
}
}

JNIEXPORT
void JNICALL Agent_OnUnload(JavaVM *jvm) { onAbort(); }

/** returns true if successful */
bool sendSignal(pthread_t thread) {
#if defined(__APPLE__) && defined(__MACH__)
  return pthread_kill(thread, SIGPROF) == 0;
#else
  union sigval sigval;
  return sigqueue(thread, SIGPROF, sigval) == 0;
#endif
}

template <typename _T> struct _PrintColumn {
  _T _content;
  size_t _width;
};

const size_t COLUMN_WIDTH = 8;

template <typename _T> inline _PrintColumn<_T> printColumn(_T content, size_t width = COLUMN_WIDTH) {
  return {content, width};
}

template <typename _CharT, typename _Traits>
inline std::basic_ostream<_CharT, _Traits> &
operator<<(std::basic_ostream<_CharT, _Traits> &__os, _PrintColumn<long> __f) {
  return __os << std::setw(__f._width) << std::right << __f._content;
}

template <typename _T, typename _CharT, typename _Traits>
inline std::basic_ostream<_CharT, _Traits> &
operator<<(std::basic_ostream<_CharT, _Traits> &__os, _PrintColumn<_T> __f) {
  return __os << std::setw(__f._width) << std::right << std::setprecision(1)
              << std::fixed << __f._content;
}

/** collects values and has stats */
class Statistic {
  std::vector<long> values;
  long _min = -1;
  long _max = -1;
  long _sum = -1;

public:
  Statistic() {}
  void push_back(long value) {
    values.push_back(value);
    if (_min == -1 || value < _min) {
      _min = value;
    }
    if (_max == -1 || value > _max) {
      _max = value;
    }
    _sum += value;
  }

  double mean() const { return _sum / values.size(); }

  double median() const {
    std::vector<long> valuesCopy = values;
    std::sort(valuesCopy.begin(), valuesCopy.end());
    if (valuesCopy.size() % 2 == 0) {
      return (valuesCopy[valuesCopy.size() / 2 - 1] +
              valuesCopy[valuesCopy.size() / 2]) /
             2;
    } else {
      return valuesCopy[valuesCopy.size() / 2];
    }
  }

  long min() const { return _min; }

  long max() const { return _max; }

  long count() const { return values.size(); }

  double stddev() const {
    return std::sqrt(std::accumulate(values.begin(), values.end(), 0.0,
                                     [this](double a, double b) {
                                       return a + (b - mean()) * (b - mean());
                                     }) /
                     values.size());
  }

  std::string header() const {
    std::stringstream ss;
    ss << printColumn("count", 12) << printColumn("min") << printColumn("median")
       << printColumn("mean") << printColumn("max") << printColumn("std")
       << printColumn("std/mean");
    return ss.str();
  }

  std::string str(bool with_header = true) const {
    if (count() == 0) {
      return "";
    }
    std::stringstream ss;
    if (with_header) {
      ss << header() << std::endl;
    }
    ss << printColumn(count(), 12) << printColumn(min()) << printColumn(median())
       << printColumn(mean()) << printColumn(max()) << printColumn(stddev())
       << printColumn(stddev() / mean());
    return ss.str();
  }
};

/** collects statistics in buckets */
template <size_t max_buckets = MAX_DEPTH> class LengthBucketStatistic {
  long bucketSize;
  std::array<Statistic, max_buckets> buckets;
  long maxBucket = 0;
  Statistic overall;

public:
  LengthBucketStatistic(long bucketSize) : bucketSize(bucketSize) {}

  void push_back(long length, long value) {
    size_t bucket = length / bucketSize;
    buckets.at(bucket).push_back(value);
    if (bucket > maxBucket) {
      maxBucket = bucket;
    }
    overall.push_back(value);
  }

  std::string header() const {
    std::stringstream ss;
    ss << std::right << std::setw(7) << "bucket" << printColumn("%")
       << overall.header();
    return ss.str();
  }

  std::string str(bool with_header = true) const {
    std::stringstream ss;
    if (with_header) {
      ss << header() << std::endl;
    }
    for (size_t i = 0; i <= maxBucket; i++) {
      ss << std::right << std::setw(7) << i * bucketSize
         << printColumn(buckets.at(i).count() * 100.0 / overall.count())
         << buckets.at(i).str(false) << std::endl;
    }
    ss << std::left << std::setw(7) << "overall" << std::setw(COLUMN_WIDTH) << std::right
       << std::setprecision(3) << 100 << overall.str(false) << std::endl;
    return ss.str();
  }

  size_t count() const { return overall.count(); }
};

LengthBucketStatistic asgctTimings(10);
Statistic jniEnvTimings;
LengthBucketStatistic asgctTimingsWithSignalHandling(10);
Statistic asgctBrokenTimings;

ASGCT_CallTrace trace;
ASGCT_CallFrame frames[MAX_DEPTH];
std::atomic<long> timing;
std::atomic<long> jniEnvTiming;
std::atomic<long> traceLength;

void printInfo() {
  std::cerr << "asgct alone" << std::endl
            << asgctTimings.str() << std::endl
            << "signal handler till end" << std::endl
            << asgctTimingsWithSignalHandling.str() << std::endl
            << "env" << std::endl
            << std::setw(12 + 7) << " " << jniEnvTimings.str(false) << std::endl
            << "asgct broken" << std::endl
            << std::setw(12 + 7) << " " << asgctBrokenTimings.str(false)
            << std::endl;
}

std::atomic<long> lastInfoPrinted(0);

void printInfoIfNeeded() {
  if (lastInfoPrinted.load() + (printStatsEveryNthTrace / 2) <
      asgctTimings.count()) {
    printInfo();
    lastInfoPrinted = asgctTimings.count();
  }
}

bool checkJThread(jthread javaThread) {
  jint state;
  jvmti->GetThreadState(javaThread, &state);

  if (!((state & JVMTI_THREAD_STATE_ALIVE) == 1 &&
        (state | JVMTI_THREAD_STATE_RUNNABLE) == state) &&
      (state & JVMTI_THREAD_STATE_IN_NATIVE) == 0) {
    return false;
  }
  return true;
}

/** busy wait till the atomic variable is as expected or the timeout (ms) is reached,
 * returns the value of the atomic variable */
bool waitOnAtomicTillUnequal(std::atomic<long> &atomic, long expected = -1,
                             int timeout = 1) {
  auto start = std::chrono::system_clock::now();
  while (atomic.load() == expected && std::chrono::system_clock::now() - start <
                                          std::chrono::milliseconds(timeout)) {
  }
  return atomic != expected;
}

/** returns true if the obtaining of stack traces was successful */
bool sample(pthread_t thread) {
  // send the signal
  auto start = std::chrono::system_clock::now();
  timing = -1;
  if (!sendSignal(thread)) {
    fprintf(stderr, "could not send signal to thread %ld\n", thread);
    return false;
  }
  if (!waitOnAtomicTillUnequal(timing, -1)) {
    return false;
  }
  if (traceLength <= 0) {
    asgctBrokenTimings.push_back(timing.load());
    return false;
  }
  auto end = std::chrono::system_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count();
  asgctTimingsWithSignalHandling.push_back(traceLength.load(), duration);
  asgctTimings.push_back(traceLength.load(), timing.load());
  jniEnvTimings.push_back(jniEnvTiming.load());

  if (printStatsEveryNthTrace > 0 &&
      asgctTimings.count() % printStatsEveryNthTrace == 0) {
    printInfoIfNeeded();
  }

  return true;
}

void asgctGSTHandler(ucontext_t *ucontext) {
  auto start = std::chrono::system_clock::now();
  JNIEnv *jni;
  jvm->GetEnv((void **)&jni, JNI_VERSION_1_6);
  if (jni == nullptr) {
    trace.num_frames = 0;
    return;
  }
  jniEnvTiming = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now() - start)
                     .count();
  start = std::chrono::system_clock::now();
  trace.env_id = jni;
  trace.frames = frames;
  asgct(&trace, maxDepth, ucontext);
  traceLength = trace.num_frames;
  timing = std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now() - start)
               .count();
}

void sample(std::mt19937 &g) {
  std::vector<pthread_t> avThreads;
  {
    std::lock_guard<std::recursive_mutex> lock(threadToJavaIdMutex);
    for (auto &pair : threadToJavaId) {
      avThreads.push_back(pair.first);
    }
  }
  if (avThreads.empty()) {
    return;
  }
  std::shuffle(avThreads.begin(), avThreads.end(), g);
  if (checkThreadRunning) {
    int count = 0;
    for (auto thread : avThreads) {
      auto javaThread = getJThreadForPThread(env, thread);
      if (!javaThread || !checkJThread(javaThread) ||
          !sample(thread)) {
        continue;
      }
      if (++count >= threadsPerInterval) {
        break;
      }
    } 
  } else {
    int count = 0;
    for (auto thread : avThreads) {
      if (!sample(thread)) {
        continue;
      }
      if (++count >= threadsPerInterval) {
        break;
      }
    }
  }
}

void signalHandler(int signum, siginfo_t *info, void *ucontext) {
  asgctGSTHandler((ucontext_t *)ucontext);
}

void sampleLoop() {
  std::random_device rd;
  std::mt19937 g(rd());
  JNIEnv *newEnv;
  jvm->AttachCurrentThreadAsDaemon(
      (void **)&newEnv,
      nullptr); // important, so that the thread doesn't keep the JVM alive

  setpriority(PRIO_PROCESS, 0,
              0); // try to make the priority of this thread higher

  std::chrono::microseconds interval{sampleIntervalInUs};
  while (!shouldStop) {
    if (env == nullptr) {
      env = newEnv;
    }
    auto start = std::chrono::system_clock::now();
    sample(g);
    auto duration = std::chrono::system_clock::now() - start;
    auto sleep = interval - duration;
    if (std::chrono::seconds::zero() < sleep) {
      std::this_thread::sleep_for(sleep);
    }
  }
}
