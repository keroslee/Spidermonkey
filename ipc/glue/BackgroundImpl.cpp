/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/process_util.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/unused.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/ipc/ProtocolTypes.h"
#include "BackgroundChild.h"
#include "BackgroundChildImpl.h"
#include "BackgroundParent.h"
#include "BackgroundParentImpl.h"
#include "GeckoProfiler.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsIEventTarget.h"
#include "nsIIPCBackgroundChildCreateCallback.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIRunnable.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsITimer.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "nsXULAppAPI.h"
#include "nsXPCOMPrivate.h"
#include "prthread.h"

#ifdef RELEASE_BUILD
#define THREADSAFETY_ASSERT MOZ_ASSERT
#else
#define THREADSAFETY_ASSERT MOZ_RELEASE_ASSERT
#endif

#define CRASH_IN_CHILD_PROCESS(_msg)                                           \
  do {                                                                         \
    if (IsMainProcess()) {                                                     \
      MOZ_ASSERT(false, _msg);                                                 \
    } else {                                                                   \
      MOZ_CRASH(_msg);                                                         \
    }                                                                          \
  }                                                                            \
  while (0)

using namespace mozilla;
using namespace mozilla::ipc;

using mozilla::dom::ContentChild;
using mozilla::dom::ContentParent;

namespace {

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------

bool
IsMainProcess()
{
  static const bool isMainProcess =
    XRE_GetProcessType() == GeckoProcessType_Default;
  return isMainProcess;
}

#ifdef DEBUG
bool
IsChildProcess()
{
  return !IsMainProcess();
}
#endif

void
AssertIsInMainProcess()
{
  MOZ_ASSERT(IsMainProcess());
}

void
AssertIsInChildProcess()
{
  MOZ_ASSERT(IsChildProcess());
}

void
AssertIsOnMainThread()
{
  THREADSAFETY_ASSERT(NS_IsMainThread());
}

// -----------------------------------------------------------------------------
// ParentImpl Declaration
// -----------------------------------------------------------------------------

class ParentImpl MOZ_FINAL : public BackgroundParentImpl
{
  friend class mozilla::ipc::BackgroundParent;

public:
  class CreateCallback;

private:
  class ShutdownObserver;
  class RequestMessageLoopRunnable;
  class ShutdownBackgroundThreadRunnable;
  class ForceCloseBackgroundActorsRunnable;
  class CreateCallbackRunnable;
  class ConnectActorRunnable;

  struct MOZ_STACK_CLASS TimerCallbackClosure
  {
    nsIThread* mThread;
    nsTArray<ParentImpl*>* mLiveActors;

    TimerCallbackClosure(nsIThread* aThread, nsTArray<ParentImpl*>* aLiveActors)
      : mThread(aThread), mLiveActors(aLiveActors)
    {
      AssertIsInMainProcess();
      AssertIsOnMainThread();
      MOZ_ASSERT(aThread);
      MOZ_ASSERT(aLiveActors);
    }
  };

  // A handle that is invalid on any platform.
  static const ProcessHandle kInvalidProcessHandle;

  // The length of time we will wait at shutdown for all actors to clean
  // themselves up before forcing them to be destroyed.
  static const uint32_t kShutdownTimerDelayMS = 10000;

  // This is only modified on the main thread. It is null if the thread does not
  // exist or is shutting down.
  static StaticRefPtr<nsIThread> sBackgroundThread;

  // This is created and destroyed on the main thread but only modified on the
  // background thread. It is specific to each instance of sBackgroundThread.
  static nsTArray<ParentImpl*>* sLiveActorsForBackgroundThread;

  // This is only modified on the main thread.
  static StaticRefPtr<nsITimer> sShutdownTimer;

  // This exists so that that [Assert]IsOnBackgroundThread() can continue to
  // work during shutdown.
  static Atomic<PRThread*> sBackgroundPRThread;

  // This is only modified on the main thread. It is null if the thread does not
  // exist or is shutting down.
  static MessageLoop* sBackgroundThreadMessageLoop;

  // This is only modified on the main thread. It maintains a count of live
  // actors so that the background thread can be shut down when it is no longer
  // needed.
  static uint64_t sLiveActorCount;

  // This is only modified on the main thread. It is true after the shutdown
  // observer is registered and is never unset thereafter.
  static bool sShutdownObserverRegistered;

  // This is only modified on the main thread. It prevents us from trying to
  // create the background thread after application shutdown has started.
  static bool sShutdownHasStarted;

  // This is only modified on the main thread. It is a FIFO queue for callbacks
  // waiting for the background thread to be created.
  static StaticAutoPtr<nsTArray<nsRefPtr<CreateCallback>>> sPendingCallbacks;

  // Only touched on the main thread, null if this is a same-process actor.
  nsRefPtr<ContentParent> mContent;

  // mTransport is "owned" by this object but it must only be released on the
  // IPC thread. It's left as a raw pointer here to prevent accidentally
  // deleting it on the wrong thread. Only non-null for other-process actors.
  Transport* mTransport;

  // Set when the actor is opened successfully and used to handle shutdown
  // hangs. Only touched on the background thread.
  nsTArray<ParentImpl*>* mLiveActorArray;

  // Set at construction to indicate whether this parent actor corresponds to a
  // child actor in another process or to a child actor from a different thread
  // in the same process.
  const bool mIsOtherProcessActor;

  // Set after ActorDestroy has been called. Only touched on the background
  // thread.
  bool mActorDestroyed;

public:
  static bool
  CreateActorForSameProcess(CreateCallback* aCallback);

  static bool
  IsOnBackgroundThread()
  {
    return PR_GetCurrentThread() == sBackgroundPRThread;
  }

  static void
  AssertIsOnBackgroundThread()
  {
    THREADSAFETY_ASSERT(IsOnBackgroundThread());
  }

  NS_INLINE_DECL_REFCOUNTING(ParentImpl)

  void
  Destroy();

private:
  // Forwarded from BackgroundParent.
  static bool
  IsOtherProcessActor(PBackgroundParent* aBackgroundActor);

  // Forwarded from BackgroundParent.
  static already_AddRefed<ContentParent>
  GetContentParent(PBackgroundParent* aBackgroundActor);

  // Forwarded from BackgroundParent.
  static PBackgroundParent*
  Alloc(ContentParent* aContent,
        Transport* aTransport,
        ProcessId aOtherProcess);

  static bool
  CreateBackgroundThread();

  static void
  ShutdownBackgroundThread();

  static void
  ShutdownTimerCallback(nsITimer* aTimer, void* aClosure);

  // For same-process actors.
  ParentImpl()
  : mTransport(nullptr), mLiveActorArray(nullptr), mIsOtherProcessActor(false),
    mActorDestroyed(false)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();

    SetOtherProcess(kInvalidProcessHandle);
  }

