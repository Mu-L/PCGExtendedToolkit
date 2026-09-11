// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGPin.h"

#include "PCGExCoreMacros.h"
#include "Core/PCGExContext.h"
#include "Core/PCGExElement.h"
#include "Core/PCGExSettings.h"
#include "Components/PCGExDataCacheComponent.h"
#include "Helpers/PCGExDataCacheHelpers.h"

#include "PCGExSetCachedData.generated.h"

class AActor;

/**
 * Set Cached Data.
 * Stores the input data on the target actor's PCGEx Data Cache component under a cache ID, so a later
 * generation can read it back with Get Cached Data. Inputs pass through to same-labelled outputs.
 * In Clear / Clear All modes the node has no data pins: it is ordered purely through its (required)
 * execution dependency and exposes a dependency-only output.
 * Game-thread only: adopting data into the component re-outers and flattens it.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category = "PCGEx|Misc", meta = (Keywords = "pcgex cache store persist memory data clear", PCGExNodeLibraryDoc = "utilities/data-cache/set-cached-data"))
class UPCGExSetCachedDataSettings : public UPCGExSettings
{
	GENERATED_BODY()

	friend class FPCGExSetCachedDataElement;

public:
	//~Begin UObject interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~End UObject interface

	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(SetCachedData, "Set Cached Data", "Stores the input data on the target actor's PCGEx Data Cache component under a cache ID, so a later generation can read it back with Get Cached Data. Clear modes drop entries instead.");

	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Generic; }
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual TArray<FPCGPreConfiguredSettingsInfo> GetPreconfiguredInfo() const override;
#endif

	virtual void ApplyPreconfiguredSettings(const FPCGPreConfiguredSettingsInfo& PreconfigureInfo) override;
	virtual FString GetAdditionalTitleInformation() const override;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

public:
	/** ID the input data is stored under. Read it back with the same ID on Get Cached Data. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition = "Mode != EPCGExDataCacheWriteMode::ClearAll"))
	FName CacheID = FName("Default");

	/** What to do with the target entry. Drives the pin layout, so not overridable. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_NotOverridable))
	EPCGExDataCacheWriteMode Mode = EPCGExDataCacheWriteMode::Replace;

	/** Which actor hosts the cache. Ignored when the Target Actor pin is connected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition = "IsTargetPinUnconnected()"))
	EPCGExDataCacheTarget Target = EPCGExDataCacheTarget::ExecutingActor;

	/** 'FSoftObjectPath' attribute read on the Target Actor pin when it is connected. A component reference resolves to its owner. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	FName ActorReferenceAttribute = FName("ActorReference");

	/** Extra input pins. Data keeps the pin label it arrived on, so a Get Cached Data node with the same pins routes
	 *  it back by name. Copy-paste this array onto the Get node. Labels colliding with In, Out or Target Actor are ignored. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Settings|Pins", meta = (TitleProperty = "{Label}", EditCondition = "!IsClearMode()", EditConditionHides))
	TArray<FPCGPinProperties> CustomInputPins;

	/** Editor only. After the write, notify PCG components tracking the target actor so they can refresh from the new
	 *  cache contents. The writing component never refreshes itself. No effect on the PCG World Actor (PCG tracking
	 *  ignores it). Off by default: a cache is passive. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_NotOverridable))
	bool bNotifyChange = false;

	/** Suppress the warning when no target actor could be resolved. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Warnings and Errors")
	bool bQuietMissingTargetWarning = false;

	/** Clear and Clear All: no data pins, execution dependency required. */
	UFUNCTION()
	bool IsClearMode() const { return Mode == EPCGExDataCacheWriteMode::Clear || Mode == EPCGExDataCacheWriteMode::ClearAll; }

	/** Custom input pins minus None labels, reserved labels and duplicates; the pins actually declared. */
	TArray<FPCGPinProperties> GetSanitizedCustomInputPins() const;

protected:
#if WITH_EDITOR
	UFUNCTION()
	bool IsTargetPinUnconnected() const { return !PCGExDataCache::IsTargetPinConnected(this); }
#endif
};

struct FPCGExSetCachedDataContext final : FPCGExContext
{
	TArray<TWeakObjectPtr<AActor>> TargetActors;
};

class FPCGExSetCachedDataElement final : public IPCGExElement
{
protected:
	PCGEX_ELEMENT_CREATE_CONTEXT(SetCachedData)
	// Duplicate + flatten + re-outer all live here; Rename and Flatten (Modify) are game-thread only.
	PCGEX_ELEMENT_MAIN_THREAD_ONLY(true)

	/** The cache changes with no dependency-CRC change; a cached result would skip the write. */
	virtual bool IsCacheable(const UPCGSettings* InSettings) const override { return false; }

	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const override;
};
