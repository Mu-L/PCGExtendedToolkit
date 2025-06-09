﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGSettings.h"
#include "Data/PCGExData.h"
#include "UObject/Object.h"

#include "PCGEx.h"
#include "PCGExContext.h"
#include "PCGExMacros.h"
#include "PCGExGlobalSettings.h"
#include "Data/PCGExPointData.h"

#include "PCGExFactoryProvider.generated.h"

#define PCGEX_FACTORY_NAME_PRIORITY FName(FString::Printf(TEXT("(%d) "), Priority) +  GetDisplayName())
#define PCGEX_FACTORY_NEW_OPERATION(_TYPE) TSharedPtr<FPCGEx##_TYPE> NewOperation = MakeShared<FPCGEx##_TYPE>();

///

namespace PCGExFactories
{
	enum class EType : uint8
	{
		None = 0,
		Instanced,
		FilterGroup,
		FilterPoint,
		FilterNode,
		FilterEdge,
		FilterCollection,
		RuleSort,
		RulePartition,
		Probe,
		NodeState,
		Sampler,
		Heuristics,
		VtxProperty,
		Action,
		ShapeBuilder,
		Blending,
		TexParam,
		Tensor,
		IndexPicker,
		FillControls,
	};

	static inline TSet<EType> AnyFilters = {EType::FilterPoint, EType::FilterNode, EType::FilterEdge, EType::FilterGroup, EType::FilterCollection};
	static inline TSet<EType> PointFilters = {EType::FilterPoint, EType::FilterGroup, EType::FilterCollection};
	static inline TSet<EType> ClusterNodeFilters = {EType::FilterPoint, EType::FilterNode, EType::FilterGroup};
	static inline TSet<EType> ClusterEdgeFilters = {EType::FilterPoint, EType::FilterEdge, EType::FilterGroup};
	static inline TSet<EType> SupportsClusterFilters = {EType::FilterEdge, EType::FilterNode, EType::NodeState, EType::FilterGroup};
	static inline TSet<EType> ClusterOnlyFilters = {EType::FilterEdge, EType::FilterNode, EType::NodeState};
}

/**
 * 
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXTENDEDTOOLKIT_API UPCGExParamDataBase : public UPCGExPointData
{
	GENERATED_BODY()

public:
	virtual EPCGDataType GetDataType() const override { return EPCGDataType::Param; } //PointOrParam would be best but it's gray and I don't like it

	virtual void OutputConfigToMetadata();

};

/**
 * 
 */
UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class PCGEXTENDEDTOOLKIT_API UPCGExFactoryData : public UPCGExParamDataBase
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Priority = 0;

	UPROPERTY()
	bool bCleanupConsumableAttributes = false;

	UPROPERTY()
	bool bQuietMissingInputError = false;

	virtual PCGExFactories::EType GetFactoryType() const { return PCGExFactories::EType::None; }

	virtual bool RegisterConsumableAttributes(FPCGExContext* InContext) const;
	virtual bool RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const;
	virtual void RegisterAssetDependencies(FPCGExContext* InContext) const;
	virtual void RegisterBuffersDependencies(FPCGExContext* InContext, PCGExData::FFacadePreloader& FacadePreloader) const;

	virtual bool WantsPreparation(FPCGExContext* InContext) { return false; }
	virtual bool Prepare(FPCGExContext* InContext) { return true; }

	virtual void AddDataDependency(const UPCGData* InData);
	virtual void BeginDestroy() override;
	
protected:
	
	UPROPERTY()
	TSet<UPCGData*> DataDependencies;
	
};

UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Filter")
class PCGEXTENDEDTOOLKIT_API UPCGExFactoryProviderSettings : public UPCGSettings
{
	GENERATED_BODY()

	friend class FPCGExFactoryProviderElement;

public:
	//~Begin UObject interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~End UObject interface
	
	//~Begin UPCGSettings
#if WITH_EDITOR
	//PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(FactoryProvider, "Factory : Provider", "Creates an abstract factory provider.", FName(GetDisplayName()))
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Param; }
	virtual FLinearColor GetNodeTitleColor() const override { return GetDefault<UPCGExGlobalSettings>()->NodeColorFilter; }
	virtual bool GetPinExtraIcon(const UPCGPin* InPin, FName& OutExtraIcon, FText& OutTooltip) const override;
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

	//~Begin UPCGExFactoryProviderSettings
public:
	virtual FName GetMainOutputPin() const { return TEXT(""); }
	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory = nullptr) const;

#if WITH_EDITOR
	virtual FString GetDisplayName() const;
#endif
	//~End UPCGExFactoryProviderSettings

	/** A dummy property used to drive cache invalidation on settings changes */
	UPROPERTY()
	int32 InternalCacheInvalidator = 0;
		
	/** Cache the results of this node. Can yield unexpected result in certain cases.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Performance, meta=(PCG_NotOverridable, AdvancedDisplay))
	EPCGExOptionState CachingBehavior = EPCGExOptionState::Default;

	/** Whether this factory can register consumable attributes or not. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Cleanup", meta = (PCG_NotOverridable))
	bool bCleanupConsumableAttributes = false;

	/** If enabled, will turn off missing input errors on factories that have inputs with missing or no data. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Warnings and Errors", meta=(PCG_NotOverridable, AdvancedDisplay))
	bool bQuietMissingInputError = false;

#if WITH_EDITOR
	/** Open browse. */
	UFUNCTION(CallInEditor, Category = Tools, meta=(DisplayName="Node Documentation", ShortToolTip="Open a browser and naviguate to that node' documentation page", DisplayOrder=-1))
	void EDITOR_OpenNodeDocumentation() const;