  // For other-process actors.
  ParentImpl(ContentParent* aContent, Transport* aTransport)
  : mContent(aContent), mTransport(aTransport), mLiveActorArray(nullptr),
    mIsOtherProcessActor(true), mActorDestroyed(false)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aContent);
    MOZ_ASSERT(aTransport);
  }

  ~ParentImpl()
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(!mContent);
    MOZ_ASSERT(!mTransport);
  }

  void
  MainThreadActorDestroy();

  void
  SetLiveActorArray(nsTArray<ParentImpl*>* aLiveActorArray)
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();
    MOZ_ASSERT(aLiveActorArray);
    MOZ_ASSERT(!aLiveActorArray->Contains(this));
    MOZ_ASSERT(!mLiveActorArray);
    MOZ_ASSERT(mIsOtherProcessActor);

    mLiveActorArray = aLiveActorArray;
    mLiveActorArray->AppendElement(this);
  }

  // These methods are only called by IPDL.
  virtual IToplevelProtocol*
  CloneToplevel(const InfallibleTArray<ProtocolFdMapping>& aFds,
                ProcessHandle aPeerProcess,
                ProtocolCloneContext* aCtx) MOZ_OVERRIDE;

  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;
};

// -----------------------------------------------------------------------------
// ChildImpl Declaration
// -----------------------------------------------------------------------------

class ChildImpl MOZ_FINAL : public BackgroundChildImpl
{
  friend class mozilla::ipc::BackgroundChild;
  friend class mozilla::ipc::BackgroundChildImpl;

  typedef base::ProcessId ProcessId;
  typedef mozilla::ipc::Transport Transport;

  class ShutdownObserver;
  class CreateActorRunnable;
  class ParentCreateCallback;
  class AlreadyCreatedCallbackRunnable;
  class FailedCreateCallbackRunnable;
  class OpenChildProcessActorRunnable;
  class OpenMainProcessActorRunnable;

  // A thread-local index that is not valid.
  static const unsigned int kBadThreadLocalIndex =
    static_cast<unsigned int>(-1);

  // This is only modified on the main thread. It is the thread-local index that
  // we use to store the BackgroundChild for each thread.
  static unsigned int sThreadLocalIndex;

  struct ThreadLocalInfo
  {
    explicit ThreadLocalInfo(nsIIPCBackgroundChildCreateCallback* aCallback)
    {
      mCallbacks.AppendElement(aCallback);
    }

    nsRefPtr<ChildImpl> mActor;
    nsTArray<nsCOMPtr<nsIIPCBackgroundChildCreateCallback>> mCallbacks;
    nsAutoPtr<BackgroundChildImpl::ThreadLocal> mConsumerThreadLocal;
  };

  // This is only modified on the main thread. It is a FIFO queue for actors
  // that are in the process of construction.
  static StaticAutoPtr<nsTArray<nsCOMPtr<nsIEventTarget>>> sPendingTargets;

  // This is only modified on the main thread. It prevents us from trying to
  // create the background thread after application shutdown has started.
  static bool sShutdownHasStarted;

#ifdef RELEASE_BUILD
  DebugOnly<nsIThread*> mBoundThread;
#else
  nsIThread* mBoundThread;
#endif

  DebugOnly<bool> mActorDestroyed;

public:
  static bool
  OpenProtocolOnMainThread(nsIEventTarget* aEventTarget);

  static void
  Shutdown();

  void
  AssertIsOnBoundThread()
  {
    THREADSAFETY_ASSERT(mBoundThread);

#ifdef RELEASE_BUILD
    DebugOnly<bool> current;
#else
    bool current;
#endif
    THREADSAFETY_ASSERT(
      NS_SUCCEEDED(mBoundThread->IsOnCurrentThread(&current)));
    THREADSAFETY_ASSERT(current);
  }

  void
  AssertActorDestroyed()
  {
    MOZ_ASSERT(mActorDestroyed, "ChildImpl::ActorDestroy not called in time");
  }

  ChildImpl()
  : mBoundThread(nullptr)
  , mActorDestroyed(false)
  {
    AssertIsOnMainThread();
  }

  NS_INLINE_DECL_REFCOUNTING(ChildImpl)

private:
  // Forwarded from BackgroundChild.
  static void
  Startup();

  // Forwarded from BackgroundChild.
  static PBackgroundChild*
  Alloc(Transport* aTransport, ProcessId aOtherProcess);

  // Forwarded from BackgroundChild.
  static PBackgroundChild*
  GetForCurrentThread();

  // Forwarded from BackgroundChild.
  static bool
  GetOrCreateForCurrentThread(nsIIPCBackgroundChildCreateCallback* aCallback);

  // Forwarded from BackgroundChild.
  static void
  CloseForCurrentThread();

  // Forwarded from BackgroundChildImpl.
  static BackgroundChildImpl::ThreadLocal*
  GetThreadLocalForCurrentThread();

  static void
  ThreadLocalDestructor(void* aThreadLocal)
  {
    auto threadLocalInfo = static_cast<ThreadLocalInfo*>(aThreadLocal);

    if (threadLocalInfo) {
      if (threadLocalInfo->mActor) {
        threadLocalInfo->mActor->Close();
        threadLocalInfo->mActor->AssertActorDestroyed();

        // Since the actor is created on the main thread it must only
        // be released on the main thread as well.
        if (!NS_IsMainThread()) {
          ChildImpl* actor;
          threadLocalInfo->mActor.forget(&actor);

          nsCOMPtr<nsIRunnable> releaser =
            NS_NewNonOwningRunnableMethod(actor, &ChildImpl::Release);
          MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(releaser)));
        }
      }
      delete threadLocalInfo;
    }
  }

  static void
  DispatchFailureCallback(nsIEventTarget* aEventTarget);

  // This class is reference counted.
  ~ChildImpl()
  {
    AssertActorDestroyed();
  }

  void
  SetBoundThread()
  {
    THREADSAFETY_ASSERT(!mBoundThread);

#if defined(DEBUG) || !defined(RELEASE_BUILD)
    mBoundThread = NS_GetCurrentThread();
#endif

    THREADSAFETY_ASSERT(mBoundThread);
  }

  // Only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  static already_AddRefed<nsIIPCBackgroundChildCreateCallback>
  GetNextCallback();
};

// -----------------------------------------------------------------------------
// ParentImpl Helper Declarations
// -----------------------------------------------------------------------------

class ParentImpl::ShutdownObserver MOZ_FINAL : public nsIObserver
{
public:
  ShutdownObserver()
  {
    AssertIsOnMainThread();
  }

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

private:
  ~ShutdownObserver()
  {
    AssertIsOnMainThread();
  }
};

