// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Filters/Points/PCGExVolumeFilter.h"

#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/PCGVolumeData.h"
#include "Math/PCGExMathBounds.h"
#include "GameFramework/Volume.h"

#define LOCTEXT_NAMESPACE "PCGExVolumeFilterDefinition"
#define PCGEX_NAMESPACE PCGExVolumeFilterDefinition

#pragma region UPCGExVolumeFilterFactory

bool UPCGExVolumeFilterFactory::Init(FPCGExContext* InContext)
{
	if (!Super::Init(InContext)) { return false; }
	return true;
}

TSharedPtr<PCGExPointFilter::IFilter> UPCGExVolumeFilterFactory::CreateFilter() const
{
	return MakeShared<PCGExPointFilter::FVolumeFilter>(this);
}

PCGExFactories::EPreparationResult UPCGExVolumeFilterFactory::Prepare(FPCGExContext* InContext, const TSharedPtr<PCGExMT::FTaskManager>& TaskManager)
{
	PCGExFactories::EPreparationResult Result = Super::Prepare(InContext, TaskManager);
	if (Result != PCGExFactories::EPreparationResult::Success) { return Result; }

	const TArray<FPCGTaggedData> Inputs = InContext->InputData.GetInputsByPin(FName("Volumes"));

	for (const FPCGTaggedData& TaggedData : Inputs)
	{
		const UPCGVolumeData* VolumeData = Cast<UPCGVolumeData>(TaggedData.Data);
		if (!VolumeData) { continue; }

		const TWeakObjectPtr<AVolume> VolumeActor = VolumeData->GetVolumeActor();
		if (!VolumeActor.IsValid()) { continue; }

		PCGExPointFilter::FCachedVolume& Entry = CachedVolumes.AddDefaulted_GetRef();
		Entry.WorldBounds = VolumeData->GetBounds();
		Entry.VolumeActor = VolumeActor;
	}

	if (CachedVolumes.IsEmpty())
	{
		if (MissingDataPolicy == EPCGExFilterNoDataFallback::Error) { PCGEX_LOG_MISSING_INPUT(InContext, FTEXT("Missing volume data.")) }
		return PCGExFactories::EPreparationResult::MissingData;
	}

	return Result;
}

void UPCGExVolumeFilterFactory::BeginDestroy()
{
	CachedVolumes.Empty();
	Super::BeginDestroy();
}

#pragma endregion

#pragma region FVolumeFilter

bool PCGExPointFilter::FVolumeFilter::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InPointDataFacade)
{
	if (!IFilter::Init(InContext, InPointDataFacade)) { return false; }

	CachedVolumes = &TypedFilterFactory->CachedVolumes;
	if (!CachedVolumes || CachedVolumes->IsEmpty()) { return false; }

	const FPCGExVolumeFilterConfig& Cfg = TypedFilterFactory->Config;
	CheckType = Cfg.CheckType;
	BoundsSource = Cfg.BoundsSource;
	RadiusSource = Cfg.RadiusSource;
	ConstantRadius = Cfg.ConstantRadius;
	bInvert = Cfg.bInvert;
	bNeedsRadius = PCGExPointFilter::NeedsRadius(CheckType);
	bUsePenetrationThreshold = Cfg.bUsePenetrationThreshold && bNeedsRadius;
	PenetrationMode = Cfg.PenetrationMode;
	PenetrationThreshold = Cfg.PenetrationThreshold;

	return true;
}

bool PCGExPointFilter::FVolumeFilter::TestPoint(const FVector& Position, const double EffectiveRadius) const
{
	// IsOutsideOrIntersects needs to track whether any volume fully contains the point,
	// because we keep iterating to look for intersections even after finding containment.
	bool bFoundInside = false;

	for (const FCachedVolume& Volume : *CachedVolumes)
	{
		if (!Volume.VolumeActor.IsValid()) { continue; }

		// AABB early-out: skip volumes whose bounding box (expanded by radius) doesn't contain the point
		if (bNeedsRadius)
		{
			if (!Volume.WorldBounds.ExpandBy(EffectiveRadius).IsInside(Position)) { continue; }
		}
		else
		{
			if (!Volume.WorldBounds.IsInside(Position)) { continue; }
		}

		float DistToSurface = 0.0f;
		const bool bPointInside = Volume.VolumeActor->EncompassesPoint(Position, 0.0f, &DistToSurface);

		switch (CheckType)
		{
		case EPCGExVolumeCheckType::IsInside:
			if (bPointInside) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::Intersects:
			if (!bPointInside && DistToSurface <= EffectiveRadius) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::IsInsideOrIntersects:
			if (bPointInside || DistToSurface <= EffectiveRadius) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::IsOutside:
			if (bPointInside) { return bInvert; }
			break;

		case EPCGExVolumeCheckType::IsOutsideOrIntersects:
			if (!bPointInside && DistToSurface <= EffectiveRadius) { return !bInvert; }
			if (bPointInside) { bFoundInside = true; }
			break;
		}
	}

	// Fell through all volumes
	switch (CheckType)
	{
	case EPCGExVolumeCheckType::IsOutside:
		return !bInvert;
	case EPCGExVolumeCheckType::IsOutsideOrIntersects:
		return bFoundInside ? bInvert : !bInvert;
	default:
		return bInvert;
	}
}

