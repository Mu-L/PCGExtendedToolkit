// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExStagingLoadPCGData.h"

#include "PCGDataAsset.h"
#include "PCGParamData.h"
#include "Collections/PCGExPCGDataAssetCollection.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExPointIO.h"
#include "Data/PCGLandscapeData.h"
#include "Data/PCGPointData.h"
#include "Data/PCGPolyLineData.h"
#include "Data/PCGPrimitiveData.h"
#include "Data/PCGSpatialData.h"
#include "Data/PCGSplineData.h"
#include "Data/PCGSurfaceData.h"
#include "Data/PCGVolumeData.h"
#include "Data/Utils/PCGExDataForward.h"
#include "Elements/PCGExStagingLoadProperties.h"
#include "Helpers/PCGExCollectionsHelpers.h"
#include "Helpers/PCGExRandomHelpers.h"
#include "Utils/PCGExPointIOMerger.h"

#define LOCTEXT_NAMESPACE "PCGExPCGDataAssetLoaderElement"
#define PCGEX_NAMESPACE PCGDataAssetLoader

#pragma region FPCGExSharedAssetPool

FPCGExSharedAssetPool::~FPCGExSharedAssetPool()
{
	PCGExHelpers::SafeReleaseHandle(LoadHandle);
}

void FPCGExSharedAssetPool::RegisterEntry(uint64 EntryHash, const FPCGExPCGDataAssetCollectionEntry* Entry)
{
	if (!Entry || Entry->bIsSubCollection || EntryHash == 0)
	{
		return;
	}

	FWriteScopeLock WriteLock(PoolLock);
	if (!EntryMap.Contains(EntryHash))
	{
		EntryMap.Add(EntryHash, Entry);
	}
}

void FPCGExSharedAssetPool::LoadAllAssets(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FOnLoadEnd&& OnLoadEnd)
{
	if (EntryMap.IsEmpty())
	{
		OnLoadEnd(false);
		return;
	}

	// Collect unique paths from all entries
	TSharedPtr<TSet<FSoftObjectPath>> PathsToLoad = MakeShared<TSet<FSoftObjectPath>>();
	for (const auto& Pair : EntryMap)
	{
		if (Pair.Value && Pair.Value->Staging.Path.IsValid())
		{
			PathsToLoad->Add(Pair.Value->Staging.Path);
		}
	}

	if (PathsToLoad->IsEmpty())
	{
		OnLoadEnd(false);
		return;
	}

	PCGExHelpers::Load(
		TaskManager,
		[PathsToLoad]()
		{
			return PathsToLoad->Array();
		},
		[PCGEX_ASYNC_THIS_CAPTURE, OnLoadEnd](const bool bSuccess, TSharedPtr<FStreamableHandle> StreamableHandle)
		{
			PCGEX_ASYNC_THIS
			This->LoadHandle = StreamableHandle;

			if (bSuccess)
			{
				// Map loaded assets back to entries
				for (const auto& Pair : This->EntryMap)
				{
					if (Pair.Value && Pair.Value->Staging.Path.IsValid())
					{
						TSoftObjectPtr<UPCGDataAsset> SoftPtr(Pair.Value->Staging.Path);
						if (UPCGDataAsset* LoadedAsset = SoftPtr.Get())
						{
							This->LoadedAssets.Add(Pair.Value, LoadedAsset);
						}
					}
				}
			}

			OnLoadEnd(bSuccess);
		});
}

UPCGDataAsset* FPCGExSharedAssetPool::GetAsset(uint64 EntryHash) const
{
	FReadScopeLock ReadLock(PoolLock);

	const FPCGExPCGDataAssetCollectionEntry* const* EntryPtr = EntryMap.Find(EntryHash);
	if (!EntryPtr || !*EntryPtr)
	{
		return nullptr;
	}

	return GetAsset(*EntryPtr);
}

UPCGDataAsset* FPCGExSharedAssetPool::GetAsset(const FPCGExPCGDataAssetCollectionEntry* Entry) const
{
	const TObjectPtr<UPCGDataAsset>* Found = LoadedAssets.Find(Entry);
	return Found ? Found->Get() : nullptr;
}

bool FPCGExSharedAssetPool::HasEntries() const
{
	FReadScopeLock ReadLock(PoolLock);
	return !EntryMap.IsEmpty();
}

int32 FPCGExSharedAssetPool::GetNumEntries() const
{
	FReadScopeLock ReadLock(PoolLock);
	return EntryMap.Num();
}

#pragma endregion

#pragma region FPCGExSpatialDataTransformer

namespace PCGExPCGDataAssetLoader
{
	FSpatialTransformResult::FSpatialTransformResult(ETransformResult InResult)
		: Result(InResult)
	{
	}


	FSpatialTransformResult::FSpatialTransformResult(const TSharedPtr<PCGExMT::FTask>& InTask)
		: Result(ETransformResult::Success)
		  , Task(InTask)
	{
	}