class ParentImpl::RequestMessageLoopRunnable MOZ_FINAL :
  public nsRunnable
{
  nsCOMPtr<nsIThread> mTargetThread;
  MessageLoop* mMessageLoop;

public:
  explicit RequestMessageLoopRunnable(nsIThread* aTargetThread)
  : mTargetThread(aTargetThread), mMessageLoop(nullptr)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aTargetThread);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~RequestMessageLoopRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ParentImpl::ShutdownBackgroundThreadRunnable MOZ_FINAL : public nsRunnable
{
public:
  ShutdownBackgroundThreadRunnable()
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~ShutdownBackgroundThreadRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ParentImpl::ForceCloseBackgroundActorsRunnable MOZ_FINAL : public nsRunnable
{
  nsTArray<ParentImpl*>* mActorArray;

public:
  explicit ForceCloseBackgroundActorsRunnable(nsTArray<ParentImpl*>* aActorArray)
  : mActorArray(aActorArray)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aActorArray);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~ForceCloseBackgroundActorsRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ParentImpl::CreateCallbackRunnable MOZ_FINAL : public nsRunnable
{
  nsRefPtr<CreateCallback> mCallback;

public:
  explicit CreateCallbackRunnable(CreateCallback* aCallback)
  : mCallback(aCallback)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aCallback);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~CreateCallbackRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ParentImpl::ConnectActorRunnable MOZ_FINAL : public nsRunnable
{
  nsRefPtr<ParentImpl> mActor;
  Transport* mTransport;
  ProcessHandle mProcessHandle;
  nsTArray<ParentImpl*>* mLiveActorArray;

public:
  ConnectActorRunnable(ParentImpl* aActor,
                       Transport* aTransport,
                       ProcessHandle aProcessHandle,
                       nsTArray<ParentImpl*>* aLiveActorArray)
  : mActor(aActor), mTransport(aTransport), mProcessHandle(aProcessHandle),
    mLiveActorArray(aLiveActorArray)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aActor);
    MOZ_ASSERT(aTransport);
    MOZ_ASSERT(aLiveActorArray);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~ConnectActorRunnable()
  {
    AssertIsInMainProcess();
  }

  NS_DECL_NSIRUNNABLE
};

class NS_NO_VTABLE ParentImpl::CreateCallback
{
public:
  NS_INLINE_DECL_REFCOUNTING(CreateCallback)

  virtual void
  Success(already_AddRefed<ParentImpl> aActor, MessageLoop* aMessageLoop) = 0;

  virtual void
  Failure() = 0;

protected:
  virtual ~CreateCallback()
  { }
};

// -----------------------------------------------------------------------------
// ChildImpl Helper Declarations
// -----------------------------------------------------------------------------

class ChildImpl::ShutdownObserver MOZ_FINAL : public nsIObserver
{
public:
  ShutdownObserver()
  {
    AssertIsOnMainThread();
  }

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

private:
  ~ShutdownObserver()
  {
    AssertIsOnMainThread();
  }
};

class ChildImpl::CreateActorRunnable MOZ_FINAL : public nsRunnable
{
  nsCOMPtr<nsIEventTarget> mEventTarget;

public:
  CreateActorRunnable()
  : mEventTarget(NS_GetCurrentThread())
  {
    MOZ_ASSERT(mEventTarget);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~CreateActorRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ChildImpl::ParentCreateCallback MOZ_FINAL :
  public ParentImpl::CreateCallback
{
  nsCOMPtr<nsIEventTarget> mEventTarget;

public:
  explicit ParentCreateCallback(nsIEventTarget* aEventTarget)
  : mEventTarget(aEventTarget)
  {
    AssertIsInMainProcess();
    AssertIsOnMainThread();
    MOZ_ASSERT(aEventTarget);
  }

private:
  ~ParentCreateCallback()
  { }

  virtual void
  Success(already_AddRefed<ParentImpl> aActor, MessageLoop* aMessageLoop)
          MOZ_OVERRIDE;

  virtual void
  Failure() MOZ_OVERRIDE;
};

// Must be cancelable in order to dispatch on active worker threads
class ChildImpl::AlreadyCreatedCallbackRunnable MOZ_FINAL :
  public nsCancelableRunnable
{
public:
  AlreadyCreatedCallbackRunnable()
  {
    // May be created on any thread!
  }

  NS_DECL_ISUPPORTS_INHERITED

protected:
  virtual ~AlreadyCreatedCallbackRunnable()
  { }

  NS_DECL_NSIRUNNABLE
  NS_DECL_NSICANCELABLERUNNABLE
};

class ChildImpl::FailedCreateCallbackRunnable MOZ_FINAL : public nsRunnable
{
public:
  FailedCreateCallbackRunnable()
  {
    // May be created on any thread!
  }

  NS_DECL_ISUPPORTS_INHERITED

protected:
  virtual ~FailedCreateCallbackRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

class ChildImpl::OpenChildProcessActorRunnable MOZ_FINAL : public nsRunnable
{
  nsRefPtr<ChildImpl> mActor;
  nsAutoPtr<Transport> mTransport;
  ProcessHandle mProcessHandle;

public:
  OpenChildProcessActorRunnable(already_AddRefed<ChildImpl>&& aActor,
                                Transport* aTransport,
                                ProcessHandle aProcessHandle)
  : mActor(aActor), mTransport(aTransport),
    mProcessHandle(aProcessHandle)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(mActor);
    MOZ_ASSERT(aTransport);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~OpenChildProcessActorRunnable()
  {
    if (mTransport) {
      CRASH_IN_CHILD_PROCESS("Leaking transport!");
      unused << mTransport.forget();
    }
  }

  NS_DECL_NSIRUNNABLE
};

class ChildImpl::OpenMainProcessActorRunnable MOZ_FINAL : public nsRunnable
{
  nsRefPtr<ChildImpl> mActor;
  nsRefPtr<ParentImpl> mParentActor;
  MessageLoop* mParentMessageLoop;

public:
  OpenMainProcessActorRunnable(already_AddRefed<ChildImpl>&& aChildActor,
                               already_AddRefed<ParentImpl> aParentActor,
                               MessageLoop* aParentMessageLoop)
  : mActor(aChildActor), mParentActor(aParentActor),
    mParentMessageLoop(aParentMessageLoop)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(mParentActor);
    MOZ_ASSERT(aParentMessageLoop);
  }

  NS_DECL_ISUPPORTS_INHERITED

private:
  ~OpenMainProcessActorRunnable()
  { }

  NS_DECL_NSIRUNNABLE
};

} // anonymous namespace

namespace mozilla {
namespace ipc {

bool
IsOnBackgroundThread()
{
  return ParentImpl::IsOnBackgroundThread();
}

#ifdef DEBUG

void
AssertIsOnBackgroundThread()
{
  ParentImpl::AssertIsOnBackgroundThread();
}

#endif // DEBUG

} // namespace ipc
} // namespace mozilla

// -----------------------------------------------------------------------------
// BackgroundParent Public Methods
// -----------------------------------------------------------------------------

// static
bool
BackgroundParent::IsOtherProcessActor(PBackgroundParent* aBackgroundActor)
{
  return ParentImpl::IsOtherProcessActor(aBackgroundActor);
}

// static
already_AddRefed<ContentParent>
BackgroundParent::GetContentParent(PBackgroundParent* aBackgroundActor)
{
  return ParentImpl::GetContentParent(aBackgroundActor);
}

// static
PBackgroundParent*
BackgroundParent::Alloc(ContentParent* aContent,
                        Transport* aTransport,
                        ProcessId aOtherProcess)
{
  return ParentImpl::Alloc(aContent, aTransport, aOtherProcess);
}

// -----------------------------------------------------------------------------
// BackgroundChild Public Methods
// -----------------------------------------------------------------------------

// static
void
BackgroundChild::Startup()
{
  ChildImpl::Startup();
}

// static
PBackgroundChild*
BackgroundChild::Alloc(Transport* aTransport, ProcessId aOtherProcess)
{
  return ChildImpl::Alloc(aTransport, aOtherProcess);
}

// static
PBackgroundChild*
BackgroundChild::GetForCurrentThread()
{
  return ChildImpl::GetForCurrentThread();
}

// static
bool
BackgroundChild::GetOrCreateForCurrentThread(
                                 nsIIPCBackgroundChildCreateCallback* aCallback)
{
  return ChildImpl::GetOrCreateForCurrentThread(aCallback);
}

// static
void
BackgroundChild::CloseForCurrentThread()
{
  ChildImpl::CloseForCurrentThread();
}

// -----------------------------------------------------------------------------
// BackgroundChildImpl Public Methods
// -----------------------------------------------------------------------------

// static
BackgroundChildImpl::ThreadLocal*
BackgroundChildImpl::GetThreadLocalForCurrentThread()
{
  return ChildImpl::GetThreadLocalForCurrentThread();
}

// -----------------------------------------------------------------------------
// ParentImpl Static Members
// -----------------------------------------------------------------------------

const ParentImpl::ProcessHandle ParentImpl::kInvalidProcessHandle =
#ifdef XP_WIN
  ProcessHandle(INVALID_HANDLE_VALUE);
#else
  ProcessHandle(-1);
#endif

StaticRefPtr<nsIThread> ParentImpl::sBackgroundThread;

nsTArray<ParentImpl*>* ParentImpl::sLiveActorsForBackgroundThread;

StaticRefPtr<nsITimer> ParentImpl::sShutdownTimer;

Atomic<PRThread*> ParentImpl::sBackgroundPRThread;

MessageLoop* ParentImpl::sBackgroundThreadMessageLoop = nullptr;

uint64_t ParentImpl::sLiveActorCount = 0;

bool ParentImpl::sShutdownObserverRegistered = false;

bool ParentImpl::sShutdownHasStarted = false;

StaticAutoPtr<nsTArray<nsRefPtr<ParentImpl::CreateCallback>>>
  ParentImpl::sPendingCallbacks;

// -----------------------------------------------------------------------------
// ChildImpl Static Members
// -----------------------------------------------------------------------------

unsigned int ChildImpl::sThreadLocalIndex = kBadThreadLocalIndex;

StaticAutoPtr<nsTArray<nsCOMPtr<nsIEventTarget>>> ChildImpl::sPendingTargets;

bool ChildImpl::sShutdownHasStarted = false;

// -----------------------------------------------------------------------------
// ParentImpl Implementation
// -----------------------------------------------------------------------------

// static
bool
ParentImpl::IsOtherProcessActor(PBackgroundParent* aBackgroundActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aBackgroundActor);

