#include <stdint.h>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "WAVM/IR/Types.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/IndexMap.h"
#include "WAVM/Inline/IntrusiveSharedPtr.h"
#include "WAVM/Inline/Lock.h"
#include "WAVM/Platform/Mutex.h"
#include "WAVM/Platform/Thread.h"
#include "WAVM/Runtime/Intrinsics.h"
#include "WAVM/Runtime/Runtime.h"
#include "WAVM/Runtime/RuntimeData.h"
#include "WAVM/ThreadTest/ThreadTest.h"

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;

enum
{
	numStackBytes = 1 * 1024 * 1024
};

// Keeps track of the entry function used by a running WebAssembly-spawned thread.
// Used to find garbage collection roots.
struct Thread
{
	Uptr id = UINTPTR_MAX;
	std::atomic<Uptr> numRefs{0};

	Platform::Thread* platformThread = nullptr;
	Runtime::GCPointer<Runtime::Context> context;
	Runtime::GCPointer<Runtime::Function> entryFunction;

	IR::Value argument;

	FORCENOINLINE Thread(Runtime::Context* inContext,
						 Runtime::Function* inEntryFunction,
						 const IR::Value& inArgument)
	: context(inContext), entryFunction(inEntryFunction), argument(inArgument)
	{
	}

	void addRef(Uptr delta = 1) { numRefs += delta; }
	void removeRef()
	{
		if(--numRefs == 0) { delete this; }
	}
};

// A global list of running threads created by WebAssembly code.
static Platform::Mutex threadsMutex;
static IndexMap<Uptr, IntrusiveSharedPtr<Thread>> threads(1, UINTPTR_MAX);

// A shared pointer to the current thread. This is used to decrement the thread's reference count
// when the thread exits.
thread_local IntrusiveSharedPtr<Thread> currentThread = nullptr;

// Adds the thread to the global thread array, assigning it an ID corresponding to its index in the
// array.
FORCENOINLINE static Uptr allocateThreadId(Thread* thread)
{
	Lock<Platform::Mutex> threadsLock(threadsMutex);
	thread->id = threads.add(0, thread);
	errorUnless(thread->id != 0);
	return thread->id;
}

// This function is just to provide a way to write to the currentThread thread-local variable in a
// way that the compiler can't cache across a call to Platform::forkCurrentThread.
FORCENOINLINE static void setCurrentThread(Thread* thread) { currentThread = thread; }

// Validates that a thread ID is valid. i.e. 0 < threadId < threads.size(), and threads[threadId] !=
// null If the thread ID is invalid, throws an invalid argument exception. The caller must have
// already locked threadsMutex before calling validateThreadId.
static void validateThreadId(Uptr threadId)
{
	if(threadId == 0 || !threads.contains(threadId))
	{ throwException(Exception::invalidArgumentType); }
}

DEFINE_INTRINSIC_MODULE(threadTest);

static I64 threadEntry(void* threadVoid)
{
	Thread* thread = (Thread*)threadVoid;
	currentThread = thread;
	thread->removeRef();

	return invokeFunctionUnchecked(thread->context, thread->entryFunction, &thread->argument)->i64;
}

DEFINE_INTRINSIC_FUNCTION(threadTest,
						  "createThread",
						  U64,
						  createThread,
						  Function* entryFunction,
						  I32 entryArgument)
{
	// Validate that the entry function is non-null and has the correct type (i32)->i64
	if(!entryFunction
	   || IR::FunctionType{entryFunction->encodedType}
			  != FunctionType(TypeTuple{ValueType::i64}, TypeTuple{ValueType::i32}))
	{ throwException(Runtime::Exception::indirectCallSignatureMismatchType); }

	// Create a thread object that will expose its entry and error functions to the garbage
	// collector as roots.
	auto newContext
		= createContext(getCompartmentFromContext(getContextFromRuntimeData(contextRuntimeData)));
	Thread* thread = new Thread(newContext, entryFunction, entryArgument);

	allocateThreadId(thread);

	// Increment the Thread's reference count for the pointer passed to the thread's entry function.
	// threadFunc calls the corresponding removeRef.
	thread->addRef();

	// Spawn and detach a platform thread that calls threadFunc.
	thread->platformThread = Platform::createThread(numStackBytes, threadEntry, thread);

	return thread->id;
}

DEFINE_INTRINSIC_FUNCTION_WITH_CONTEXT_SWITCH(threadTest, "forkThread", I64, forkThread)
{
	auto oldContext = getContextFromRuntimeData(contextRuntimeData);
	auto compartment = getCompartmentFromContext(oldContext);
	auto newContext = cloneContext(oldContext, compartment);

	wavmAssert(currentThread);
	Thread* childThread
		= new Thread(newContext, currentThread->entryFunction, currentThread->argument);

	// Increment the Thread's reference count twice to account for the reference to the Thread on
	// the stack which is about to be forked. Each fork calls removeRef separately below.
	childThread->addRef(2);

	Platform::Thread* platformThread = Platform::forkCurrentThread();
	if(platformThread)
	{
		// Initialize the child thread's platform thread pointer, and allocate a thread ID for it.
		childThread->platformThread = platformThread;
		const Uptr threadId = allocateThreadId(childThread);
		childThread->removeRef();

		return Intrinsics::resultInContextRuntimeData<I64>(contextRuntimeData, threadId);
	}
	else
	{
		// Move the childThread pointer into the thread-local currentThread variable. Since some
		// compilers will cache a pointer to thread-local data that's accessed multiple times in one
		// function, and currentThread is accessed before calling forkCurrentThread, we can't
		// directly write to it in this function in case the compiler tries to write to the original
		// thread's currentThread variable. Instead, call a FORCENOINLINE function
		// (setCurrentThread) to set the variable.
		setCurrentThread(childThread);
		childThread->removeRef();
		childThread = nullptr;

		// Switch the contextRuntimeData to point to the new context's runtime data.
		contextRuntimeData = getContextRuntimeData(newContext);

		return Intrinsics::resultInContextRuntimeData<I64>(contextRuntimeData, 0);
	}
}

DEFINE_INTRINSIC_FUNCTION(threadTest, "exitThread", void, exitThread, I64 code)
{
	Platform::exitThread(code);
}

// Validates a thread ID, removes the corresponding thread from the threads array, and returns it.
static IntrusiveSharedPtr<Thread> removeThreadById(Uptr threadId)
{
	IntrusiveSharedPtr<Thread> thread;

	Lock<Platform::Mutex> threadsLock(threadsMutex);
	validateThreadId(threadId);
	thread = std::move(threads[threadId]);
	threads.removeOrFail(threadId);

	wavmAssert(thread->id == Uptr(threadId));
	thread->id = UINTPTR_MAX;

	return thread;
}

DEFINE_INTRINSIC_FUNCTION(threadTest, "joinThread", I64, joinThread, U64 threadId)
{
	IntrusiveSharedPtr<Thread> thread = removeThreadById(Uptr(threadId));
	const I64 result = Platform::joinThread(thread->platformThread);
	thread->platformThread = nullptr;
	return result;
}

DEFINE_INTRINSIC_FUNCTION(threadTest, "detachThread", void, detachThread, U64 threadId)
{
	IntrusiveSharedPtr<Thread> thread = removeThreadById(Uptr(threadId));
	Platform::detachThread(thread->platformThread);
	thread->platformThread = nullptr;
}

ModuleInstance* ThreadTest::instantiate(Compartment* compartment)
{
	return Intrinsics::instantiateModule(
		compartment, INTRINSIC_MODULE_REF(threadTest), "threadTest");
}
