/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsTimerImpl.h"
#include "TimerThread.h"

#include "nsThreadUtils.h"
#include "pratom.h"

#include "nsIObserverService.h"
#include "nsIServiceManager.h"
#include "mozilla/Services.h"
#include "mozilla/ChaosMode.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/BinarySearch.h"

#include <math.h>

using namespace mozilla;

NS_IMPL_ISUPPORTS(TimerThread, nsIRunnable, nsIObserver)

TimerThread::TimerThread() :
  mInitInProgress(false),
  mInitialized(false),
  mMonitor("TimerThread.mMonitor"),
  mShutdown(false),
  mWaiting(false),
  mNotified(false),
  mSleeping(false),
  mLastTimerEventLoopRun(TimeStamp::Now())
{
}

TimerThread::~TimerThread()
{
  mThread = nullptr;

  NS_ASSERTION(mTimers.IsEmpty(), "Timers remain in TimerThread::~TimerThread");
}

nsresult
TimerThread::InitLocks()
{
  return NS_OK;
}

namespace {

class TimerObserverRunnable : public nsRunnable
{
public:
  explicit TimerObserverRunnable(nsIObserver* aObserver)
    : mObserver(aObserver)
  {
  }

  NS_DECL_NSIRUNNABLE

private:
  nsCOMPtr<nsIObserver> mObserver;
};

NS_IMETHODIMP
TimerObserverRunnable::Run()
{
  nsCOMPtr<nsIObserverService> observerService =
    mozilla::services::GetObserverService();
  if (observerService) {
    observerService->AddObserver(mObserver, "sleep_notification", false);
    observerService->AddObserver(mObserver, "wake_notification", false);
    observerService->AddObserver(mObserver, "suspend_process_notification", false);
    observerService->AddObserver(mObserver, "resume_process_notification", false);
  }
  return NS_OK;
}

} // anonymous namespace