  return static_cast<ParentImpl*>(aBackgroundActor)->mIsOtherProcessActor;
}

// static
already_AddRefed<ContentParent>
ParentImpl::GetContentParent(PBackgroundParent* aBackgroundActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aBackgroundActor);

  auto actor = static_cast<ParentImpl*>(aBackgroundActor);
  if (actor->mActorDestroyed) {
    MOZ_ASSERT(false, "GetContentParent called after ActorDestroy was called!");
    return nullptr;
  }

  if (actor->mContent) {
    // We need to hand out a reference to our ContentParent but we also need to
    // keep the one we have. We can't call AddRef here because ContentParent is
    // not threadsafe so instead we dispatch a runnable to the main thread to do
    // it for us. This is safe since we are guaranteed that our AddRef runnable
    // will run before the reference we hand out can be released, and the
    // ContentParent can't die as long as the existing reference is maintained.
    nsCOMPtr<nsIRunnable> runnable =
      NS_NewNonOwningRunnableMethod(actor->mContent, &ContentParent::AddRef);
    MOZ_ASSERT(runnable);

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(runnable)));
  }

  return already_AddRefed<ContentParent>(actor->mContent.get());
}

// static
PBackgroundParent*
ParentImpl::Alloc(ContentParent* aContent,
                  Transport* aTransport,
                  ProcessId aOtherProcess)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(aTransport);

  ProcessHandle processHandle;
  if (!base::OpenProcessHandle(aOtherProcess, &processHandle)) {
    // Process has already died?
    return nullptr;
  }

  if (!sBackgroundThread && !CreateBackgroundThread()) {
    NS_WARNING("Failed to create background thread!");
    return nullptr;
  }

  MOZ_ASSERT(sLiveActorsForBackgroundThread);

  sLiveActorCount++;

  nsRefPtr<ParentImpl> actor = new ParentImpl(aContent, aTransport);

  nsCOMPtr<nsIRunnable> connectRunnable =
    new ConnectActorRunnable(actor, aTransport, processHandle,
                             sLiveActorsForBackgroundThread);

  if (NS_FAILED(sBackgroundThread->Dispatch(connectRunnable,
                                            NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch connect runnable!");

    MOZ_ASSERT(sLiveActorCount);
    sLiveActorCount--;

    if (!sLiveActorCount) {
      ShutdownBackgroundThread();
    }

    return nullptr;
  }

  return actor;
}

// static
bool
ParentImpl::CreateActorForSameProcess(CreateCallback* aCallback)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(aCallback);

  if (!sBackgroundThread && !CreateBackgroundThread()) {
    NS_WARNING("Failed to create background thread!");
    return false;
  }

  MOZ_ASSERT(!sShutdownHasStarted);

  sLiveActorCount++;

  if (sBackgroundThreadMessageLoop) {
    nsCOMPtr<nsIRunnable> callbackRunnable =
      new CreateCallbackRunnable(aCallback);
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(callbackRunnable)));
    return true;
  }

  if (!sPendingCallbacks) {
    sPendingCallbacks = new nsTArray<nsRefPtr<CreateCallback>>();
  }

  sPendingCallbacks->AppendElement(aCallback);
  return true;
}

