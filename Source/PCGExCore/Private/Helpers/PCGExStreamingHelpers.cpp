// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExStreamingHelpers.h"

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Core/PCGExContext.h"
#include "PCGExSubSystem.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "UObject/SoftObjectPath.h"

namespace PCGExHelpers
{
	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext)
	{
		// Thread-safe synchronous asset loading. UAssetManager requires game-thread access,
		// so when called from a worker thread, dispatch to game thread and block until complete.
		// The context tracks the handle to prevent premature GC of loaded assets.
		TSharedPtr<FStreamableHandle> Handle;
		if (IsInGameThread())
		{
			Handle = UAssetManager::GetStreamableManager().RequestSyncLoad(Path);
			if (InContext)
			{
				InContext->TrackAssetsHandle(Handle);
			}
		}
		else
		{
			PCGExMT::ExecuteOnMainThreadAndWait([&]()
			{
				Handle = LoadBlocking_AnyThread(Path, InContext);
			});
		}

		return Handle;
	}

	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext)
	{
		TSharedPtr<FStreamableHandle> Handle;
		if (IsInGameThread())
		{
			Handle = UAssetManager::GetStreamableManager().RequestSyncLoad(Paths->Array());
			if (InContext)
			{
				InContext->TrackAssetsHandle(Handle);
			}
		}
		else
		{
			PCGExMT::ExecuteOnMainThreadAndWait([&]()
			{
				Handle = LoadBlocking_AnyThread(Paths, InContext);
			});
		}
		return Handle;
	}

	FPCGExSharedAssetHandle::~FPCGExSharedAssetHandle()
	{
		// Release exactly once, when the last owner (cache entry or any context) drops the wrapper.
		// SafeReleaseHandle marshals to the game thread if we're being destroyed off-thread.
		SafeReleaseHandle(Handle);
	}

	bool FPCGExSharedAssetHandle::IsValid() const
	{
		return Handle.IsValid() && Handle->IsActive();
	}

	void FPCGExSharedAssetHandle::GetCoveredPaths(TArray<FSoftObjectPath>& OutPaths) const
	{
		// The streamable handle already owns its requested-asset list; read it back instead of duplicating it.
		if (Handle.IsValid())
		{
			Handle->GetRequestedAssets(OutPaths);
		}
		else
		{
			OutPaths.Reset();
		}
	}

	TArray<FPCGExSharedAssetHandlePtr> LoadAndCacheBlocking_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext, const bool bCacheMisses)
	{
		TArray<FPCGExSharedAssetHandlePtr> Result;

		if (!Paths || Paths->IsEmpty())
		{
			return Result;
		}

		// Interrogate the cache FIRST, off the game thread -- it's a lock-only read. Already-resident
		// resources are reused with no marshal at all: the entire point of the cache, and what keeps a warm
		// load deadlock-free under PCG cancellation. Only an actual miss needs the game thread (RequestSyncLoad),
		// and only the misses are loaded there.
		UPCGExSubSystem* Subsystem = UPCGExSubSystem::GetSubsystemForCurrentWorld();

		TArray<FSoftObjectPath> Misses;
		if (Subsystem)
		{
			Subsystem->PeekCachedResources(*Paths, Result, Misses);
		}
		else
		{
			Misses = Paths->Array();
		}

		if (!Misses.IsEmpty())
		{
			// RequestSyncLoad is the only game-thread-bound step; the peek above already ran off-thread.
			auto LoadMisses = [&Subsystem, &Misses, bCacheMisses]() -> FPCGExSharedAssetHandlePtr
			{
				const TSharedPtr<FStreamableHandle> Handle = UAssetManager::GetStreamableManager().RequestSyncLoad(MoveTemp(Misses));
				return Subsystem
					? Subsystem->TrackLoadedBatch(Handle, bCacheMisses)
					: MakeShared<FPCGExSharedAssetHandle>(Handle, FPlatformTime::Seconds());
			};

			FPCGExSharedAssetHandlePtr MissWrapper;
			if (IsInGameThread())
			{
				MissWrapper = LoadMisses();
			}
			else
			{
				PCGExMT::ExecuteOnMainThreadAndWait([&]()
				{
					MissWrapper = LoadMisses();
				});
			}

			if (MissWrapper)
			{
				Result.Add(MissWrapper);
			}
		}

		if (InContext)
		{
			for (const FPCGExSharedAssetHandlePtr& Handle : Result)
			{
				InContext->TrackCachedAsset(Handle);
			}
		}

		return Result;
	}

	FPCGExSharedAssetHandlePtr LoadAndCacheBlocking_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext, const bool bCacheMisses)
	{
		const TSharedPtr<TSet<FSoftObjectPath>> Paths = MakeShared<TSet<FSoftObjectPath>>();
		Paths->Add(Path);

		const TArray<FPCGExSharedAssetHandlePtr> Handles = LoadAndCacheBlocking_AnyThread(Paths, InContext, bCacheMisses);
		return Handles.IsEmpty() ? nullptr : Handles[0];
	}

	void LoadBlockingTracked_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext)
	{
		check(InContext);
		// Always reuse already-cached resources; cache the freshly-loaded misses only when the node opted in
		// via WantsResourcesCached(). Keep-alive comes from registration on the context, so discard the return.
		LoadAndCacheBlocking_AnyThread(Paths, InContext, InContext->WantsResourcesCached());
	}

	void LoadBlockingTracked_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext)
	{
		check(InContext);
		LoadAndCacheBlocking_AnyThread(Path, InContext, InContext->WantsResourcesCached());
	}

	void Load(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FGetPaths&& GetPathsFunc, FOnLoadEnd&& OnLoadEnd)
	{
		check(TaskManager);

		// Async asset loading integrated with the PCGEx task system.
		// Dispatches to game thread (required by UAssetManager), creates a LoadToken
		// to keep the task manager alive during the async load, and fires OnLoadEnd
		// when the streamable manager completes. The token is released in both the
		// success callback and the early-completion/failure path to ensure the task
		// group's completion count stays correct.
		PCGExMT::ExecuteOnMainThread(TaskManager, [GetPathsFunc, OnLoadEnd, TaskManager]()
		{
			TArray<FSoftObjectPath> Paths = GetPathsFunc();

			if (Paths.IsEmpty())
			{
				OnLoadEnd(false, nullptr);
				return;
			}

			TWeakPtr<PCGExMT::FAsyncToken> LoadToken = TaskManager->TryCreateToken(FName("LoadToken"));
			const TSharedPtr<FStreamableHandle> LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
				MoveTemp(Paths),
				[OnLoadEnd, LoadToken](TSharedPtr<FStreamableHandle> InHandle) // NOLINT(performance-unnecessary-value-param)
				{
					OnLoadEnd(true, InHandle);
					PCGEX_ASYNC_RELEASE_CAPTURED_TOKEN(LoadToken)
				});

			// Handle already-completed or failed loads (assets were cached or paths invalid).
			if (!LoadHandle || !LoadHandle->IsActive())
			{
				if (!LoadHandle || !LoadHandle->HasLoadCompleted())
				{
					OnLoadEnd(false, LoadHandle);
				}
				else
				{
					OnLoadEnd(true, LoadHandle);
				}
				PCGEX_ASYNC_RELEASE_TOKEN(LoadToken)
			}
		});
	}

	bool LoadTracked(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FGetPaths&& GetPathsFunc, FOnTrackedLoadEnd&& OnLoadEnd)
	{
		check(TaskManager);

		// Capture the context's WEAK handle + caching decision now, while the caller's context is alive. The
		// raw FPCGExContext* from the task manager must NOT be used at async completion: the context can be
		// cancelled/destroyed while the load token keeps the manager (and this callback) alive.
		const TWeakPtr<FPCGContextHandle> CtxHandle = TaskManager->GetContext()->GetWeakSelfHandle();
		const bool bCacheMisses = TaskManager->GetContext()->WantsResourcesCached();

		// Interrogate the cache FIRST, off the game thread: resolve paths, reuse resident resources, and only
		// fall through to a game-thread async load if something is actually missing. An all-cached request
		// completes here with no roundtrip at all.
		TArray<FSoftObjectPath> Paths = GetPathsFunc();
		if (Paths.IsEmpty())
		{
			OnLoadEnd(false);
			return true;
		}

		UPCGExSubSystem* Subsystem = UPCGExSubSystem::GetSubsystemForCurrentWorld();

		TArray<FPCGExSharedAssetHandlePtr> Hits;
		TArray<FSoftObjectPath> Misses;
		if (Subsystem)
		{
			TSet<FSoftObjectPath> PathSet;
			PathSet.Append(Paths);
			Subsystem->PeekCachedResources(PathSet, Hits, Misses);
		}
		else
		{
			Misses = MoveTemp(Paths);
		}

		// Register reused hits for keep-alive (skip silently if the context is already gone).
		if (!Hits.IsEmpty())
		{
			FPCGContext::FSharedContext<FPCGExContext> SharedContext(CtxHandle);
			if (FPCGExContext* Ctx = SharedContext.Get())
			{
				for (const FPCGExSharedAssetHandlePtr& Handle : Hits)
				{
					Ctx->TrackCachedAsset(Handle);
				}
			}
		}

		// Fully served from the cache -- complete now, no game-thread roundtrip. The surrounding async flow
		// must tolerate this synchronous completion (caller's responsibility).
		if (Misses.IsEmpty())
		{
			OnLoadEnd(true);
			return true;
		}

		// Misses remain: marshal ONLY the async load to the game thread.
		PCGExMT::ExecuteOnMainThread(TaskManager, [Misses = MoveTemp(Misses), OnLoadEnd, TaskManager, CtxHandle, Subsystem, bCacheMisses]() mutable
		{
			TWeakPtr<PCGExMT::FAsyncToken> LoadToken = TaskManager->TryCreateToken(FName("LoadToken"));

			// Wrap + (optionally) cache the freshly-loaded batch, register it on the context, report success.
			// Touches the cache + context only while the context (hence its world subsystem) is alive. Shared
			// by the async-completion and synchronously-completed paths below; the two are mutually exclusive.
			auto RegisterAndComplete = [OnLoadEnd, CtxHandle, Subsystem, bCacheMisses](const TSharedPtr<FStreamableHandle>& InHandle)
			{
				FPCGContext::FSharedContext<FPCGExContext> SharedContext(CtxHandle);
				if (FPCGExContext* Ctx = SharedContext.Get())
				{
					FPCGExSharedAssetHandlePtr Wrapper;
					if (Subsystem)
					{
						Wrapper = Subsystem->TrackLoadedBatch(InHandle, bCacheMisses);
					}
					else
						if (InHandle.IsValid())
						{
							Wrapper = MakeShared<FPCGExSharedAssetHandle>(InHandle, FPlatformTime::Seconds());
						}
					if (Wrapper)
					{
						Ctx->TrackCachedAsset(Wrapper);
					}
				}

				OnLoadEnd(InHandle.IsValid() && InHandle->HasLoadCompleted());
			};

			const TSharedPtr<FStreamableHandle> LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
				MoveTemp(Misses),
				[RegisterAndComplete, LoadToken](TSharedPtr<FStreamableHandle> InHandle) // NOLINT(performance-unnecessary-value-param)
				{
					RegisterAndComplete(InHandle);
					PCGEX_ASYNC_RELEASE_CAPTURED_TOKEN(LoadToken)
				});

			// Synchronously already-completed or failed: the async callback won't fire, so finish here.
			if (!LoadHandle || !LoadHandle->IsActive())
			{
				RegisterAndComplete(LoadHandle);
				PCGEX_ASYNC_RELEASE_TOKEN(LoadToken)
			}
		});

		return false;
	}

	void SafeReleaseHandle(TSharedPtr<FStreamableHandle>& InHandle)
	{
		if (!InHandle.IsValid())
		{
			return;
		}

		if (IsInGameThread())
		{
			InHandle->ReleaseHandle();
			InHandle.Reset();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Handle = MoveTemp(InHandle)]()
			{
				if (Handle.IsValid())
				{
					Handle->ReleaseHandle();
				}
			});
		}
	}

	void SafeReleaseHandles(TArray<TSharedPtr<FStreamableHandle>>& InHandles)
	{
		if (InHandles.IsEmpty())
		{
			return;
		}

		if (IsInGameThread())
		{
			for (TSharedPtr<FStreamableHandle>& Handle : InHandles)
			{
				if (Handle.IsValid())
				{
					Handle->ReleaseHandle();
				}
			}
			InHandles.Empty();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Handles = MoveTemp(InHandles)]()
			{
				for (const TSharedPtr<FStreamableHandle>& Handle : Handles)
				{
					if (Handle.IsValid())
					{
						Handle->ReleaseHandle();
					}
				}
			});
		}
	}

}
