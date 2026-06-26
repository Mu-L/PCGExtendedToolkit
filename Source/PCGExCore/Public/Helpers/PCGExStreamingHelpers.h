// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#pragma once

#include <atomic>
#include <functional>

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include "Async/Async.h"

struct FPCGExContext;
struct FStreamableHandle;

namespace PCGExMT
{
	class IAsyncHandleGroup;
	class FTaskManager;
}

namespace PCGExHelpers
{
	using FGetPaths = std::function<TArray<FSoftObjectPath>()>;
	using FOnLoadEnd = std::function<void(const bool bSuccess, TSharedPtr<FStreamableHandle> StreamableHandle)>;
	using FOnTrackedLoadEnd = std::function<void(const bool bSuccess)>;

	// RAII keep-alive wrapper around a (possibly batched) FStreamableHandle.
	// Ownership is shared: the per-world subsystem resource cache and any number of contexts may hold a
	// reference. The underlying handle is released exactly once -- when the LAST reference drops -- via
	// the destructor, routed through SafeReleaseHandle so the release is game-thread-safe even when the
	// last owner happens to be a worker thread.
	// The wrapped handle is intentionally never exposed: the single-release guarantee depends on nobody
	// copying it out and calling ReleaseHandle() independently.
	struct PCGEXCORE_API FPCGExSharedAssetHandle
	{
		// Cache metadata (FPlatformTime seconds): bumped on every cache hit, read by the subsystem
		// eviction sweep. Atomic so a hit can refresh it without taking the cache lock.
		std::atomic<double> LastAccess;

		FPCGExSharedAssetHandle(TSharedPtr<FStreamableHandle> InHandle, const double InLastAccess)
			: LastAccess(InLastAccess), Handle(MoveTemp(InHandle))
		{
		}

		~FPCGExSharedAssetHandle();

		// True when the wrapped streamable handle is live (asset resident).
		bool IsValid() const;

		// The asset paths this (batched) handle keeps resident, read back from the streamable handle itself
		// (FStreamableHandle::RequestedAssets is the single source of truth -- the wrapper stores no path
		// copy of its own). Used by the cache to build its per-path index and prune it on eviction.
		void GetCoveredPaths(TArray<FSoftObjectPath>& OutPaths) const;

	private:
		TSharedPtr<FStreamableHandle> Handle;
	};

	using FPCGExSharedAssetHandlePtr = TSharedPtr<FPCGExSharedAssetHandle>;

	PCGEXCORE_API
	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext = nullptr);

	template <typename T>
	static TSharedPtr<FStreamableHandle> LoadBlocking_AnyThreadTpl(const TSoftObjectPtr<T>& SoftObjectPtr, FPCGExContext* InContext = nullptr)
	{
		return LoadBlocking_AnyThread(SoftObjectPtr.ToSoftObjectPath(), InContext);
	}

	PCGEXCORE_API
	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext = nullptr);

	// Cached sibling of LoadBlocking_AnyThread. Resolves the requested paths against the per-world
	// subsystem resource cache first, batch-loads only the misses as a single request, and (when
	// bCacheMisses is true) keeps loaded assets warm across executions so repeated graph runs don't reload.
	// Returns the (deduplicated) set of cache wrappers covering the requested paths; when InContext is
	// provided, each wrapper is also registered on the context for keep-alive for the duration of the
	// execution. With bCacheMisses false, cache hits are still reused but freshly-loaded misses are not
	// inserted into the cache (read-only reuse).
	// Like LoadBlocking_AnyThread, any actual load is marshaled to (and completed on) the game thread --
	// call it from a game-thread-affine prep step to stay deadlock-free under PCG cancellation.
	PCGEXCORE_API
	TArray<FPCGExSharedAssetHandlePtr> LoadAndCacheBlocking_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext = nullptr, bool bCacheMisses = true);

	PCGEXCORE_API
	FPCGExSharedAssetHandlePtr LoadAndCacheBlocking_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext = nullptr, bool bCacheMisses = true);

	// Context-required, fire-and-forget load. Always reuses already-cached resources, and caches the
	// freshly-loaded misses iff InContext->WantsResourcesCached() (so the graph author opts in per node).
	// Keep-alive is guaranteed by registration on the context -- the caller never receives a handle, so it
	// can never release one out from under the cache. This is the ergonomic front door for the many call
	// sites that just need assets resident for the duration of the node.
	PCGEXCORE_API
	void LoadBlockingTracked_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext);

	PCGEXCORE_API
	void LoadBlockingTracked_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext);

	template <typename T>
	static void LoadBlockingTracked_AnyThreadTpl(const TSoftObjectPtr<T>& SoftObjectPtr, FPCGExContext* InContext)
	{
		LoadBlockingTracked_AnyThread(SoftObjectPtr.ToSoftObjectPath(), InContext);
	}

	PCGEXCORE_API
	void Load(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FGetPaths&& GetPathsFunc, FOnLoadEnd&& OnLoadEnd);

	// Cache-aware, context-owned async sibling of Load(). Reuses already-cached resources, async-loads only
	// the misses, and caches them iff the task manager's context opted in (WantsResourcesCached()). Keep-
	// alive is fully internal -- loaded resources are registered on that context (via its weak handle, so a
	// mid-load cancellation is safe) -- so the completion callback only reports success. Use Load() instead
	// when there is no context to own the result.
	PCGEXCORE_API
	bool LoadTracked(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FGetPaths&& GetPathsFunc, FOnTrackedLoadEnd&& OnLoadEnd);

	PCGEXCORE_API
	void SafeReleaseHandle(TSharedPtr<FStreamableHandle>& InHandle);

	PCGEXCORE_API
	void SafeReleaseHandles(TArray<TSharedPtr<FStreamableHandle>>& InHandles);
}