// static
bool
ParentImpl::CreateBackgroundThread()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(!sBackgroundThread);
  MOZ_ASSERT(!sLiveActorsForBackgroundThread);

  if (sShutdownHasStarted) {
    NS_WARNING("Trying to create background thread after shutdown has "
               "already begun!");
    return false;
  }

  nsCOMPtr<nsITimer> newShutdownTimer;

  if (!sShutdownTimer) {
    nsresult rv;
    newShutdownTimer = do_CreateInstance(NS_TIMER_CONTRACTID, &rv);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }
  }

  if (!sShutdownObserverRegistered) {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (NS_WARN_IF(!obs)) {
      return false;
    }

    nsCOMPtr<nsIObserver> observer = new ShutdownObserver();

    nsresult rv =
      obs->AddObserver(observer, NS_XPCOM_SHUTDOWN_THREADS_OBSERVER_ID, false);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }

    sShutdownObserverRegistered = true;
  }

  nsCOMPtr<nsIThread> thread;
  if (NS_FAILED(NS_NewNamedThread("IPDL Background", getter_AddRefs(thread)))) {
    NS_WARNING("NS_NewNamedThread failed!");
    return false;
  }

  nsCOMPtr<nsIRunnable> messageLoopRunnable =
    new RequestMessageLoopRunnable(thread);
  if (NS_FAILED(thread->Dispatch(messageLoopRunnable, NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch RequestMessageLoopRunnable!");
    return false;
  }

  sBackgroundThread = thread;
  sLiveActorsForBackgroundThread = new nsTArray<ParentImpl*>(1);

  if (!sShutdownTimer) {
    MOZ_ASSERT(newShutdownTimer);
    sShutdownTimer = newShutdownTimer;
  }

  return true;
}

// static
void
ParentImpl::ShutdownBackgroundThread()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT_IF(!sBackgroundThread, !sBackgroundThreadMessageLoop);
  MOZ_ASSERT_IF(!sShutdownHasStarted, !sLiveActorCount);
  MOZ_ASSERT_IF(!sBackgroundThread, !sLiveActorCount);
  MOZ_ASSERT_IF(sBackgroundThread, sShutdownTimer);

  if (sPendingCallbacks) {
    if (!sPendingCallbacks->IsEmpty()) {
      nsTArray<nsRefPtr<CreateCallback>> callbacks;
      sPendingCallbacks->SwapElements(callbacks);

      for (uint32_t index = 0; index < callbacks.Length(); index++) {
        nsRefPtr<CreateCallback> callback;
        callbacks[index].swap(callback);
        MOZ_ASSERT(callback);

        callback->Failure();
      }
    }

    if (sShutdownHasStarted) {
      sPendingCallbacks = nullptr;
    }
  }

  nsCOMPtr<nsITimer> shutdownTimer;
  if (sShutdownHasStarted) {
    shutdownTimer = sShutdownTimer.get();
    sShutdownTimer = nullptr;
  }

  if (sBackgroundThread) {
    nsCOMPtr<nsIThread> thread = sBackgroundThread.get();
    nsAutoPtr<nsTArray<ParentImpl*>> liveActors(sLiveActorsForBackgroundThread);

    sBackgroundThread = nullptr;
    sLiveActorsForBackgroundThread = nullptr;
    sBackgroundThreadMessageLoop = nullptr;

    MOZ_ASSERT_IF(!sShutdownHasStarted, !sLiveActorCount);

    if (sShutdownHasStarted) {
      // If this is final shutdown then we need to spin the event loop while we
      // wait for all the actors to be cleaned up. We also set a timeout to
      // force-kill any hanging actors.

      if (sLiveActorCount) {
        TimerCallbackClosure closure(thread, liveActors);

        MOZ_ALWAYS_TRUE(NS_SUCCEEDED(
          shutdownTimer->InitWithFuncCallback(&ShutdownTimerCallback, &closure,
                                              kShutdownTimerDelayMS,
                                              nsITimer::TYPE_ONE_SHOT)));

        nsIThread* currentThread = NS_GetCurrentThread();
        MOZ_ASSERT(currentThread);

        while (sLiveActorCount) {
          NS_ProcessNextEvent(currentThread);
        }

        MOZ_ASSERT(liveActors->IsEmpty());

        MOZ_ALWAYS_TRUE(NS_SUCCEEDED(shutdownTimer->Cancel()));
      }
    }

    // Dispatch this runnable to unregister the thread from the profiler.
    nsCOMPtr<nsIRunnable> shutdownRunnable =
      new ShutdownBackgroundThreadRunnable();
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(thread->Dispatch(shutdownRunnable,
                                                  NS_DISPATCH_NORMAL)));

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(thread->Shutdown()));
  }
}

// static
void
ParentImpl::ShutdownTimerCallback(nsITimer* aTimer, void* aClosure)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(sShutdownHasStarted);
  MOZ_ASSERT(sLiveActorCount);

  auto closure = static_cast<TimerCallbackClosure*>(aClosure);
  MOZ_ASSERT(closure);

  // Don't let the stack unwind until the ForceCloseBackgroundActorsRunnable has
  // finished.
  sLiveActorCount++;

  nsCOMPtr<nsIRunnable> forceCloseRunnable =
    new ForceCloseBackgroundActorsRunnable(closure->mLiveActors);
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(closure->mThread->Dispatch(forceCloseRunnable,
                                                          NS_DISPATCH_NORMAL)));
}

void
ParentImpl::Destroy()
{
  // May be called on any thread!

  AssertIsInMainProcess();

  nsCOMPtr<nsIRunnable> destroyRunnable =
    NS_NewNonOwningRunnableMethod(this, &ParentImpl::MainThreadActorDestroy);
  MOZ_ASSERT(destroyRunnable);

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(destroyRunnable)));
}

void
ParentImpl::MainThreadActorDestroy()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT_IF(mIsOtherProcessActor, mContent);
  MOZ_ASSERT_IF(!mIsOtherProcessActor, !mContent);
  MOZ_ASSERT_IF(mIsOtherProcessActor, mTransport);
  MOZ_ASSERT_IF(!mIsOtherProcessActor, !mTransport);

  if (mTransport) {
    XRE_GetIOMessageLoop()->PostTask(FROM_HERE,
                                     new DeleteTask<Transport>(mTransport));
    mTransport = nullptr;
  }

  ProcessHandle otherProcess = OtherProcess();
  if (otherProcess != kInvalidProcessHandle) {
    base::CloseProcessHandle(otherProcess);
#ifdef DEBUG
    SetOtherProcess(kInvalidProcessHandle);
#endif
  }

  mContent = nullptr;

  MOZ_ASSERT(sLiveActorCount);
  sLiveActorCount--;

  if (!sLiveActorCount) {
    ShutdownBackgroundThread();
  }

  // This may be the last reference!
  Release();
}

IToplevelProtocol*
ParentImpl::CloneToplevel(const InfallibleTArray<ProtocolFdMapping>& aFds,
                          ProcessHandle aPeerProcess,
                          ProtocolCloneContext* aCtx)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  const ProtocolId protocolId = GetProtocolId();

  for (unsigned int i = 0; i < aFds.Length(); i++) {
    if (static_cast<ProtocolId>(aFds[i].protocolId()) != protocolId) {
      continue;
    }

    Transport* transport = OpenDescriptor(aFds[i].fd(),
                                          Transport::MODE_SERVER);
    if (!transport) {
      NS_WARNING("Failed to open transport!");
      break;
    }

    PBackgroundParent* clonedActor =
      Alloc(mContent, transport, base::GetProcId(aPeerProcess));
    MOZ_ASSERT(clonedActor);

    clonedActor->CloneManagees(this, aCtx);
    clonedActor->SetTransport(transport);

    return clonedActor;
  }

  return nullptr;
}

void
ParentImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!mActorDestroyed);
  MOZ_ASSERT_IF(mIsOtherProcessActor, mLiveActorArray);

  BackgroundParentImpl::ActorDestroy(aWhy);

  mActorDestroyed = true;

  if (mLiveActorArray) {
    MOZ_ALWAYS_TRUE(mLiveActorArray->RemoveElement(this));
    mLiveActorArray = nullptr;
  }

  // This is tricky. We should be able to call Destroy() here directly because
  // we're not going to touch 'this' or our MessageChannel any longer on this
  // thread. Destroy() dispatches the MainThreadActorDestroy runnable and when
  // it runs it will destroy 'this' and our associated MessageChannel. However,
  // IPDL is about to call MessageChannel::Clear() on this thread! To avoid
  // racing with the main thread we must ensure that the MessageChannel lives
  // long enough to be cleared in this call stack.
  nsCOMPtr<nsIRunnable> destroyRunnable =
    NS_NewNonOwningRunnableMethod(this, &ParentImpl::Destroy);
  MOZ_ASSERT(destroyRunnable);

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(destroyRunnable)));
}