bool PCGExPointFilter::FVolumeFilter::Test(const PCGExData::FProxyPoint& Point) const
{
	const FVector Position = Point.GetTransform().GetLocation();

	if (!bNeedsRadius) { return TestPoint(Position, 0.0); }

	const double Radius = ComputeRadius(PCGExMath::GetLocalBounds(Point, BoundsSource), RadiusSource, ConstantRadius);
	return TestPoint(Position, ComputeEffectiveRadius(Radius, bUsePenetrationThreshold, PenetrationMode, PenetrationThreshold));
}

bool PCGExPointFilter::FVolumeFilter::Test(const int32 PointIndex) const
{
	const PCGExData::FConstPoint Point = PointDataFacade->Source->GetInPoint(PointIndex);
	const FVector Position = Point.GetTransform().GetLocation();

	if (!bNeedsRadius) { return TestPoint(Position, 0.0); }

	const double Radius = ComputeRadius(PCGExMath::GetLocalBounds(Point, BoundsSource), RadiusSource, ConstantRadius);
	return TestPoint(Position, ComputeEffectiveRadius(Radius, bUsePenetrationThreshold, PenetrationMode, PenetrationThreshold));
}

bool PCGExPointFilter::FVolumeFilter::Test(const TSharedPtr<PCGExData::FPointIO>& IO, const TSharedPtr<PCGExData::FPointIOCollection>& ParentCollection) const
{
	PCGExData::FProxyPoint ProxyPoint;
	IO->GetDataAsProxyPoint(ProxyPoint);
	return Test(ProxyPoint);
}

#pragma endregion

#pragma region UPCGExVolumeFilterProviderSettings

#if WITH_EDITOR
TArray<FPCGPreConfiguredSettingsInfo> UPCGExVolumeFilterProviderSettings::GetPreconfiguredInfo() const
{
	const TSet<EPCGExVolumeCheckType> ValuesToSkip = {};
	return FPCGPreConfiguredSettingsInfo::PopulateFromEnum<EPCGExVolumeCheckType>(ValuesToSkip, FTEXT("{0} (Volume)"));
}
#endif

void UPCGExVolumeFilterProviderSettings::ApplyPreconfiguredSettings(const FPCGPreConfiguredSettingsInfo& PreconfigureInfo)
{
	Super::ApplyPreconfiguredSettings(PreconfigureInfo);
	if (const UEnum* EnumPtr = StaticEnum<EPCGExVolumeCheckType>())
	{
		if (EnumPtr->IsValidEnumValue(PreconfigureInfo.PreconfiguredIndex))
		{
			Config.CheckType = static_cast<EPCGExVolumeCheckType>(PreconfigureInfo.PreconfiguredIndex);
		}
	}
}

TArray<FPCGPinProperties> UPCGExVolumeFilterProviderSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_VOLUMES(FName("Volumes"), TEXT("Volume data to test points against"), Required)
	return PinProperties;
}

PCGEX_CREATE_FILTER_FACTORY(Volume)

#if WITH_EDITOR
FString UPCGExVolumeFilterProviderSettings::GetDisplayName() const
{
	switch (Config.CheckType)
	{
	default:
	case EPCGExVolumeCheckType::IsInside: return TEXT("Is Inside");
	case EPCGExVolumeCheckType::Intersects: return TEXT("Intersects");
	case EPCGExVolumeCheckType::IsInsideOrIntersects: return TEXT("Is Inside or Intersects");
	case EPCGExVolumeCheckType::IsOutside: return TEXT("Is Outside");
	case EPCGExVolumeCheckType::IsOutsideOrIntersects: return TEXT("Is Outside or Intersects");
	}
}
#endif

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
