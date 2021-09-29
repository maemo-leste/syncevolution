/*
 *  File:         platform_thread.cpp
 *
 *  Author:       Lukas Zeller (luz@plan44.ch)
 *
 *  Platform specific thread object implementation
 *
 *  Copyright (c) 2004-2011 by Synthesis AG + plan44.ch
 *
 *  2004-05-24 : luz : created from Win version
 *
 */

#include "prefix_file.h"

#include "platform_thread.h"
#include "platform_time.h"
#include "lineartime.h"
#include "timezones.h"

// private includes needed
#include <errno.h>
#include <signal.h>

#ifdef ANDROID
#include <unistd.h>
#endif

namespace sysync {

//  get id of the running process
uIntArch myProcessID() {
  return (uIntArch)getpid();
} // myProcessID


// get id of the running thread
uIntArch myThreadID() {
  return (uIntArch)pthread_self();
} // myThreadID


// The POSIX thread function, must be passed the Thread Object address as parameter
extern "C" void * PosixThreadFunc(void *aParam);
void* PosixThreadFunc(void *aParam)
{
  // get Thread Object pointer
  TThreadObject *threadObjP = static_cast<TThreadObject *>(aParam);
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: starting", threadObjP));
  // call thread execution method
  uInt32 retval = threadObjP->execute();
  // signal thread termination condition
  pthread_mutex_lock  (&(threadObjP->fDoneCondMutex));
  pthread_cond_signal (&(threadObjP->fDoneCond));
  threadObjP->fTerminated= true; // no longer valid
  pthread_mutex_unlock(&(threadObjP->fDoneCondMutex));
  // auto-dispose the thread object if requested
  if (threadObjP->fAutoDisposeThreadObj) {
    PNCDEBUGPRINTFX(DBG_HOT,("auto-disposing thread object %p", threadObjP));
    delete threadObjP;
  }
  // Exit thread now.
  // Avoid "cast to pointer from integer of different size" warning with
  // intermediate cast.
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: terminating", threadObjP));
  return (void *)(uIntArch)retval;
} // PosixThreadFunc


// creates thread object. Thread ist not started yet, must use launch() for this
TThreadObject::TThreadObject(void) :
  fStackSize(0),
  fThreadFunc(NULL),
  fThreadFuncParam(0),
  fTerminationRequested(false),
  fExitCode(0),
  fAutoDisposeThreadObj(false),
  fPosixThread(0),
  fTerminated(false)
{
  // init cond and mutex required to implement WaitFor
  pthread_mutex_init(&fDoneCondMutex,NULL);
  pthread_cond_init(&fDoneCond,NULL);
} // TThreadObject::TThreadObject


// destroys thread object. This will kill the thread if it still active.
TThreadObject::~TThreadObject()
{
  // kill thread (only if not auto-disposed at end of thread)
  if (!fAutoDisposeThreadObj) kill();
  // destroy cond/mutex
  pthread_mutex_destroy(&fDoneCondMutex);
  pthread_cond_destroy(&fDoneCond);
} // TThreadObject::TThreadObject


// launches thread. Returns false if thread could not be started. exitcode() will
// contain the platform error code for the failure to start the thread
bool TThreadObject::launch(
  TThreadFunc aThreadFunc, // the function to execute in the thread
  uIntArch aThreadFuncParam, // a parameter to pass to the thread
  size_t aStackSize,       // if 0, default stack size is used
  bool aAutoDispose        // if true, the thread object will dispose itself when thread has finished running
)
{
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: launching", this));
  // save parameters
  fThreadFunc=aThreadFunc;
  fThreadFuncParam=aThreadFuncParam;
  fStackSize=aStackSize;
  fAutoDisposeThreadObj=aAutoDispose;
  // assume ok exit errno.h
  fExitCode=0;
  fTerminationRequested=false;
  // thread must not exist yet
  if (fPosixThread!=0 && !fTerminated) {
    fExitCode=EEXIST; // we use this to signal thread is already started
    return false;
  }
  // now create a POSIX thread
  fPosixThread= 0;
  fTerminated= false;
  fExitCode = pthread_create(
    &fPosixThread, // pointer to returned thread identifier
    NULL, // default attributes = joinable, not realtime
    PosixThreadFunc, // pointer to thread function
    this // pass the pointer to the thread object
  );
  if (fExitCode==0) {
    PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: launched okay", this));
    // thread created successfully
    return true; // ok
  }
  else {
    // could not create thread
    fTerminated= true;
    return false; // not ok
  }
} // TThreadObject::launch


uIntArch TThreadObject::getid(void) {
  return (uIntArch)fPosixThread;
} // TThreadObject::getid



// kills the thread
void TThreadObject::kill(void)
{
  if (fPosixThread!=0 && !fTerminated) {
    PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: killing", this));
    // the thread is actually running
    fExitCode=EINTR; // nearest match, interrupted
    // kill it
    pthread_kill(fPosixThread, SIGKILL);
    fTerminated= true;
  } else {
    PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: skipped killing, not running", this));
  }
} // TThreadObject::kill

static int CondTimedWait(pthread_cond_t *cond, pthread_mutex_t *mutex, bool &aTerminated, long aMilliSecondsToWait)
{
  int retval;
  if (aMilliSecondsToWait<0) {
    // wait indefinitely
    retval= pthread_cond_wait(cond, mutex);
  }
  else {
    // wait specified amount of time
    struct timespec timeout;

    /* this conversion might be implemented in time module later */
    #define milli 1E-3
    #define msToLinearTimeFactor (secondToLinearTimeFactor*milli)     // is usually 1 again

    lineartime_t ltm = getSystemNowAs(TCTX_UTC,NULL) - UnixToLineartimeOffset; // starting 1970
    ltm = (lineartime_t)(ltm/msToLinearTimeFactor);               // as milliSeconds
    ltm+= aMilliSecondsToWait;                           // add the offset from now

    timeout.tv_sec = (unsigned int)(ltm * milli);
    timeout.tv_nsec= ( ltm % 1000 )*1000000; // ns

    retval= pthread_cond_timedwait(cond, mutex, &timeout);
  } /* if */

  return retval;
}

extern "C"
{
  /**
   * Wait until thread terminates or timeout occurs.
   *
   * Can use pthread_cond_[timed]wait with the given condition variable
   * and mutex to be worken up by the terminating thread. However, aTerminated
   * must be checked before each such pthread call, because the thread may
   * already have signaled termination.
   *
   * If the implementation does not use pthread_cond_[timed]wait, then it
   * must unlock the mutex. Otherwise the thread will never be able to lock
   * it and set aTerminated.
   *
   * @param aMilliSecondsToWait  < 0 for infinite wait, otherwise maximum time to wait since entering the function in ms
   */
  int (*SySync_CondTimedWait)(pthread_cond_t *cond, pthread_mutex_t *mutex, bool &aTerminated, long aMilliSecondsToWait) = CondTimedWait;
}

// waits for the thread to stop
bool TThreadObject::waitfor(sInt32 aMilliSecondsToWait)
{
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: waitfor %d", this, aMilliSecondsToWait));

  int retval= 0;
  if (fPosixThread==0) {
    PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: not running", this));
    return true; // thread not running
  }

  // thread is running
  // wait for termination condition of the thread
  pthread_mutex_lock(&fDoneCondMutex);
  if (!fTerminated) { // catch also, if signalled already
    retval= SySync_CondTimedWait(&fDoneCond, &fDoneCondMutex, fTerminated, aMilliSecondsToWait);
  } /* if */
  pthread_mutex_unlock(&fDoneCondMutex);
  if (retval!=0) {
    PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: still running", this));
    return false; // check if thread has completed => no, not yet
  }

  // thread has terminated (or is terminating) -> join and get exit code
  void* threadret;
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: join thread", this));
  int ret= pthread_join(fPosixThread,&threadret);
  if (ret==0) fExitCode= (uInt32)(uIntPtr)threadret;
  else        fExitCode= ret;

  fTerminated= true; // thread has ended
  fPosixThread= 0; // don't join thread again (doesn't work reliably) when waitfor() is called again
  PNCDEBUGPRINTFX(DBG_HOT,("thread object %p: joined thread", this));
  return true;
} // TThreadObject::waitfor



// Thread execution method
uInt32 TThreadObject::execute()
{
  // standard implementation simply starts the predefined thread function, if any
  if (fThreadFunc) {
    uInt32 err= fThreadFunc(this,fThreadFuncParam);
    return err;
  }
  else {
    return 0;
  }
} // TThreadObject::execute


} // namespace sysync

/* eof */