NS_IMPL_ISUPPORTS(ParentImpl::ShutdownObserver, nsIObserver)

NS_IMETHODIMP
ParentImpl::ShutdownObserver::Observe(nsISupports* aSubject,
                                      const char* aTopic,
                                      const char16_t* aData)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(!sShutdownHasStarted);
  MOZ_ASSERT(!strcmp(aTopic, NS_XPCOM_SHUTDOWN_THREADS_OBSERVER_ID));

  sShutdownHasStarted = true;

  // Do this first before calling (and spinning the event loop in)
  // ShutdownBackgroundThread().
  ChildImpl::Shutdown();

  ShutdownBackgroundThread();

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ParentImpl::RequestMessageLoopRunnable,
                             nsRunnable)

NS_IMETHODIMP
ParentImpl::RequestMessageLoopRunnable::Run()
{
  AssertIsInMainProcess();
  MOZ_ASSERT(mTargetThread);

  if (NS_IsMainThread()) {
    MOZ_ASSERT(mMessageLoop);

    if (!sBackgroundThread ||
        !SameCOMIdentity(mTargetThread.get(), sBackgroundThread.get())) {
      return NS_OK;
    }

    MOZ_ASSERT(!sBackgroundThreadMessageLoop);
    sBackgroundThreadMessageLoop = mMessageLoop;

    if (sPendingCallbacks && !sPendingCallbacks->IsEmpty()) {
      nsTArray<nsRefPtr<CreateCallback>> callbacks;
      sPendingCallbacks->SwapElements(callbacks);

      for (uint32_t index = 0; index < callbacks.Length(); index++) {
        MOZ_ASSERT(callbacks[index]);

        nsCOMPtr<nsIRunnable> callbackRunnable =
          new CreateCallbackRunnable(callbacks[index]);
        if (NS_FAILED(NS_DispatchToCurrentThread(callbackRunnable))) {
          NS_WARNING("Failed to dispatch callback runnable!");
        }
      }
    }

    return NS_OK;
  }

  char stackBaseGuess;
  profiler_register_thread("IPDL Background", &stackBaseGuess);

#ifdef DEBUG
  {
    bool correctThread;
    MOZ_ASSERT(NS_SUCCEEDED(mTargetThread->IsOnCurrentThread(&correctThread)));
    MOZ_ASSERT(correctThread);
  }
#endif

  DebugOnly<PRThread*> oldBackgroundThread =
    sBackgroundPRThread.exchange(PR_GetCurrentThread());

  MOZ_ASSERT_IF(oldBackgroundThread,
                PR_GetCurrentThread() != oldBackgroundThread);

  MOZ_ASSERT(!mMessageLoop);

  mMessageLoop = MessageLoop::current();
  MOZ_ASSERT(mMessageLoop);

  if (NS_FAILED(NS_DispatchToMainThread(this))) {
    NS_WARNING("Failed to dispatch RequestMessageLoopRunnable to main thread!");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ParentImpl::ShutdownBackgroundThreadRunnable,
                             nsRunnable)

NS_IMETHODIMP
ParentImpl::ShutdownBackgroundThreadRunnable::Run()
{
  AssertIsInMainProcess();

  // It is possible that another background thread was created while this thread
  // was shutting down. In that case we can't assert anything about
  // sBackgroundPRThread and we should not modify it here.
  sBackgroundPRThread.compareExchange(PR_GetCurrentThread(), nullptr);

  profiler_unregister_thread();

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ParentImpl::ForceCloseBackgroundActorsRunnable,
                             nsRunnable)

NS_IMETHODIMP
ParentImpl::ForceCloseBackgroundActorsRunnable::Run()
{
  AssertIsInMainProcess();
  MOZ_ASSERT(mActorArray);

  if (NS_IsMainThread()) {
    MOZ_ASSERT(sLiveActorCount);
    sLiveActorCount--;
    return NS_OK;
  }

  AssertIsOnBackgroundThread();

  if (!mActorArray->IsEmpty()) {
    // Copy the array since calling Close() could mutate the actual array.
    nsTArray<ParentImpl*> actorsToClose(*mActorArray);

    for (uint32_t index = 0; index < actorsToClose.Length(); index++) {
      actorsToClose[index]->Close();
    }
  }

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(this)));

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ParentImpl::CreateCallbackRunnable, nsRunnable)

NS_IMETHODIMP
ParentImpl::CreateCallbackRunnable::Run()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(sBackgroundThreadMessageLoop);
  MOZ_ASSERT(mCallback);

  nsRefPtr<CreateCallback> callback;
  mCallback.swap(callback);

  nsRefPtr<ParentImpl> actor = new ParentImpl();

  callback->Success(actor.forget(), sBackgroundThreadMessageLoop);

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ParentImpl::ConnectActorRunnable, nsRunnable)

NS_IMETHODIMP
ParentImpl::ConnectActorRunnable::Run()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  // Transfer ownership to this thread. If Open() fails then we will release
  // this reference in Destroy.
  ParentImpl* actor;
  mActor.forget(&actor);

  if (!actor->Open(mTransport, mProcessHandle, XRE_GetIOMessageLoop(),
                   ParentSide)) {
    actor->Destroy();
    return NS_ERROR_FAILURE;
  }

  actor->SetLiveActorArray(mLiveActorArray);

  return NS_OK;
}

// -----------------------------------------------------------------------------
// ChildImpl Implementation
// -----------------------------------------------------------------------------

// static
void
ChildImpl::Startup()
{
  // This happens on the main thread but before XPCOM has started so we can't
  // assert that we're being called on the main thread here.

  MOZ_ASSERT(sThreadLocalIndex == kBadThreadLocalIndex,
             "BackgroundChild::Startup() called more than once!");

  PRStatus status =
    PR_NewThreadPrivateIndex(&sThreadLocalIndex, ThreadLocalDestructor);
  MOZ_RELEASE_ASSERT(status == PR_SUCCESS, "PR_NewThreadPrivateIndex failed!");

  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex);

  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  MOZ_RELEASE_ASSERT(observerService);

  nsCOMPtr<nsIObserver> observer = new ShutdownObserver();

  nsresult rv =
    observerService->AddObserver(observer,
                                 NS_XPCOM_SHUTDOWN_THREADS_OBSERVER_ID,
                                 false);
  MOZ_RELEASE_ASSERT(NS_SUCCEEDED(rv));
}