#endif
	
protected:
	virtual bool ShouldCache() const;
};

struct PCGEXTENDEDTOOLKIT_API FPCGExFactoryProviderContext : FPCGExContext
{
	friend class FPCGExFactoryProviderElement;

	virtual ~FPCGExFactoryProviderContext() override;

	UPCGExFactoryData* OutFactory = nullptr;

	void LaunchDeferredCallback(PCGExMT::FSimpleCallback&& InCallback);

protected:
	TArray<TSharedPtr<PCGExMT::FDeferredCallbackHandle>> DeferredTasks;
};

class PCGEXTENDEDTOOLKIT_API FPCGExFactoryProviderElement final : public IPCGElement
{
public:
#if WITH_EDITOR
	virtual bool ShouldLog() const override { return false; }
#endif

protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

public:
	virtual FPCGContext* CreateContext() override
	{
		FPCGExFactoryProviderContext* NewContext = new FPCGExFactoryProviderContext();
		NewContext->SetState(PCGEx::State_InitialExecution);
		return NewContext;
	}

	virtual bool IsCacheable(const UPCGSettings* InSettings) const override;
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
};

namespace PCGExFactories
{
	template <typename T_DEF>
	static bool GetInputFactories(FPCGExContext* InContext, const FName InLabel, TArray<TObjectPtr<const T_DEF>>& OutFactories, const TSet<EType>& Types, const bool bThrowError = true)
	{
		const TArray<FPCGTaggedData>& Inputs = InContext->InputData.GetInputsByPin(InLabel);
		TSet<uint32> UniqueData;
		UniqueData.Reserve(Inputs.Num());

		for (const FPCGTaggedData& TaggedData : Inputs)
		{
			bool bIsAlreadyInSet;
			UniqueData.Add(TaggedData.Data->GetUniqueID(), &bIsAlreadyInSet);
			if (bIsAlreadyInSet) { continue; }

			if (const T_DEF* Factory = Cast<T_DEF>(TaggedData.Data))
			{
				if (!Types.Contains(Factory->GetFactoryType()))
				{
					PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(FTEXT("Input '{0}' is not supported."), FText::FromString(Factory->GetClass()->GetName())));
					continue;
				}

				OutFactories.AddUnique(Factory);
				Factory->RegisterAssetDependencies(InContext);

				Factory->RegisterConsumableAttributes(InContext);
			}
			else
			{
				PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(FTEXT("Input '{0}' is not supported."), FText::FromString(TaggedData.Data->GetClass()->GetName())));
			}
		}

		if (OutFactories.IsEmpty())
		{
			if (bThrowError) { PCGE_LOG_C(Error, GraphAndLog, InContext, FText::Format(FTEXT("Missing required '{0}' inputs."), FText::FromName(InLabel))); }
			return false;
		}

		OutFactories.Sort([](const T_DEF& A, const T_DEF& B) { return A.Priority < B.Priority; });
		
		return true;
	}

	template <typename T_DEF>
	static void RegisterConsumableAttributesWithData(const TArray<TObjectPtr<const T_DEF>>& InFactories, FPCGExContext* InContext, const UPCGData* InData)
	{
		check(InContext)

		if (!InData || InFactories.IsEmpty()) { return; }

		for (const TObjectPtr<const T_DEF>& Factory : InFactories)
		{
			if (!Factory.Get()) { continue; }
			Factory->RegisterConsumableAttributesWithData(InContext, InData);
		}
	}

	template <typename T_DEF>
	static void RegisterConsumableAttributesWithFacade(const TArray<TObjectPtr<const T_DEF>>& InFactories, const TSharedPtr<PCGExData::FFacade>& InFacade)
	{
		FPCGContext::FSharedContext<FPCGExContext> SharedContext(InFacade->Source->GetContextHandle());
		check(SharedContext.Get())

		if (!InFacade->GetIn()) { return; }

		const UPCGData* Data = InFacade->GetIn();

		if (!Data) { return; }

		for (const TObjectPtr<const T_DEF>& Factory : InFactories)
		{
			Factory->RegisterConsumableAttributesWithData(SharedContext.Get(), Data);
		}
	}

	template <typename T_DEF>
	static void RegisterConsumableAttributesWithFacade(const TObjectPtr<const T_DEF>& InFactory, const TSharedPtr<PCGExData::FFacade>& InFacade)
	{
		FPCGContext::FSharedContext<FPCGExContext> SharedContext(InFacade->Source->GetContextHandle());
		check(SharedContext.Get())

		if (!InFacade->GetIn()) { return; }

		const UPCGData* Data = InFacade->GetIn();

		if (!Data) { return; }

		InFactory->RegisterConsumableAttributesWithData(SharedContext.Get(), Data);
	}

#if WITH_EDITOR
	void EDITOR_SortPins(UPCGSettings* InSettings, FName InPin);
#endif
}