	class FTransformTask : public PCGExMT::FTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FTransformTask)

		FTransformTask(const FTransform& InTransform)
			: FTask()
			  , Transform(InTransform)
		{
		}

		const FTransform& Transform;
	};

	class FTransformPoints final : public FTransformTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FTransformPoints)

		FTransformPoints(const FTransform& InTransform, UPCGBasePointData* InData)
			: FTransformTask(InTransform)
			  , Data(InData)
		{
		}

		// Ranged: only InScope of InData. Ranged tasks share one data, so its ranges must be allocated before they start.
		FTransformPoints(const FTransform& InTransform, UPCGBasePointData* InData, const PCGExMT::FScope& InScope)
			: FTransformTask(InTransform)
			  , Data(InData)
			  , Scope(InScope)
		{
		}

		UPCGBasePointData* Data = nullptr;
		PCGExMT::FScope Scope;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			TPCGValueRange<FTransform> OutTransforms = Data->GetTransformValueRange();

			const int32 Start = Scope.IsValid() ? Scope.Start : 0;
			const int32 Count = Scope.IsValid() ? Scope.Count : OutTransforms.Num();

			PCGEX_PARALLEL_FOR(Count, OutTransforms[Start + i] *= Transform;)

			const auto* Settings = TaskManager->GetContext()->GetInputSettings<UPCGExPCGDataAssetLoaderSettings>();
			if (Settings->bRefreshSeeds)
			{
				TPCGValueRange<int32> OutSeeds = Data->GetSeedValueRange(true);
				PCGEX_PARALLEL_FOR(Count, OutSeeds[Start + i] = PCGExRandomHelpers::ComputeSpatialSeed(OutTransforms[Start + i].GetLocation());)
			}
		}
	};

	class FTransformSpline final : public FTransformTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FTransformSpline)

		FTransformSpline(const FTransform& InTransform, UPCGSplineData* InData)
			: FTransformTask(InTransform)
			  , Data(InData)
		{
		}

		UPCGSplineData* Data = nullptr;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			// Copy keys
			TArray<FInterpCurvePoint<FVector>>& Scales = const_cast<FInterpCurveVector&>(Data->SplineStruct.GetSplinePointsScale()).Points;
			TArray<FInterpCurvePoint<FQuat>>& Rotations = const_cast<FInterpCurveQuat&>(Data->SplineStruct.GetSplinePointsRotation()).Points;
			TArray<FInterpCurvePoint<FVector>>& Positions = const_cast<FInterpCurveVector&>(Data->SplineStruct.GetSplinePointsPosition()).Points;

			FVector OutScale = Transform.GetScale3D();
			for (FInterpCurvePoint<FVector>& Scale : Scales)
			{
				Scale.ArriveTangent = Transform.TransformVector(Scale.ArriveTangent);
				Scale.LeaveTangent = Transform.TransformVector(Scale.LeaveTangent);
				Scale.OutVal *= OutScale;
			}

			for (FInterpCurvePoint<FQuat>& Rotation : Rotations)
			{
				Rotation.ArriveTangent = Transform.TransformRotation(Rotation.ArriveTangent);
				Rotation.LeaveTangent = Transform.TransformRotation(Rotation.LeaveTangent);
				Rotation.OutVal = Transform.TransformRotation(Rotation.OutVal);
			}

			for (FInterpCurvePoint<FVector>& Position : Positions)
			{
				Position.ArriveTangent = Transform.TransformVector(Position.ArriveTangent);
				Position.LeaveTangent = Transform.TransformVector(Position.LeaveTangent);
				Position.OutVal = Transform.TransformPosition(Position.OutVal);
			}
		}
	};

	class FTransformPolyline final : public FTransformTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FTransformPolyline)

		FTransformPolyline(const FTransform& InTransform, UPCGPolyLineData* InData)
			: FTransformTask(InTransform)
			  , Data(InData)
		{
		}

		UPCGPolyLineData* Data = nullptr;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
		}
	};

	class FTransformVolume final : public FTransformTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FTransformVolume)

		FTransformVolume(const FTransform& InTransform, UPCGVolumeData* InData)
			: FTransformTask(InTransform)
			  , Data(InData)
		{
		}

		UPCGVolumeData* Data = nullptr;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			Data->Initialize(Data->GetStrictBounds().TransformBy(Transform));
		}
	};

	FSpatialTransformResult PrepareTransformTask(UPCGSpatialData* InData, const FTransform& InTransform, const bool bOmitIfEmpty)
	{
		if (!InData)
		{
			return FSpatialTransformResult();
		}

		if (UPCGBasePointData* PointData = Cast<UPCGBasePointData>(InData))
		{
			if (bOmitIfEmpty && PointData->IsEmpty())
			{
				return FSpatialTransformResult();
			}
			return FSpatialTransformResult(MakeShared<FTransformPoints>(InTransform, PointData));
		}

		if (UPCGSplineData* SplineData = Cast<UPCGSplineData>(InData))
		{
			if (bOmitIfEmpty && !SplineData->GetNumSegments())
			{
				return FSpatialTransformResult();
			}
			return FSpatialTransformResult(MakeShared<FTransformSpline>(InTransform, SplineData));
		}

		if (UPCGPolyLineData* PolyLineData = Cast<UPCGPolyLineData>(InData))
		{
			if (bOmitIfEmpty && !PolyLineData->GetNumSegments())
			{
				return FSpatialTransformResult();
			}
			return FSpatialTransformResult(MakeShared<FTransformPolyline>(InTransform, PolyLineData));
		}

		if (UPCGPrimitiveData* PrimitiveData = Cast<UPCGPrimitiveData>(InData))
		{
			return FSpatialTransformResult(ETransformResult::Unsupported);
		}

		if (UPCGSurfaceData* SurfaceData = Cast<UPCGSurfaceData>(InData))
		{
			return FSpatialTransformResult(ETransformResult::Unsupported);
		}

		if (UPCGVolumeData* VolumeData = Cast<UPCGVolumeData>(InData))
		{
			return FSpatialTransformResult(MakeShared<FTransformVolume>(InTransform, VolumeData));
		}

		if (UPCGLandscapeData* LandscapeData = Cast<UPCGLandscapeData>(InData))
		{
			return FSpatialTransformResult(ETransformResult::Unsupported);
		}

		return FSpatialTransformResult(ETransformResult::Unsupported);
	}
}
#pragma endregion