// static
void
ChildImpl::Shutdown()
{
  AssertIsOnMainThread();

  if (sShutdownHasStarted) {
    MOZ_ASSERT_IF(sThreadLocalIndex != kBadThreadLocalIndex,
                  !PR_GetThreadPrivate(sThreadLocalIndex));
    return;
  }

  sShutdownHasStarted = true;

  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex);

  DebugOnly<PRStatus> status = PR_SetThreadPrivate(sThreadLocalIndex, nullptr);
  MOZ_ASSERT(status == PR_SUCCESS);
}

// static
PBackgroundChild*
ChildImpl::Alloc(Transport* aTransport, ProcessId aOtherProcess)
{
  AssertIsInChildProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(aTransport);
  MOZ_ASSERT(sPendingTargets);
  MOZ_ASSERT(!sPendingTargets->IsEmpty());

  nsCOMPtr<nsIEventTarget> eventTarget;
  sPendingTargets->ElementAt(0).swap(eventTarget);

  sPendingTargets->RemoveElementAt(0);

  ProcessHandle processHandle;
  if (!base::OpenProcessHandle(aOtherProcess, &processHandle)) {
    MOZ_CRASH("Failed to open process handle!");
  }

  nsRefPtr<ChildImpl> actor = new ChildImpl();

  ChildImpl* weakActor = actor;

  nsCOMPtr<nsIRunnable> openRunnable =
    new OpenChildProcessActorRunnable(actor.forget(), aTransport,
                                      processHandle);
  if (NS_FAILED(eventTarget->Dispatch(openRunnable, NS_DISPATCH_NORMAL))) {
    MOZ_CRASH("Failed to dispatch OpenActorRunnable!");
  }

  // This value is only checked against null to determine success/failure, so
  // there is no need to worry about the reference count here.
  return weakActor;
}

// static
PBackgroundChild*
ChildImpl::GetForCurrentThread()
{
  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex);

  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  if (!threadLocalInfo) {
    return nullptr;
  }

  return threadLocalInfo->mActor;
}

// static
bool
ChildImpl::GetOrCreateForCurrentThread(
                                 nsIIPCBackgroundChildCreateCallback* aCallback)
{
  MOZ_ASSERT(aCallback);
  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex,
             "BackgroundChild::Startup() was never called!");

  bool created = false;

  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  if (threadLocalInfo) {
    threadLocalInfo->mCallbacks.AppendElement(aCallback);
  } else {
    nsAutoPtr<ThreadLocalInfo> newInfo(new ThreadLocalInfo(aCallback));

    if (PR_SetThreadPrivate(sThreadLocalIndex, newInfo) != PR_SUCCESS) {
      CRASH_IN_CHILD_PROCESS("PR_SetThreadPrivate failed!");
      return false;
    }

    created = true;
    threadLocalInfo = newInfo.forget();
  }

  if (threadLocalInfo->mActor) {
    // Runnable will use GetForCurrentThread() to retrieve actor again.  This
    // allows us to avoid addref'ing on the wrong thread.
    nsCOMPtr<nsIRunnable> runnable = new AlreadyCreatedCallbackRunnable();
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(runnable)));

    return true;
  }

  if (!created) {
    // We have already started the sequence for opening the actor so there's
    // nothing else we need to do here. This callback will be called after the
    // first callback in the schedule runnable.
    return true;
  }

  if (NS_IsMainThread()) {
    if (NS_WARN_IF(!OpenProtocolOnMainThread(NS_GetCurrentThread()))) {
      return false;
    }

    return true;
  }

  nsRefPtr<CreateActorRunnable> runnable = new CreateActorRunnable();
  if (NS_FAILED(NS_DispatchToMainThread(runnable))) {
    CRASH_IN_CHILD_PROCESS("Failed to dispatch to main thread!");
    return false;
  }

  return true;
}

// static
void
ChildImpl::CloseForCurrentThread()
{
  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex,
             "BackgroundChild::Startup() was never called!");
  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  // If we don't have a thread local we are in one of these conditions:
  //   1) Startup has not completed and we are racing
  //   2) We were called again after a previous close or shutdown
  // For now, these should not happen, so crash.  We can add extra complexity
  // in the future if it turns out we need to support these cases.
  if (!threadLocalInfo) {
    MOZ_CRASH("Attempting to close a non-existent PBackground actor!");
  }

  if (threadLocalInfo->mActor) {
    threadLocalInfo->mActor->FlushPendingInterruptQueue();
  }

  // Clearing the thread local will synchronously close the actor.
  DebugOnly<PRStatus> status = PR_SetThreadPrivate(sThreadLocalIndex, nullptr);
  MOZ_ASSERT(status == PR_SUCCESS);
}

// static
BackgroundChildImpl::ThreadLocal*
ChildImpl::GetThreadLocalForCurrentThread()
{
  MOZ_ASSERT(sThreadLocalIndex != kBadThreadLocalIndex,
             "BackgroundChild::Startup() was never called!");

  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  if (!threadLocalInfo) {
    return nullptr;
  }

  if (!threadLocalInfo->mConsumerThreadLocal) {
    threadLocalInfo->mConsumerThreadLocal =
      new BackgroundChildImpl::ThreadLocal();
  }

  return threadLocalInfo->mConsumerThreadLocal;
}

// static
already_AddRefed<nsIIPCBackgroundChildCreateCallback>
ChildImpl::GetNextCallback()
{
  // May run on any thread!

  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));
  MOZ_ASSERT(threadLocalInfo);

  if (threadLocalInfo->mCallbacks.IsEmpty()) {
    return nullptr;
  }

  nsCOMPtr<nsIIPCBackgroundChildCreateCallback> callback;
  threadLocalInfo->mCallbacks[0].swap(callback);

  threadLocalInfo->mCallbacks.RemoveElementAt(0);

  return callback.forget();
}

NS_IMPL_ISUPPORTS_INHERITED0(ChildImpl::AlreadyCreatedCallbackRunnable,
                             nsCancelableRunnable)

NS_IMETHODIMP
ChildImpl::AlreadyCreatedCallbackRunnable::Run()
{
  // May run on any thread!

  // Report the current actor back in the callback.
  PBackgroundChild* actor = ChildImpl::GetForCurrentThread();

  // If the current actor is null, do not create a new actor here.  This likely
  // means we are in the process of cleaning up a worker thread and do not want
  // a new actor created.  Unfortunately we cannot report back to the callback
  // because the thread local is gone at this point.  Instead simply do nothing
  // and return.
  if (NS_WARN_IF(!actor)) {
    return NS_OK;
  }

  nsCOMPtr<nsIIPCBackgroundChildCreateCallback> callback =
    ChildImpl::GetNextCallback();
  while (callback) {
    callback->ActorCreated(actor);
    callback = ChildImpl::GetNextCallback();
  }

  return NS_OK;
}

NS_IMETHODIMP
ChildImpl::AlreadyCreatedCallbackRunnable::Cancel()
{
  // These are IPC infrastructure objects and need to run unconditionally.
  Run();
  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ChildImpl::FailedCreateCallbackRunnable,
                             nsRunnable);

NS_IMETHODIMP
ChildImpl::FailedCreateCallbackRunnable::Run()
{
  // May run on any thread!

  nsCOMPtr<nsIIPCBackgroundChildCreateCallback> callback =
    ChildImpl::GetNextCallback();
  while (callback) {
    callback->ActorFailed();
    callback = ChildImpl::GetNextCallback();
  }

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ChildImpl::OpenChildProcessActorRunnable,
                             nsRunnable);

