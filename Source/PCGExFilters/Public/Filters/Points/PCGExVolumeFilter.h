// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Core/PCGExFilterFactoryProvider.h"
#include "UObject/Object.h"

#include "Core/PCGExPointFilter.h"
#include "PCGExFilterCommon.h"

#include "PCGExVolumeFilter.generated.h"

class AVolume;
class UPCGVolumeData;

UENUM()
enum class EPCGExVolumeCheckType : uint8
{
	IsInside             = 0 UMETA(DisplayName = "Is Inside", Tooltip="Point center is inside any volume", ActionIcon="PCGEx.Pin.OUT_Filter", SearchHints = "Inside Volume"),
	Intersects           = 1 UMETA(DisplayName = "Intersects", Tooltip="Point sphere overlaps any volume boundary (not inside)", ActionIcon="PCGEx.Pin.OUT_Filter", SearchHints = "Intersects Volume"),
	IsInsideOrIntersects = 2 UMETA(DisplayName = "Is Inside or Intersects", Tooltip="Point is inside OR sphere overlaps any volume", ActionIcon="PCGEx.Pin.OUT_Filter", SearchHints = "Inside Intersects Volume"),
	IsOutside            = 3 UMETA(DisplayName = "Is Outside", Tooltip="Point center is outside ALL volumes", ActionIcon="PCGEx.Pin.OUT_Filter", SearchHints = "Outside Volume"),
	IsOutsideOrIntersects = 4 UMETA(DisplayName = "Is Outside or Intersects", Tooltip="Point is outside all volumes OR sphere overlaps any volume boundary", ActionIcon="PCGEx.Pin.OUT_Filter", SearchHints = "Outside Intersects Volume"),
};

UENUM()
enum class EPCGExVolumeRadiusSource : uint8
{
	MinExtent     = 0 UMETA(DisplayName = "Min Extent", Tooltip="Smallest axis of the local bounds half-extent"),
	MaxExtent     = 1 UMETA(DisplayName = "Max Extent", Tooltip="Largest axis of the local bounds half-extent"),
	AverageExtent = 2 UMETA(DisplayName = "Average Extent", Tooltip="Average of the three half-extent axes"),
	DiagonalHalf  = 3 UMETA(DisplayName = "Diagonal Half", Tooltip="Half the diagonal of the local bounds box"),
	Constant      = 4 UMETA(DisplayName = "Constant", Tooltip="Use a constant radius value"),
};

UENUM()
enum class EPCGExVolumePenetrationMode : uint8
{
	Discrete = 0 UMETA(DisplayName = "Discrete", Tooltip="Penetration depth in world units must meet threshold"),
	Relative = 1 UMETA(DisplayName = "Relative", Tooltip="Penetration ratio (depth / radius, 0..1) must meet threshold"),
};

namespace PCGExPointFilter
{
	struct FCachedVolume
	{
		FBox WorldBounds = FBox(EForceInit::ForceInit);
		TWeakObjectPtr<AVolume> VolumeActor = nullptr;
	};

	FORCEINLINE static bool NeedsRadius(const EPCGExVolumeCheckType InCheckType)
	{
		return InCheckType != EPCGExVolumeCheckType::IsInside && InCheckType != EPCGExVolumeCheckType::IsOutside;
	}

	FORCEINLINE static double ComputeRadius(const FBox& LocalBox, const EPCGExVolumeRadiusSource Source, const double ConstantValue)
	{
		const FVector Extent = LocalBox.GetExtent();
		switch (Source)
		{
		default:
		case EPCGExVolumeRadiusSource::MinExtent:
			return Extent.GetMin();
		case EPCGExVolumeRadiusSource::MaxExtent:
			return Extent.GetAbsMax();
		case EPCGExVolumeRadiusSource::AverageExtent:
			return (Extent.X + Extent.Y + Extent.Z) / 3.0;
		case EPCGExVolumeRadiusSource::DiagonalHalf:
			return Extent.Size();
		case EPCGExVolumeRadiusSource::Constant:
			return ConstantValue;
		}
	}

	FORCEINLINE static double ComputeEffectiveRadius(const double InRadius, const bool bUsePenetration, const EPCGExVolumePenetrationMode PenetrationMode, const double PenetrationThreshold)
	{
		if (!bUsePenetration) { return InRadius; }
		if (PenetrationMode == EPCGExVolumePenetrationMode::Discrete) { return FMath::Max(0.0, InRadius - PenetrationThreshold); }
		return InRadius * FMath::Clamp(1.0 - PenetrationThreshold, 0.0, 1.0);
	}
}

USTRUCT(BlueprintType)
struct FPCGExVolumeFilterConfig
{
	GENERATED_BODY()

	FPCGExVolumeFilterConfig()
	{
	}