#pragma region FPCGExPCGDataAssetLoaderContext

void FPCGExPCGDataAssetLoaderContext::RegisterOutput(const FPCGTaggedData& InTaggedData, bool bAddPinTag, const int32 InIndex)
{
	if (!InTaggedData.Data)
	{
		return;
	}

	FName TargetPin = PCGExPCGDataAssetLoader::OutputPinDefault;

	// Check if we have a custom pin that matches
	if (CustomPinNames.Contains(InTaggedData.Pin))
	{
		TargetPin = InTaggedData.Pin;
	}

	FPCGTaggedData LocalOutputData = InTaggedData;

	// Only add Pin: tag for data going to default "Out" pin
	if (bAddPinTag && TargetPin == PCGExPCGDataAssetLoader::OutputPinDefault && !InTaggedData.Pin.IsNone())
	{
		LocalOutputData.Tags.Add(FString::Printf(TEXT("Pin:%s"), *InTaggedData.Pin.ToString()));
	}

	LocalOutputData.Pin = TargetPin;

	{
		FWriteScopeLock WriteLock(OutputLock);
		OutputByPin.FindOrAdd(TargetPin).Add(LocalOutputData);
		OutputIndices.Add(InTaggedData.Data->GetUniqueID(), InIndex);
	}
}

void FPCGExPCGDataAssetLoaderContext::RegisterUniqueData(const FPCGTaggedData& InTaggedData, const int32 InIndex)
{
	if (!InTaggedData.Data)
	{
		return;
	}

	const uint32 UID = InTaggedData.Data->GetUniqueID();

	{
		FReadScopeLock ReadLock(UniqueDataLock);
		if (UniqueDataUIDs.Contains(UID))
		{
			return;
		}
	}

	{
		FWriteScopeLock WriteLock(UniqueDataLock);

		bool bAlreadyInSet = false;
		UniqueDataUIDs.Add(UID, &bAlreadyInSet);
		if (bAlreadyInSet)
		{
			return;
		}

		// Goes to appropriate pin, with Pin: tag if going to default
		RegisterOutput(InTaggedData, true, InIndex);
	}
}

#pragma endregion

#pragma region UPCGSettings

void UPCGExPCGDataAssetLoaderSettings::InputPinPropertiesBeforeFilters(TArray<FPCGPinProperties>& PinProperties) const
{
	PCGEX_PIN_PARAMS(PCGExCollections::Labels::SourceCollectionMapLabel, "Collection map information from staging nodes or Get Collection Data.", Required)
	Super::InputPinPropertiesBeforeFilters(PinProperties);
}

TArray<FPCGPinProperties> UPCGExPCGDataAssetLoaderSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;

	// Main output pin, same label as PCGExPCGDataAssetLoader::OutputPinDefault ("Out").
	// Must be declared once and stay at index 0: RegisterOutput routes unmatched data to it,
	// and the inactive-pin bitmask in AdvanceWork assumes [Out, CustomOutputPins..., Map].
	PCGEX_PIN_ANY(GetMainOutputPin(), "Loaded data that doesn't match any custom pin, tagged with Pin:OriginalPinName. From points: spatial data is one per input point, other is single instance only. From attribute sets: asset contents as-is, once per asset, or once per row with Targets Forwarding.", Normal)

	// Custom output pins, routed by exact pin name
	for (const FPCGPinProperties& CustomPin : CustomOutputPins)
	{
		if (!CustomPin.Label.IsNone())
		{
			PinProperties.Add(CustomPin);
		}
	}

	if (bMergeEmbeddedCollectionMaps)
	{
		PCGEX_PIN_PARAMS(PCGExCollections::Labels::OutputCollectionMapLabel, "Merged collection map from embedded data assets.", Normal)
	}

	return PinProperties;
}