nsresult
TimerThread::Init()
{
  PR_LOG(GetTimerLog(), PR_LOG_DEBUG,
         ("TimerThread::Init [%d]\n", mInitialized));

  if (mInitialized) {
    if (!mThread) {
      return NS_ERROR_FAILURE;
    }

    return NS_OK;
  }

  if (mInitInProgress.exchange(true) == false) {
    // We hold on to mThread to keep the thread alive.
    nsresult rv = NS_NewThread(getter_AddRefs(mThread), this);
    if (NS_FAILED(rv)) {
      mThread = nullptr;
    } else {
      nsRefPtr<TimerObserverRunnable> r = new TimerObserverRunnable(this);
      if (NS_IsMainThread()) {
        r->Run();
      } else {
        NS_DispatchToMainThread(r);
      }
    }

    {
      MonitorAutoLock lock(mMonitor);
      mInitialized = true;
      mMonitor.NotifyAll();
    }
  } else {
    MonitorAutoLock lock(mMonitor);
    while (!mInitialized) {
      mMonitor.Wait();
    }
  }

  if (!mThread) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
TimerThread::Shutdown()
{
  PR_LOG(GetTimerLog(), PR_LOG_DEBUG, ("TimerThread::Shutdown begin\n"));

  if (!mThread) {
    return NS_ERROR_NOT_INITIALIZED;
  }

  nsTArray<nsTimerImpl*> timers;
  {
    // lock scope
    MonitorAutoLock lock(mMonitor);

    mShutdown = true;

    // notify the cond var so that Run() can return
    if (mWaiting) {
      mNotified = true;
      mMonitor.Notify();
    }

    // Need to copy content of mTimers array to a local array
    // because call to timers' ReleaseCallback() (and release its self)
    // must not be done under the lock. Destructor of a callback
    // might potentially call some code reentering the same lock
    // that leads to unexpected behavior or deadlock.
    // See bug 422472.
    timers.AppendElements(mTimers);
    mTimers.Clear();
  }

  uint32_t timersCount = timers.Length();
  for (uint32_t i = 0; i < timersCount; i++) {
    nsTimerImpl* timer = timers[i];
    timer->ReleaseCallback();
    ReleaseTimerInternal(timer);
  }

  mThread->Shutdown();    // wait for the thread to die

  PR_LOG(GetTimerLog(), PR_LOG_DEBUG, ("TimerThread::Shutdown end\n"));
  return NS_OK;
}

#ifdef MOZ_NUWA_PROCESS
#include "ipc/Nuwa.h"
#endif

namespace {

struct MicrosecondsToInterval
{
  PRIntervalTime operator[](size_t aMs) const {
    return PR_MicrosecondsToInterval(aMs);
  }
};

struct IntervalComparator
{
  int operator()(PRIntervalTime aInterval) const {
    return (0 < aInterval) ? -1 : 1;
  }
};

} // namespace

/* void Run(); */
NS_IMETHODIMP
TimerThread::Run()
{
  PR_SetCurrentThreadName("Timer");

#ifdef MOZ_NUWA_PROCESS
  if (IsNuwaProcess()) {
    NuwaMarkCurrentThread(nullptr, nullptr);
  }
#endif

  NS_SetIgnoreStatusOfCurrentThread();
  MonitorAutoLock lock(mMonitor);

  // We need to know how many microseconds give a positive PRIntervalTime. This
  // is platform-dependent and we calculate it at runtime, finding a value |v|
  // such that |PR_MicrosecondsToInterval(v) > 0| and then binary-searching in
  // the range [0, v) to find the ms-to-interval scale.
  uint32_t usForPosInterval = 1;
  while (PR_MicrosecondsToInterval(usForPosInterval) == 0) {
    usForPosInterval <<= 1;
  }

  size_t usIntervalResolution;
  BinarySearchIf(MicrosecondsToInterval(), 0, usForPosInterval, IntervalComparator(), &usIntervalResolution);
  MOZ_ASSERT(PR_MicrosecondsToInterval(usIntervalResolution - 1) == 0);
  MOZ_ASSERT(PR_MicrosecondsToInterval(usIntervalResolution) == 1);

  // Half of the amount of microseconds needed to get positive PRIntervalTime.
  // We use this to decide how to round our wait times later
  int32_t halfMicrosecondsIntervalResolution = usIntervalResolution / 2;
  bool forceRunNextTimer = false;

  while (!mShutdown) {
    // Have to use PRIntervalTime here, since PR_WaitCondVar takes it
    PRIntervalTime waitFor;
    bool forceRunThisTimer = forceRunNextTimer;
    forceRunNextTimer = false;

    if (mSleeping) {
      // Sleep for 0.1 seconds while not firing timers.
      uint32_t milliseconds = 100;
      if (ChaosMode::isActive(ChaosMode::TimerScheduling)) {
        milliseconds = ChaosMode::randomUint32LessThan(200);
      }
      waitFor = PR_MillisecondsToInterval(milliseconds);
    } else {
      waitFor = PR_INTERVAL_NO_TIMEOUT;
      TimeStamp now = TimeStamp::Now();
      mLastTimerEventLoopRun = now;
      nsTimerImpl* timer = nullptr;

      if (!mTimers.IsEmpty()) {
        timer = mTimers[0];

        if (now >= timer->mTimeout || forceRunThisTimer) {
    next:
          // NB: AddRef before the Release under RemoveTimerInternal to avoid
          // mRefCnt passing through zero, in case all other refs than the one
          // from mTimers have gone away (the last non-mTimers[i]-ref's Release
          // must be racing with us, blocked in gThread->RemoveTimer waiting
          // for TimerThread::mMonitor, under nsTimerImpl::Release.

          nsRefPtr<nsTimerImpl> timerRef(timer);
          RemoveTimerInternal(timer);
          timer = nullptr;

          PR_LOG(GetTimerLog(), PR_LOG_DEBUG,
                 ("Timer thread woke up %fms from when it was supposed to\n",
                  fabs((now - timerRef->mTimeout).ToMilliseconds())));

          {
            // We release mMonitor around the Fire call to avoid deadlock.
            MonitorAutoUnlock unlock(mMonitor);

            // We are going to let the call to PostTimerEvent here handle the
            // release of the timer so that we don't end up releasing the timer
            // on the TimerThread instead of on the thread it targets.
            timerRef = nsTimerImpl::PostTimerEvent(timerRef.forget());
          }

          if (timerRef) {
            // We got our reference back due to an error.
            // Unhook the nsRefPtr, and release manually so we can get the
            // refcount.
            nsrefcnt rc = timerRef.forget().take()->Release();
            (void)rc;

            // The nsITimer interface requires that its users keep a reference
            // to the timers they use while those timers are initialized but
            // have not yet fired.  If this ever happens, it is a bug in the
            // code that created and used the timer.
            //
            // Further, note that this should never happen even with a
            // misbehaving user, because nsTimerImpl::Release checks for a
            // refcount of 1 with an armed timer (a timer whose only reference
            // is from the timer thread) and when it hits this will remove the
            // timer from the timer thread and thus destroy the last reference,
            // preventing this situation from occurring.
            MOZ_ASSERT(rc != 0, "destroyed timer off its target thread!");
          }

          if (mShutdown) {
            break;
          }

          // Update now, as PostTimerEvent plus the locking may have taken a
          // tick or two, and we may goto next below.
          now = TimeStamp::Now();
        }
      }

      if (!mTimers.IsEmpty()) {
        timer = mTimers[0];

        TimeStamp timeout = timer->mTimeout;

        // Don't wait at all (even for PR_INTERVAL_NO_WAIT) if the next timer
        // is due now or overdue.
        //
        // Note that we can only sleep for integer values of a certain
        // resolution. We use halfMicrosecondsIntervalResolution, calculated
        // before, to do the optimal rounding (i.e., of how to decide what
        // interval is so small we should not wait at all).
        double microseconds = (timeout - now).ToMilliseconds() * 1000;

        if (ChaosMode::isActive(ChaosMode::TimerScheduling)) {
          // The mean value of sFractions must be 1 to ensure that
          // the average of a long sequence of timeouts converges to the
          // actual sum of their times.
          static const float sFractions[] = {
            0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.75f, 2.75f
          };
          microseconds *=
            sFractions[ChaosMode::randomUint32LessThan(ArrayLength(sFractions))];
          forceRunNextTimer = true;
        }

        if (microseconds < halfMicrosecondsIntervalResolution) {
          forceRunNextTimer = false;
          goto next; // round down; execute event now
        }
        waitFor = PR_MicrosecondsToInterval(
          static_cast<uint32_t>(microseconds)); // Floor is accurate enough.
        if (waitFor == 0) {
          waitFor = 1;  // round up, wait the minimum time we can wait
        }
      }

      if (PR_LOG_TEST(GetTimerLog(), PR_LOG_DEBUG)) {
        if (waitFor == PR_INTERVAL_NO_TIMEOUT)
          PR_LOG(GetTimerLog(), PR_LOG_DEBUG,
                 ("waiting for PR_INTERVAL_NO_TIMEOUT\n"));
        else
          PR_LOG(GetTimerLog(), PR_LOG_DEBUG,
                 ("waiting for %u\n", PR_IntervalToMilliseconds(waitFor)));
      }
    }

    mWaiting = true;
    mNotified = false;
    mMonitor.Wait(waitFor);
    if (mNotified) {
      forceRunNextTimer = false;
    }
    mWaiting = false;
  }

  return NS_OK;
}

nsresult
TimerThread::AddTimer(nsTimerImpl* aTimer)
{
  MonitorAutoLock lock(mMonitor);

  // Add the timer to our list.
  int32_t i = AddTimerInternal(aTimer);
  if (i < 0) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // Awaken the timer thread.
  if (mWaiting && i == 0) {
    mNotified = true;
    mMonitor.Notify();
  }

  return NS_OK;
}

nsresult
TimerThread::TimerDelayChanged(nsTimerImpl* aTimer)
{
  MonitorAutoLock lock(mMonitor);

  // Our caller has a strong ref to aTimer, so it can't go away here under
  // ReleaseTimerInternal.
  RemoveTimerInternal(aTimer);

  int32_t i = AddTimerInternal(aTimer);
  if (i < 0) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // Awaken the timer thread.
  if (mWaiting && i == 0) {
    mNotified = true;
    mMonitor.Notify();
  }

  return NS_OK;
}

nsresult
TimerThread::RemoveTimer(nsTimerImpl* aTimer)
{
  MonitorAutoLock lock(mMonitor);

  // Remove the timer from our array.  Tell callers that aTimer was not found
  // by returning NS_ERROR_NOT_AVAILABLE.  Unlike the TimerDelayChanged case
  // immediately above, our caller may be passing a (now-)weak ref in via the
  // aTimer param, specifically when nsTimerImpl::Release loses a race with
  // TimerThread::Run, must wait for the mMonitor auto-lock here, and during the
  // wait Run drops the only remaining ref to aTimer via RemoveTimerInternal.

  if (!RemoveTimerInternal(aTimer)) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // Awaken the timer thread.
  if (mWaiting) {
    mNotified = true;
    mMonitor.Notify();
  }

  return NS_OK;
}

// This function must be called from within a lock
int32_t
TimerThread::AddTimerInternal(nsTimerImpl* aTimer)
{
  mMonitor.AssertCurrentThreadOwns();
  if (mShutdown) {
    return -1;
  }

  TimeStamp now = TimeStamp::Now();

  TimerAdditionComparator c(now, aTimer);
  nsTimerImpl** insertSlot = mTimers.InsertElementSorted(aTimer, c);

  if (!insertSlot) {
    return -1;
  }

  aTimer->mArmed = true;
  NS_ADDREF(aTimer);

#ifdef MOZ_TASK_TRACER
  // Create a FakeTracedTask, and dispatch it here. This is the start point of
  // the latency.
  aTimer->DispatchTracedTask();
#endif

  return insertSlot - mTimers.Elements();
}

bool
TimerThread::RemoveTimerInternal(nsTimerImpl* aTimer)
{
  mMonitor.AssertCurrentThreadOwns();
  if (!mTimers.RemoveElement(aTimer)) {
    return false;
  }

  ReleaseTimerInternal(aTimer);
  return true;
}

void
TimerThread::ReleaseTimerInternal(nsTimerImpl* aTimer)
{
  if (!mShutdown) {
    // copied to a local array before releasing in shutdown
    mMonitor.AssertCurrentThreadOwns();
  }
  // Order is crucial here -- see nsTimerImpl::Release.
  aTimer->mArmed = false;
  NS_RELEASE(aTimer);
}

void
TimerThread::DoBeforeSleep()
{
  // Mainthread
  MonitorAutoLock lock(mMonitor);
  mLastTimerEventLoopRun = TimeStamp::Now();
  mSleeping = true;
}

// Note: wake may be notified without preceding sleep notification
void
TimerThread::DoAfterSleep()
{
  // Mainthread
  TimeStamp now = TimeStamp::Now();

  MonitorAutoLock lock(mMonitor);

  // an over-estimate of time slept, usually small
  TimeDuration slept = now - mLastTimerEventLoopRun;

  // Adjust all old timers to expire roughly similar times in the future
  // compared to when we went to sleep, by adding the time we slept to the
  // target time. It's slightly possible a few will end up slightly in the
  // past and fire immediately, but ordering should be preserved.  All
  // timers retain the exact same order (and relative times) as before
  // going to sleep.
  for (uint32_t i = 0; i < mTimers.Length(); i ++) {
    nsTimerImpl* timer = mTimers[i];
    timer->mTimeout += slept;
  }
  mSleeping = false;
  mLastTimerEventLoopRun = now;

  // Wake up the timer thread to process the updated array
  mNotified = true;
  mMonitor.Notify();
}


/* void observe (in nsISupports aSubject, in string aTopic, in wstring aData); */
NS_IMETHODIMP
TimerThread::Observe(nsISupports* /* aSubject */, const char* aTopic,
                     const char16_t* /* aData */)
{
  if (strcmp(aTopic, "sleep_notification") == 0 ||
      strcmp(aTopic, "suspend_process_notification") == 0) {
    DoBeforeSleep();
  } else if (strcmp(aTopic, "wake_notification") == 0 ||
             strcmp(aTopic, "resume_process_notification") == 0) {
    DoAfterSleep();
  }

  return NS_OK;
}