	/** Type of volume check to perform. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExVolumeCheckType CheckType = EPCGExVolumeCheckType::IsInside;

	/** Bounds to use on input points for deriving the sphere radius. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="CheckType == EPCGExVolumeCheckType::Intersects || CheckType == EPCGExVolumeCheckType::IsInsideOrIntersects || CheckType == EPCGExVolumeCheckType::IsOutsideOrIntersects", EditConditionHides))
	EPCGExPointBoundsSource BoundsSource = EPCGExPointBoundsSource::ScaledBounds;

	/** How to derive the sphere radius from the point's bounds box. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="CheckType == EPCGExVolumeCheckType::Intersects || CheckType == EPCGExVolumeCheckType::IsInsideOrIntersects || CheckType == EPCGExVolumeCheckType::IsOutsideOrIntersects", EditConditionHides))
	EPCGExVolumeRadiusSource RadiusSource = EPCGExVolumeRadiusSource::MaxExtent;

	/** Constant radius value when RadiusSource is set to Constant. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="(CheckType == EPCGExVolumeCheckType::Intersects || CheckType == EPCGExVolumeCheckType::IsInsideOrIntersects || CheckType == EPCGExVolumeCheckType::IsOutsideOrIntersects) && RadiusSource == EPCGExVolumeRadiusSource::Constant", EditConditionHides, ClampMin=0))
	double ConstantRadius = 100.0;

	/** If enabled, intersection tests require a minimum penetration depth to pass. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, InlineEditConditionToggle))
	bool bUsePenetrationThreshold = false;

	/** Penetration measurement mode. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bUsePenetrationThreshold"))
	EPCGExVolumePenetrationMode PenetrationMode = EPCGExVolumePenetrationMode::Discrete;

	/** Minimum penetration to pass. Discrete = world units, Relative = 0..1 ratio of radius. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="bUsePenetrationThreshold", ClampMin=0))
	double PenetrationThreshold = 10.0;

	/** If enabled, invert the result of the test. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	bool bInvert = false;
};

/**
 * Factory for volume-based point filters.
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Filter")
class UPCGExVolumeFilterFactory : public UPCGExPointFilterFactoryData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FPCGExVolumeFilterConfig Config;

	TArray<PCGExPointFilter::FCachedVolume> CachedVolumes;

	virtual bool Init(FPCGExContext* InContext) override;

	virtual bool SupportsProxyEvaluation() const override { return true; }

	virtual TSharedPtr<PCGExPointFilter::IFilter> CreateFilter() const override;

	virtual bool WantsPreparation(FPCGExContext* InContext) override { return true; }
	virtual PCGExFactories::EPreparationResult Prepare(FPCGExContext* InContext, const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override;

	virtual void BeginDestroy() override;
};

namespace PCGExPointFilter
{
	class FVolumeFilter final : public ISimpleFilter
	{
	public:
		explicit FVolumeFilter(const TObjectPtr<const UPCGExVolumeFilterFactory>& InFactory)
			: ISimpleFilter(InFactory), TypedFilterFactory(InFactory)
		{
		}

		const TObjectPtr<const UPCGExVolumeFilterFactory> TypedFilterFactory;

		virtual bool Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InPointDataFacade) override;
		virtual bool Test(const PCGExData::FProxyPoint& Point) const override;
		virtual bool Test(const int32 PointIndex) const override;
		virtual bool Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection) const override;

		virtual ~FVolumeFilter() override = default;

	private:
		const TArray<FCachedVolume>* CachedVolumes = nullptr;
		EPCGExVolumeCheckType CheckType = EPCGExVolumeCheckType::IsInside;
		EPCGExPointBoundsSource BoundsSource = EPCGExPointBoundsSource::ScaledBounds;
		EPCGExVolumeRadiusSource RadiusSource = EPCGExVolumeRadiusSource::MaxExtent;
		EPCGExVolumePenetrationMode PenetrationMode = EPCGExVolumePenetrationMode::Discrete;
		double ConstantRadius = 100.0;
		double PenetrationThreshold = 10.0;
		bool bInvert = false;
		bool bNeedsRadius = false;
		bool bUsePenetrationThreshold = false;

		bool TestPoint(const FVector& Position, double EffectiveRadius) const;
	};
}

///

UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Filter", meta=(PCGExNodeLibraryDoc="filters/point-filters/spatial/filter-inclusion-volume"))
class UPCGExVolumeFilterProviderSettings : public UPCGExFilterProviderSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	PCGEX_NODE_INFOS_CUSTOM_SUBTITLE(VolumeFilterFactory, "Filter : Inclusion (Volume)", "Creates a filter definition that tests points against volume data.", PCGEX_FACTORY_NAME_PRIORITY)
	virtual TArray<FPCGPreConfiguredSettingsInfo> GetPreconfiguredInfo() const override;
#endif

	virtual void ApplyPreconfiguredSettings(const FPCGPreConfiguredSettingsInfo& PreconfigureInfo) override;

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;

public:
	/** Filter Config. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, ShowOnlyInnerProperties))
	FPCGExVolumeFilterConfig Config;

	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
	virtual bool ShowMissingDataPolicy_Internal() const override { return true; }
#endif
};