#pragma endregion

PCGEX_INITIALIZE_ELEMENT(PCGDataAssetLoader)
PCGEX_ELEMENT_BATCH_POINT_IMPL_ADV(PCGDataAssetLoader)

void FPCGExPCGDataAssetLoaderElement::DisabledPassThroughData(FPCGContext* Context) const
{
	FPCGExPointsProcessorElement::DisabledPassThroughData(Context);
	PCGExCollections::ForwardCollectionMap(Context);
}

bool FPCGExPCGDataAssetLoaderElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext))
	{
		return false;
	}

	PCGEX_CONTEXT_AND_SETTINGS(PCGDataAssetLoader)

	Context->CollectionUnpacker = MakeShared<PCGExCollections::FPickUnpacker>();
	Context->CollectionUnpacker->UnpackPin(InContext);

	if (!Context->CollectionUnpacker->HasValidMapping())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Could not rebuild a valid asset mapping from the provided map."));
		return false;
	}

	Context->SharedAssetPool = MakeShared<FPCGExSharedAssetPool>();
	Context->MergeCarryOver.Init();
	Context->CustomPinNames.Reserve(Settings->CustomOutputPins.Num());

	// Build custom pin name set for fast lookup
	for (const FPCGPinProperties& CustomPin : Settings->CustomOutputPins)
	{
		if (!CustomPin.Label.IsNone())
		{
			Context->CustomPinNames.Add(CustomPin.Label);
		}
	}

	return true;
}

bool FPCGExPCGDataAssetLoaderElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPCGDataAssetLoaderElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(PCGDataAssetLoader)
	PCGEX_EXECUTION_CHECK

	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartBatchProcessingPoints(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				return true;
			},
			[&](const TSharedPtr<PCGExPointsMT::IBatch>& NewBatch)
			{
				// Merged outputs are facade-written; per-target duplicates are complete after CompleteWork.
				NewBatch->bRequiresWriteStep = Settings->bMergePointOutputs;
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to process."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGExCommon::States::State_Done)

	for (auto& Pair : Context->OutputByPin)
	{
		Pair.Value.Sort([&](const FPCGTaggedData& A, const FPCGTaggedData& B)
		{
			return Context->OutputIndices[A.Data->GetUniqueID()] < Context->OutputIndices[B.Data->GetUniqueID()];
		});
		Context->OutputData.TaggedData.Append(Pair.Value);
	}

	// Mark unused pins as inactive
	int32 PinIndex = 0;

	if (!Context->OutputByPin.Contains(PCGExPCGDataAssetLoader::OutputPinDefault) ||
		Context->OutputByPin[PCGExPCGDataAssetLoader::OutputPinDefault].IsEmpty())
	{
		Context->OutputData.InactiveOutputPinBitmask |= (1ULL << 0);
	}

	PinIndex++;

	for (const FPCGPinProperties& CustomPin : Settings->CustomOutputPins)
	{
		if (!CustomPin.Label.IsNone())
		{
			if (!Context->OutputByPin.Contains(CustomPin.Label) || Context->OutputByPin[CustomPin.Label].IsEmpty())
			{
				Context->OutputData.InactiveOutputPinBitmask |= (1ULL << PinIndex);
			}
			PinIndex++;
		}
	}

	// Map pin (when bMergeEmbeddedCollectionMaps is enabled)
	if (Settings->bMergeEmbeddedCollectionMaps)
	{
		if (!Context->MergedMapPacker)
		{
			Context->OutputData.InactiveOutputPinBitmask |= (1ULL << PinIndex);
		}
		PinIndex++;
	}

	return Context->TryComplete();
}

