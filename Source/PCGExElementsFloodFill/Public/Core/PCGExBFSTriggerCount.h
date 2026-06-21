// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Details/PCGExInputShorthandsDetails.h"

#include "PCGExBFSTriggerCount.generated.h"

struct FPCGExContext;

namespace PCGExData
{
	class FFacade;

	template <typename T>
	class TBuffer;
}

namespace PCGExDetails
{
	template <typename T>
	class TSettingValue;
}

/**
 * Optional BFS output: a per-vtx running count of "trigger" vtx (those passing the connected Vtx Filters)
 * crossed along the BFS path from the seed. Seeded from a constant or per-seed attribute, the value
 * increments by 1 at each trigger. Unreachable vtx stay 0.
 *
 * Batch-owned (the writer lives on the shared vtx facade); processors share it and write disjoint vtx.
 */
USTRUCT(BlueprintType)
struct PCGEXELEMENTSFLOODFILL_API FPCGExBFSTriggerCountDetails
{
	GENERATED_BODY()

	FPCGExBFSTriggerCountDetails() = default;

	/** Output the running trigger count. Requires Vtx Filters to be connected (they define what a "trigger" is). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	bool bOutputTriggerCount = false;

	/** Name of the 'int32' attribute to write the trigger count to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(DisplayName="Trigger Count", PCG_Overridable, EditCondition="bOutputTriggerCount"))
	FName TriggerCountAttributeName = FName("TriggerCount");

	/** Starting value for each seed -- a constant, or read per-seed from a seed attribute. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bOutputTriggerCount"))
	FPCGExInputShorthandSelectorInteger32 InitialValue;

	/** If enabled, a trigger vtx itself receives the incremented value (counts on entry); otherwise only vtx reached after it are incremented. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bOutputTriggerCount"))
	bool bCountTriggerItself = true;

	FORCEINLINE bool WantsOutput() const { return bOutputTriggerCount; }

	/** Whether the writer was created -- use to branch once outside hot loops. */
	FORCEINLINE bool IsActive() const { return WritePtr != nullptr; }

	FORCEINLINE bool CountsTriggerItself() const { return bCountTriggerItself; }

	/** Boot-time soft validation: warns and disables the output if the attribute name is invalid. */
	void ValidateNames(FPCGExContext* InContext);

	/**
	 * Batch-time: create the output writer on the shared vtx facade and bind the initial-value reader
	 * to the seeds facade. Returns false on hard failure. No-op (returns true) when disabled.
	 */
	bool InitForBatch(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InVtxDataFacade, const TSharedRef<PCGExData::FFacade>& InSeedsDataFacade);

	/** Per-seed starting value (constant or attribute). */
	int32 GetInitialValue(const int32 SeedPointIndex) const;

	/** Write one vtx's count. Only call when IsActive(); safe across processors (disjoint vtx). */
	FORCEINLINE void Set(const int32 VtxPointIndex, const int32 Value) const
	{
		WritePtr[VtxPointIndex] = Value;
	}

protected:
	TSharedPtr<PCGExData::TBuffer<int32>> Writer;
	int32* WritePtr = nullptr;
	TSharedPtr<PCGExDetails::TSettingValue<int32>> InitialValueGetter;
};
