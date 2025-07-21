﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGContext.h"
#include "PCGExHelpers.h"
#include "PCGManagedResource.h"
#include "Engine/StreamableManager.h"

namespace PCGExMT
{
	class FTaskManager;
}

namespace PCGEx
{
	using ContextState = uint64;

#define PCGEX_CTX_STATE(_NAME) const PCGEx::ContextState _NAME = GetTypeHash(FName(#_NAME));

	PCGEX_CTX_STATE(State_Preparation)
	PCGEX_CTX_STATE(State_LoadingAssetDependencies)
	PCGEX_CTX_STATE(State_AsyncPreparation)
	PCGEX_CTX_STATE(State_FacadePreloading)

	PCGEX_CTX_STATE(State_InitialExecution)
	PCGEX_CTX_STATE(State_ReadyForNextPoints)
	PCGEX_CTX_STATE(State_ProcessingPoints)

	PCGEX_CTX_STATE(State_WaitingOnAsyncWork)
	PCGEX_CTX_STATE(State_Done)

	PCGEX_CTX_STATE(State_Processing)
	PCGEX_CTX_STATE(State_Completing)
	PCGEX_CTX_STATE(State_Writing)

	PCGEX_CTX_STATE(State_UnionWriting)
}

struct PCGEXTENDEDTOOLKIT_API FPCGExContext : FPCGContext
{
protected:
	mutable FRWLock AsyncLock;
	mutable FRWLock StagedOutputLock;
	mutable FRWLock AssetDependenciesLock;

	TSharedPtr<PCGEx::FWorkPermit> WorkPermit;

	bool bFlattenOutput = false;

	TSet<FName> ConsumableAttributesSet;
	TSet<FName> ProtectedAttributesSet;

public:
	TWeakPtr<PCGEx::FWorkPermit> GetWorkPermit() { return WorkPermit; }
	TSharedPtr<PCGEx::FManagedObjects> ManagedObjects;

	bool bScopedAttributeGet = false;

	FPCGExContext();

	virtual ~FPCGExContext() override;

	void IncreaseStagedOutputReserve(const int32 InIncreaseNum);

	FPCGTaggedData& StageOutput(UPCGData* InData, const bool bManaged, const bool bIsMutable);
	void StageOutput(UPCGData* InData, const FName& InPin, const TSet<FString>& InTags, const bool bManaged, const bool bIsMutable, const bool bPinless);
	FPCGTaggedData& StageOutput(UPCGData* InData, const bool bManaged);

	UWorld* GetWorld() const;
	const UPCGComponent* GetComponent() const;
	UPCGComponent* GetMutableComponent() const;

#pragma region State

	TSharedPtr<PCGExMT::FTaskManager> GetAsyncManager();

	void PauseContext();
	void UnpauseContext();

	void SetState(const PCGEx::ContextState StateId);
	void SetAsyncState(const PCGEx::ContextState WaitState);

	virtual bool ShouldWaitForAsync();
	void ReadyForExecution();

	bool IsState(const PCGEx::ContextState StateId) const { return CurrentState.load(std::memory_order_acquire) == StateId; }
	bool IsInitialExecution() const { return IsState(PCGEx::State_InitialExecution); }
	bool IsDone() const { return IsState(PCGEx::State_Done); }
	void Done();

	virtual void OnComplete();
	bool TryComplete(const bool bForce = false);

	virtual void ResumeExecution();

protected:
	TSharedPtr<PCGExMT::FTaskManager> AsyncManager;
	bool bWaitingForAsyncCompletion = false;
	std::atomic<PCGEx::ContextState> CurrentState;

#pragma endregion

#pragma region Async resource management

public:
	void CancelAssetLoading();

	TSet<FSoftObjectPath>& GetRequiredAssets();
	bool HasAssetRequirements() const { return RequiredAssets && !RequiredAssets->IsEmpty(); }

	virtual void RegisterAssetDependencies();
	void AddAssetDependency(const FSoftObjectPath& Dependency);
	void LoadAssets();

protected:
	bool bForceSynchronousAssetLoad = false;
	bool bAssetLoadRequested = false;
	bool bAssetLoadError = false;
	TSharedPtr<TSet<FSoftObjectPath>> RequiredAssets;

	/** Handle holder for any loaded resources */
	TSharedPtr<FStreamableHandle> LoadHandle;

#pragma endregion

#pragma region Managed Components

public:
	UPCGManagedComponent* AttachManagedComponent(AActor* InParent, UActorComponent* InComponent, const FAttachmentTransformRules& AttachmentRules) const;

#pragma endregion

	mutable FRWLock ConsumableAttributesLock;
	mutable FRWLock ProtectedAttributesLock;
	bool bCleanupConsumableAttributes = false;
	TSet<FName>& GetConsumableAttributesSet() { return ConsumableAttributesSet; }
	void AddConsumableAttributeName(FName InName);
	void AddProtectedAttributeName(FName InName);

	TSharedPtr<PCGEx::FUniqueNameGenerator> UniqueNameGenerator;

	void EDITOR_TrackPath(const FSoftObjectPath& Path, bool bIsCulled = false) const;
	void EDITOR_TrackClass(const TSubclassOf<UObject>& InSelectionClass, bool bIsCulled = false) const;

	bool CanExecute() const;
	virtual bool IsAsyncWorkComplete();

	bool bQuietCancellationError = false;
	virtual bool CancelExecution(const FString& InReason);

protected:
	mutable FRWLock NotifyActorsLock;

	// Actors to notify when execution is complete
	TSet<AActor*> NotifyActors;

	void ExecuteOnNotifyActors(const TArray<FName>& FunctionNames) const;

	bool bExecutionCancelled = false;

	virtual void AddExtraStructReferencedObjects(FReferenceCollector& Collector) override;

public:
	void AddNotifyActor(AActor* InActor);
};
