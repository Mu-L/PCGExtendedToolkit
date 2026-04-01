// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Filters/Points/PCGExVolumeFilter.h"

#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/PCGVolumeData.h"
#include "Details/PCGExSettingsDetails.h"
#include "Math/PCGExMathBounds.h"
#include "GameFramework/Volume.h"

#define LOCTEXT_NAMESPACE "PCGExVolumeFilterDefinition"
#define PCGEX_NAMESPACE PCGExVolumeFilterDefinition

#pragma region UPCGExVolumeFilterFactory

bool UPCGExVolumeFilterFactory::Init(FPCGExContext* InContext)
{
	if (!Super::Init(InContext)) { return false; }

	bAllShorthandsConstant = Config.ExtraRadius.CanSupportDataOnly()
		&& (!Config.bUsePenetrationThreshold || Config.PenetrationThreshold.CanSupportDataOnly());

	return true;
}

bool UPCGExVolumeFilterFactory::DomainCheck()
{
	return Config.ExtraRadius.CanSupportDataOnly()
		&& (!Config.bUsePenetrationThreshold || Config.PenetrationThreshold.CanSupportDataOnly());
}

bool UPCGExVolumeFilterFactory::SupportsProxyEvaluation() const
{
	return bAllShorthandsConstant;
}

bool UPCGExVolumeFilterFactory::SupportsCollectionEvaluation() const
{
	return bOnlyUseDataDomain;
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
	bInvert = Cfg.bInvert;
	bUsePenetrationThreshold = Cfg.bUsePenetrationThreshold;
	PenetrationMode = Cfg.PenetrationMode;

	ExtraRadius = Cfg.ExtraRadius.GetValueSetting();
	if (!ExtraRadius->Init(InPointDataFacade)) { return false; }

	if (bUsePenetrationThreshold)
	{
		PenetrationThresholdValue = Cfg.PenetrationThreshold.GetValueSetting();
		if (!PenetrationThresholdValue->Init(InPointDataFacade)) { return false; }
	}

	return true;
}

double PCGExPointFilter::FVolumeFilter::GetEffectiveRadius(const FBox& LocalBox, const int32 PointIndex) const
{
	const double BoundsRadius = ComputeRadius(LocalBox, RadiusSource);
	const double Radius = BoundsRadius + ExtraRadius->Read(PointIndex);

	if (!bUsePenetrationThreshold) { return Radius; }

	const double Threshold = PenetrationThresholdValue->Read(PointIndex);
	return ComputeEffectiveRadius(Radius, true, PenetrationMode, Threshold);
}

bool PCGExPointFilter::FVolumeFilter::TestPoint(const FVector& Position, const double EffectiveRadius) const
{
	bool bFoundInside = false;

	for (const FCachedVolume& Volume : *CachedVolumes)
	{
		if (!Volume.VolumeActor.IsValid()) { continue; }

		// AABB early-out
		if (!Volume.WorldBounds.ExpandBy(EffectiveRadius).IsInside(Position)) { continue; }

		// Let the engine handle sphere-volume overlap directly — more robust
		// than manual distance comparison for brush collision geometry.
		// DistToSurface = distance from point center to nearest surface (0 when inside).
		float DistToSurface = 0.0f;
		const bool bSphereOverlaps = Volume.VolumeActor->EncompassesPoint(Position, static_cast<float>(EffectiveRadius), &DistToSurface);
		const bool bPointInside = DistToSurface < KINDA_SMALL_NUMBER;

		switch (CheckType)
		{
		case EPCGExVolumeCheckType::IsInside:
			if (bPointInside) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::Intersects:
			if (bSphereOverlaps && !bPointInside) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::IsInsideOrIntersects:
			if (bSphereOverlaps) { return !bInvert; }
			break;

		case EPCGExVolumeCheckType::IsOutsideOrIntersects:
			if (bSphereOverlaps && !bPointInside) { return !bInvert; }
			if (bPointInside) { bFoundInside = true; }
			break;
		}
	}

	// No volume matched
	if (CheckType == EPCGExVolumeCheckType::IsOutsideOrIntersects)
	{
		return bFoundInside ? bInvert : !bInvert;
	}

	return bInvert;
}

bool PCGExPointFilter::FVolumeFilter::Test(const PCGExData::FProxyPoint& Point) const
{
	const FVector Position = Point.GetTransform().GetLocation();
	const FBox LocalBox = PCGExMath::GetLocalBounds(Point, BoundsSource);
	const double BoundsRadius = ComputeRadius(LocalBox, RadiusSource);
	const double Radius = BoundsRadius + ExtraRadius->Read(0);
	const double EffRadius = bUsePenetrationThreshold
		                         ? ComputeEffectiveRadius(Radius, true, PenetrationMode, PenetrationThresholdValue->Read(0))
		                         : Radius;
	return TestPoint(Position, EffRadius);
}

bool PCGExPointFilter::FVolumeFilter::Test(const int32 PointIndex) const
{
	const PCGExData::FConstPoint Point = PointDataFacade->Source->GetInPoint(PointIndex);
	const FVector Position = Point.GetTransform().GetLocation();
	const FBox LocalBox = PCGExMath::GetLocalBounds(Point, BoundsSource);
	return TestPoint(Position, GetEffectiveRadius(LocalBox, PointIndex));
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
	case EPCGExVolumeCheckType::IsOutsideOrIntersects: return TEXT("Is Outside or Intersects");
	}
}
#endif

#pragma endregion

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