NS_IMETHODIMP
ChildImpl::OpenChildProcessActorRunnable::Run()
{
  // May be run on any thread!

  AssertIsInChildProcess();
  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mTransport);

  nsCOMPtr<nsIIPCBackgroundChildCreateCallback> callback =
    ChildImpl::GetNextCallback();
  MOZ_ASSERT(callback,
             "There should be at least one callback when first creating the "
             "actor!");

  nsRefPtr<ChildImpl> strongActor;
  mActor.swap(strongActor);

  if (!strongActor->Open(mTransport.forget(), mProcessHandle,
                         XRE_GetIOMessageLoop(), ChildSide)) {
    CRASH_IN_CHILD_PROCESS("Failed to open ChildImpl!");

    while (callback) {
      callback->ActorFailed();
      callback = ChildImpl::GetNextCallback();
    }

    return NS_OK;
  }

  // Now that Open() has succeeded transfer the ownership of the actor to IPDL.
  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  MOZ_ASSERT(threadLocalInfo);
  MOZ_ASSERT(!threadLocalInfo->mActor);

  nsRefPtr<ChildImpl>& actor = threadLocalInfo->mActor;
  strongActor.swap(actor);

  actor->SetBoundThread();

  while (callback) {
    callback->ActorCreated(actor);
    callback = ChildImpl::GetNextCallback();
  }

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ChildImpl::OpenMainProcessActorRunnable,
                             nsRunnable);

NS_IMETHODIMP
ChildImpl::OpenMainProcessActorRunnable::Run()
{
  // May run on any thread!

  AssertIsInMainProcess();
  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mParentActor);
  MOZ_ASSERT(mParentMessageLoop);

  nsCOMPtr<nsIIPCBackgroundChildCreateCallback> callback =
    ChildImpl::GetNextCallback();
  MOZ_ASSERT(callback,
             "There should be at least one callback when first creating the "
             "actor!");

  nsRefPtr<ChildImpl> strongChildActor;
  mActor.swap(strongChildActor);

  nsRefPtr<ParentImpl> parentActor;
  mParentActor.swap(parentActor);

  MessageChannel* parentChannel = parentActor->GetIPCChannel();
  MOZ_ASSERT(parentChannel);

  if (!strongChildActor->Open(parentChannel, mParentMessageLoop, ChildSide)) {
    NS_WARNING("Failed to open ChildImpl!");

    parentActor->Destroy();

    while (callback) {
      callback->ActorFailed();
      callback = ChildImpl::GetNextCallback();
    }

    return NS_OK;
  }

  // Now that Open() has succeeded transfer the ownership of the actors to IPDL.
  unused << parentActor.forget();

  auto threadLocalInfo =
    static_cast<ThreadLocalInfo*>(PR_GetThreadPrivate(sThreadLocalIndex));

  MOZ_ASSERT(threadLocalInfo);
  MOZ_ASSERT(!threadLocalInfo->mActor);

  nsRefPtr<ChildImpl>& childActor = threadLocalInfo->mActor;
  strongChildActor.swap(childActor);

  childActor->SetBoundThread();

  while (callback) {
    callback->ActorCreated(childActor);
    callback = ChildImpl::GetNextCallback();
  }

  return NS_OK;
}

NS_IMPL_ISUPPORTS_INHERITED0(ChildImpl::CreateActorRunnable, nsRunnable)

NS_IMETHODIMP
ChildImpl::CreateActorRunnable::Run()
{
  AssertIsOnMainThread();

  if (!OpenProtocolOnMainThread(mEventTarget)) {
    NS_WARNING("OpenProtocolOnMainThread failed!");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

void
ChildImpl::ParentCreateCallback::Success(
                                      already_AddRefed<ParentImpl> aParentActor,
                                      MessageLoop* aParentMessageLoop)
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  nsRefPtr<ParentImpl> parentActor = aParentActor;
  MOZ_ASSERT(parentActor);
  MOZ_ASSERT(aParentMessageLoop);
  MOZ_ASSERT(mEventTarget);

  nsRefPtr<ChildImpl> childActor = new ChildImpl();

  nsCOMPtr<nsIEventTarget> target;
  mEventTarget.swap(target);

  nsCOMPtr<nsIRunnable> openRunnable =
    new OpenMainProcessActorRunnable(childActor.forget(), parentActor.forget(),
                                     aParentMessageLoop);
  if (NS_FAILED(target->Dispatch(openRunnable, NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch open runnable!");
  }
}

void
ChildImpl::ParentCreateCallback::Failure()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();
  MOZ_ASSERT(mEventTarget);

  nsCOMPtr<nsIEventTarget> target;
  mEventTarget.swap(target);

  DispatchFailureCallback(target);
}

// static
bool
ChildImpl::OpenProtocolOnMainThread(nsIEventTarget* aEventTarget)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aEventTarget);

  if (sShutdownHasStarted) {
    MOZ_CRASH("Called BackgroundChild::GetOrCreateForCurrentThread after "
              "shutdown has started!");
  }

  if (IsMainProcess()) {
    nsRefPtr<ParentImpl::CreateCallback> parentCallback =
      new ParentCreateCallback(aEventTarget);

    if (!ParentImpl::CreateActorForSameProcess(parentCallback)) {
      NS_WARNING("BackgroundParent::CreateActor() failed!");
      DispatchFailureCallback(aEventTarget);
      return false;
    }

    return true;
  }

  ContentChild* content = ContentChild::GetSingleton();
  MOZ_ASSERT(content);

  if (!PBackground::Open(content)) {
    MOZ_CRASH("Failed to create top level actor!");
    return false;
  }

  if (!sPendingTargets) {
    sPendingTargets = new nsTArray<nsCOMPtr<nsIEventTarget>>(1);
    ClearOnShutdown(&sPendingTargets);
  }

  sPendingTargets->AppendElement(aEventTarget);

  return true;
}

// static
void
ChildImpl::DispatchFailureCallback(nsIEventTarget* aEventTarget)
{
  MOZ_ASSERT(aEventTarget);

  nsCOMPtr<nsIRunnable> callbackRunnable = new FailedCreateCallbackRunnable();
  if (NS_FAILED(aEventTarget->Dispatch(callbackRunnable, NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch CreateCallbackRunnable!");
  }
}

void
ChildImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBoundThread();

  MOZ_ASSERT(!mActorDestroyed);
  mActorDestroyed = true;

  BackgroundChildImpl::ActorDestroy(aWhy);
}

NS_IMPL_ISUPPORTS(ChildImpl::ShutdownObserver, nsIObserver)

NS_IMETHODIMP
ChildImpl::ShutdownObserver::Observe(nsISupports* aSubject,
                                     const char* aTopic,
                                     const char16_t* aData)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(!strcmp(aTopic, NS_XPCOM_SHUTDOWN_THREADS_OBSERVER_ID));

  ChildImpl::Shutdown();

  return NS_OK;
}