namespace PCGExPCGDataAssetLoader
{
	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InTaskManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExPCGDataAssetLoader::Process);

		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!IProcessor::Process(InTaskManager))
		{
			return false;
		}

		PCGEX_INIT_IO(PointDataFacade->Source, PCGExData::EIOInit::NoInit)

		// Attribute-set inputs arrive as a temp identity-point conversion (see PCGExPointIO::ToPointData);
		// there is nothing to spawn onto, so loaded contents are output as-is.
		bPassthrough = PointDataFacade->Source->IsConvertedInput();

		EntryHashGetter = PointDataFacade->GetReadable<int64>(Settings->GetEntryIdxAttributeName(), PCGExData::EIOSide::In, true);
		if (!EntryHashGetter)
		{
			PCGE_LOG_C(Error, GraphAndLog, ExecutionContext, FTEXT("Missing staging hash attribute. Make sure inputs were staged (points) or come from Get Collection Data (attribute sets), with a matching Collection Map."));
			return false;
		}

		// Setup forward handler if needed
		if (Settings->TargetsForwarding.bEnabled)
		{
			ForwardHandler = Settings->TargetsForwarding.GetHandler(PointDataFacade);
		}

		// Initialize per-point hash storage
		const int32 NumPoints = PointDataFacade->GetNum();
		PointEntryHashes.SetNumZeroed(NumPoints);

		StartParallelLoopForPoints(PCGExData::EIOSide::In);

		return true;
	}

	void FProcessor::ProcessPoints(const PCGExMT::FScope& Scope)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGEx::PCGDataAssetLoader::ProcessPoints);

		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);

		// Collect entry hashes and register to shared pool - no loading here (parallel safe)
		PCGEX_SCOPE_LOOP(Index)
		{
			if (!PointFilterCache[Index])
			{
				continue;
			}

			const int64 Hash = EntryHashGetter->Read(Index);
			if (Hash == 0 || Hash == -1)
			{
				continue;
			}

			int16 SecondaryIndex = 0;
			FPCGExEntryAccessResult Result = Context->CollectionUnpacker->ResolveEntry(Hash, SecondaryIndex);

			if (!Result.IsValid())
			{
				continue;
			}

			if (!Result.Entry->IsType(PCGExAssetCollection::TypeIds::PCGDataAsset))
			{
				continue;
			}

			const FPCGExPCGDataAssetCollectionEntry* PCGDataEntry = static_cast<const FPCGExPCGDataAssetCollectionEntry*>(Result.Entry);

			PointEntryHashes[Index] = Hash;

			// Register to shared pool (thread-safe, deduplicates by hash)
			Context->SharedAssetPool->RegisterEntry(Hash, PCGDataEntry);
		}
	}

	bool FProcessor::PassesTagFilter(const FPCGTaggedData& InTaggedData) const
	{
		if (!Settings->bFilterByTags)
		{
			return true;
		}

		// Check exclude tags first
		for (const FString& ExcludeTag : Settings->ExcludeTags)
		{
			if (InTaggedData.Tags.Contains(ExcludeTag))
			{
				return false;
			}
		}

		// Check include tags (if specified)
		if (!Settings->IncludeTags.IsEmpty())
		{
			for (const FString& IncludeTag : Settings->IncludeTags)
			{
				if (InTaggedData.Tags.Contains(IncludeTag))
				{
					return true;
				}
			}
			return false;
		}

		return true;
	}

	FSpatialTransformResult FProcessor::ProcessTaggedData(int32 PointIndex, const FTransform& TargetTransform, const FPCGTaggedData& InTaggedData, FClusterIdRemapper& ClusterRemapper)
	{
		UPCGData* Data = const_cast<UPCGData*>(InTaggedData.Data.Get());
		if (!Data)
		{
			return FSpatialTransformResult();
		}

		const int32 OutIdx = BatchIndex * 1000000 + PointIndex;

		if (bPassthrough)
		{
			// Attribute-set input: no target transform, output loaded contents as-is
			ProcessPassthroughData(PointIndex, OutIdx, InTaggedData, ClusterRemapper);
			return FSpatialTransformResult();
		}

		UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Data);

		if (!SpatialData)
		{
			// Non-spatial data: register once per unique asset (not per point), ordered before spatial data
			Context->RegisterUniqueData(InTaggedData, OutIdx * -1);
			return FSpatialTransformResult();
		}

		if (ShouldMerge(InTaggedData))
		{
			QueueMerge(PointIndex, OutIdx, InTaggedData);
			return FSpatialTransformResult();
		}

		// Spatial data: duplicate and transform for this point
		UPCGSpatialData* DuplicatedData = Context->ManagedObjects->DuplicateData<UPCGSpatialData>(SpatialData);

		if (!DuplicatedData)
		{
			if (!Settings->bQuietUnsupportedTypeWarnings)
			{
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Failed to duplicate spatial data of type {0}"), FText::FromString(Data->GetClass()->GetName())));
			}
			return FSpatialTransformResult();
		}

		// Apply transform
		FSpatialTransformResult TransformResult = PrepareTransformTask(DuplicatedData, TargetTransform, Settings->bOmitEmptyData);

		if (TransformResult.Result == ETransformResult::Unsupported)
		{
			if (!Settings->bQuietUnsupportedTypeWarnings)
			{
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Spatial data type {0} does not support transformation. Data will be output untransformed."), FText::FromString(Data->GetClass()->GetName())));
			}
		}
		else if (TransformResult.Result == ETransformResult::Failed)
		{
			if (!Settings->bQuietUnsupportedTypeWarnings)
			{
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Failed to transform spatial data of type {0}"), FText::FromString(Data->GetClass()->GetName())));
			}
		}

		FPCGTaggedData OutputData;
		OutputData.Data = DuplicatedData;
		OutputData.Pin = InTaggedData.Pin;
		OutputData.Tags = InTaggedData.Tags;

		// Remap PCGEx cluster tags if present (maintains Vtx/Edges pairing with new IDs)
		RemapClusterTags(OutputData.Tags, ClusterRemapper);

		if (Settings->bForwardInputTags)
		{
			PointDataFacade->Source->Tags->DumpTo(OutputData.Tags);
		}

		// Forward attributes to point data if configured
		if (ForwardHandler)
		{
			if (UPCGMetadata* TargetMetadata = DuplicatedData->MutableMetadata())
			{
				ForwardHandler->Forward(PointIndex, TargetMetadata);
			}
		}

		// Register output (Pin: tag added only for default "Out" pin)
		Context->RegisterOutput(OutputData, true, OutIdx);
		return TransformResult;
	}

	/** Emptiness check on the original (un-duplicated) spatial data, mirroring PrepareTransformTask's bOmitIfEmpty rules */
	bool IsSpatialDataEmpty(const UPCGSpatialData* InData)
	{
		if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(InData))
		{
			return PointData->IsEmpty();
		}

		if (const UPCGSplineData* SplineData = Cast<UPCGSplineData>(InData))
		{
			return SplineData->GetNumSegments() == 0;
		}

		if (const UPCGPolyLineData* PolyLineData = Cast<UPCGPolyLineData>(InData))
		{
			return PolyLineData->GetNumSegments() == 0;
		}

		return false;
	}

	void FProcessor::ProcessPassthroughData(const int32 PointIndex, const int32 OutIdx, const FPCGTaggedData& InTaggedData, FClusterIdRemapper& ClusterRemapper)
	{
		const UPCGData* Data = InTaggedData.Data.Get();
		if (!Data)
		{
			return;
		}

		if (Settings->bOmitEmptyData)
		{
			const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Data);
			if (SpatialData && IsSpatialDataEmpty(SpatialData))
			{
				return;
			}
		}

		FPCGTaggedData OutputData = InTaggedData;

		if (Settings->bForwardInputTags)
		{
			PointDataFacade->Source->Tags->DumpTo(OutputData.Tags);
		}

		if (!ForwardHandler)
		{
			// Raw: asset-owned data goes out untouched, once per unique data. Cluster IDs stay as saved.
			Context->RegisterUniqueData(OutputData, OutIdx);
			return;
		}

		// Attribute forwarding needs writable metadata: duplicate (never transformed), one per row
		UPCGData* DuplicatedData = Context->ManagedObjects->DuplicateData<UPCGData>(Data);
		if (!DuplicatedData)
		{
			if (!Settings->bQuietUnsupportedTypeWarnings)
			{
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Failed to duplicate data of type {0}"), FText::FromString(Data->GetClass()->GetName())));
			}
			return;
		}

		OutputData.Data = DuplicatedData;

		// Per-row copies need distinct cluster IDs to keep Vtx/Edges pairs unambiguous
		RemapClusterTags(OutputData.Tags, ClusterRemapper);

		if (UPCGMetadata* TargetMetadata = DuplicatedData->MutableMetadata())
		{
			ForwardHandler->Forward(PointIndex, TargetMetadata);
		}

		Context->RegisterOutput(OutputData, true, OutIdx);
	}

	// PCGEx cluster pairing tag: PCGEx/Cluster:ID
	const FString ClusterTagPrefix = TEXT("PCGEx/Cluster:");

	bool FProcessor::ShouldMerge(const FPCGTaggedData& InTaggedData) const
	{
		if (!Settings->bMergePointOutputs || bPassthrough || !Cast<UPCGBasePointData>(InTaggedData.Data))
		{
			return false;
		}

		// Concatenated Vtx copies would collide their endpoint identities with the paired Edges
		for (const FString& Tag : InTaggedData.Tags)
		{
			if (Tag.StartsWith(ClusterTagPrefix))
			{
				return false;
			}
		}

		return true;
	}

	void FProcessor::QueueMerge(const int32 PointIndex, const int32 OutIdx, const FPCGTaggedData& InTaggedData)
	{
		const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(InTaggedData.Data);
		if (Settings->bOmitEmptyData && PointData->IsEmpty())
		{
			return;
		}

		const uint32 UID = InTaggedData.Data->GetUniqueID();
		if (const int32* GroupIndex = MergeGroupByUID.Find(UID))
		{
			MergeGroups[*GroupIndex]->TargetIndices.Add(PointIndex);
			return;
		}

		TSharedPtr<FMergeGroup> Group = MakeShared<FMergeGroup>();
		Group->Source = InTaggedData;
		Group->OutIdx = OutIdx;
		Group->TargetIndices.Add(PointIndex);
		MergeGroupByUID.Add(UID, MergeGroups.Add(Group));
	}

	void FProcessor::StartMergeGroup(const TSharedPtr<FMergeGroup>& Group)
	{
		const UPCGBasePointData* SourceData = Cast<UPCGBasePointData>(Group->Source.Data);

		Group->MergedIO = MakeShared<PCGExData::FPointIO>(PointDataFacade->Source->GetContextHandle(), SourceData);
		Group->MergedIO->SetInfos(0, OutputPinDefault);
		if (!Group->MergedIO->InitializeOutput(PCGExData::EIOInit::New))
		{
			if (!Settings->bQuietUnsupportedTypeWarnings)
			{
				PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Failed to create merged output for point data of type {0}"), FText::FromString(SourceData->GetClass()->GetName())));
			}
			return;
		}

		// Registered now (the batch only collects outputs once every task and the write step are done), ordered by first target
		FPCGTaggedData OutputData;
		OutputData.Data = Group->MergedIO->GetOut();
		OutputData.Pin = Group->Source.Pin;
		OutputData.Tags = Group->Source.Tags;

		if (Settings->bForwardInputTags)
		{
			PointDataFacade->Source->Tags->DumpTo(OutputData.Tags);
		}

		Context->RegisterOutput(OutputData, true, Group->OutIdx);

		const int32 NumSourcePoints = SourceData->GetNumPoints();
		if (NumSourcePoints <= 0)
		{
			// One empty output stands in for the empty per-target duplicates
			return;
		}

		Group->MergedFacade = MakeShared<PCGExData::FFacade>(Group->MergedIO.ToSharedRef());
		Group->Merger = MakeShared<FPCGExPointIOMerger>(Group->MergedFacade.ToSharedRef());

		const PCGExMT::FScope ReadScope(0, NumSourcePoints);
		for (int32 k = 0; k < Group->TargetIndices.Num(); k++)
		{
			Group->Merger->Append(Group->MergedIO, ReadScope, PCGExMT::FScope(k * NumSourcePoints, NumSourcePoints));
		}

		Group->Merger->MergeAsync(
			TaskManager, &Context->MergeCarryOver, nullptr, false, nullptr,
			[PCGEX_ASYNC_THIS_CAPTURE, Group]()
			{
				PCGEX_ASYNC_THIS
				This->OnMergeGroupComplete(Group);
			});
	}

	void FProcessor::OnMergeGroupComplete(const TSharedPtr<FMergeGroup>& Group)
	{
		// Runs inside a merger task: buffers are sized, so per-element writers and native ranges can be created once here
		UPCGBasePointData* MergedOut = Group->MergedFacade->GetOut();
		const int32 NumSourcePoints = Group->MergedIO->GetNum();

		EPCGPointNativeProperties Allocations = EPCGPointNativeProperties::Transform;
		if (Settings->bRefreshSeeds)
		{
			EnumAddFlags(Allocations, EPCGPointNativeProperties::Seed);
		}
		MergedOut->AllocateProperties(Allocations);

		const TSharedPtr<PCGExData::FDataForwardHandler> MergedForwardHandler = Settings->TargetsForwarding.TryGetHandler(PointDataFacade, Group->MergedFacade, PCGExData::EForwardDomain::ToElements);
		const TConstPCGValueRange<FTransform> InTransforms = PointDataFacade->GetIn()->GetConstTransformValueRange();

		TArray<TSharedPtr<PCGExMT::FTask>> Tasks;
		Tasks.Reserve(Group->TargetIndices.Num());

		for (int32 k = 0; k < Group->TargetIndices.Num(); k++)
		{
			const int32 TargetIndex = Group->TargetIndices[k];
			const PCGExMT::FScope WriteScope(k * NumSourcePoints, NumSourcePoints);

			if (MergedForwardHandler)
			{
				MergedForwardHandler->Forward(TargetIndex, WriteScope);
			}

			Tasks.Add(MakeShared<FTransformPoints>(InTransforms[TargetIndex], MergedOut, WriteScope));
		}

		PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, MergedTransformTasks)
		MergedTransformTasks->StartTasksBatch(Tasks);
	}

	void FProcessor::Write()
	{
		for (const TSharedPtr<FMergeGroup>& Group : MergeGroups)
		{
			if (Group->MergedFacade)
			{
				Group->MergedFacade->WriteFastest(TaskManager);
			}
		}
	}

	void FProcessor::RemapClusterTags(TSet<FString>& Tags, FClusterIdRemapper& ClusterRemapper) const
	{
		TArray<FString> TagsToRemove;
		TArray<FString> TagsToAdd;

		for (const FString& Tag : Tags)
		{
			if (Tag.StartsWith(ClusterTagPrefix))
			{
				// Extract the original ID
				FString IdString = Tag.Mid(ClusterTagPrefix.Len());
				int32 OriginalId = FCString::Atoi(*IdString);

				// Get the remapped ID (consistent within this point's data)
				int32 NewId = ClusterRemapper.GetRemappedId(OriginalId);

				// Queue for replacement
				TagsToRemove.Add(Tag);
				TagsToAdd.Add(FString::Printf(TEXT("%s%d"), *ClusterTagPrefix, NewId));
			}
		}

		// Apply replacements
		for (const FString& Tag : TagsToRemove)
		{
			Tags.Remove(Tag);
		}
		for (const FString& Tag : TagsToAdd)
		{
			Tags.Add(Tag);
		}
	}

	void FProcessor::CompleteWork()
	{
		// Process each point using the shared asset pool

		const UPCGBasePointData* InPointData = PointDataFacade->GetIn();
		TConstPCGValueRange<FTransform> InTransforms = InPointData->GetConstTransformValueRange();
		const int32 NumPoints = PointDataFacade->GetNum();

		TArray<TSharedPtr<PCGExMT::FTask>> Tasks;
		Tasks.Reserve(NumPoints);

		for (int32 Index = 0; Index < NumPoints; Index++)
		{
			if (!PointFilterCache[Index])
			{
				continue;
			}

			const uint64 EntryHash = PointEntryHashes[Index];
			if (EntryHash == 0)
			{
				continue;
			}

			UPCGDataAsset* DataAsset = Context->SharedAssetPool->GetAsset(EntryHash);
			if (!DataAsset)
			{
				continue;
			}

			const FTransform& TargetTransform = InTransforms[Index];

			// Create cluster ID remapper for this point - all data within this point
			// shares the same remapper so Vtx/Edges pairs maintain their relationship
			FClusterIdRemapper ClusterRemapper(ClusterIdCounter);

			for (const FPCGTaggedData& TaggedData : DataAsset->Data.GetAllInputs())
			{
				// Strip embedded CollectionMap entries (consumed by merge in FBatch::OnLoadAssetsComplete)
				if (Settings->bMergeEmbeddedCollectionMaps && TaggedData.Pin == FName(TEXT("CollectionMap")))
				{
					continue;
				}

				if (!PassesTagFilter(TaggedData))
				{
					continue;
				}

				// Process the data (cluster remapper ensures paired data gets consistent new IDs)
				FSpatialTransformResult Result = ProcessTaggedData(Index, TargetTransform, TaggedData, ClusterRemapper);
				if (Result.Task)
				{
					Tasks.Add(Result.Task);
				}
			}
		}

		for (const TSharedPtr<FMergeGroup>& Group : MergeGroups)
		{
			StartMergeGroup(Group);
		}

		if (!Tasks.IsEmpty())
		{
			PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, TransformTasks)
			TransformTasks->StartTasksBatch(Tasks);
		}
	}

	void FBatch::CompleteWork()
	{
		// Create a token to hold execution in its current state
		// Only move forward once loading is complete
		LoadingToken = TaskManager->TryCreateToken(TEXT("PCGDataAssetLoading"));
		if (!LoadingToken.IsValid())
		{
			// Token creation failed, proceed without loading
			TBatch<FProcessor>::CompleteWork();
			return;
		}

		PCGEX_TYPED_CONTEXT_AND_SETTINGS(PCGDataAssetLoader)

		if (!Context->SharedAssetPool->HasEntries())
		{
			// Nothing to load
			PCGEX_ASYNC_RELEASE_TOKEN(LoadingToken)
			TBatch<FProcessor>::CompleteWork();
			return;
		}

		Context->SharedAssetPool->LoadAllAssets(
			TaskManager,
			[PCGEX_ASYNC_THIS_CAPTURE](const bool bSuccess)
			{
				PCGEX_ASYNC_THIS
				This->OnLoadAssetsComplete(bSuccess);
			});
	}

	void FBatch::OnLoadAssetsComplete(const bool bSuccess)
	{
		if (bSuccess)
		{
			PCGEX_TYPED_CONTEXT_AND_SETTINGS(PCGDataAssetLoader)

			if (Settings->bMergeEmbeddedCollectionMaps)
			{
				Context->MergedMapPacker = MakeShared<PCGExCollections::FPickPacker>();
				PCGExCollections::FPickUnpacker TempUnpacker;

				// Scan all loaded data assets for embedded CollectionMap entries
				for (const auto& Pair : Context->SharedAssetPool->GetEntryMap())
				{
					UPCGDataAsset* Asset = Context->SharedAssetPool->GetAsset(Pair.Key);
					if (!Asset)
					{
						continue;
					}

					for (const FPCGTaggedData& TD : Asset->Data.TaggedData)
					{
						if (TD.Pin != FName(TEXT("CollectionMap")))
						{
							continue;
						}

						const UPCGParamData* ParamData = Cast<UPCGParamData>(TD.Data);
						if (!ParamData)
						{
							continue;
						}

						// Unpack into temporary unpacker (accumulates all GUID→Path pairs)
						TempUnpacker.UnpackDataset(Context, ParamData);
					}
				}

				// Re-pack merged map: iterate unpacker's collection map and register each
				// collection through the packer to produce a merged output
				if (TempUnpacker.HasValidMapping())
				{
					UPCGParamData* MergedMapData = Context->ManagedObjects->New<UPCGParamData>();

					// The unpacker's CollectionMap has (GUID → Collection*) pairs.
					// We need to produce the same format that PackToDataset writes.
					// Since GUIDs are deterministic and globally unique, the union is trivial:
					// just feed each collection through the packer once.
					for (const auto& MapPair : TempUnpacker.GetCollections())
					{
						Context->MergedMapPacker->RegisterCollection(MapPair.Value);
					}

					Context->MergedMapPacker->PackToDataset(MergedMapData);

					FPCGTaggedData MapOutput;
					MapOutput.Data = MergedMapData;
					MapOutput.Pin = PCGExCollections::Labels::OutputCollectionMapLabel;
					Context->OutputData.TaggedData.Add(MapOutput);
				}
			}

			// Assets loaded, now complete work on all processors
			TBatch<FProcessor>::CompleteWork();
		}

		PCGEX_ASYNC_RELEASE_TOKEN(LoadingToken)
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
