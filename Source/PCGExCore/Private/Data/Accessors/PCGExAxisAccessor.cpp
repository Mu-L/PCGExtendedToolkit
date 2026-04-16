// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/Accessors/PCGExAxisAccessor.h"

#include "Types/PCGExTypeOpsImpl.h"

namespace PCGExData
{
	namespace
	{
		// Mirrors STRMAP_AXIS from PCGExSubSelection.h.
		const TMap<FString, EPCGExAxis>& GetAxisTable()
		{
			static const TMap<FString, EPCGExAxis> Table = {
				{TEXT("FORWARD"),  EPCGExAxis::Forward},
				{TEXT("FRONT"),    EPCGExAxis::Forward},
				{TEXT("BACKWARD"), EPCGExAxis::Backward},
				{TEXT("BACK"),     EPCGExAxis::Backward},
				{TEXT("RIGHT"),    EPCGExAxis::Right},
				{TEXT("LEFT"),     EPCGExAxis::Left},
				{TEXT("UP"),       EPCGExAxis::Up},
				{TEXT("TOP"),      EPCGExAxis::Up},
				{TEXT("DOWN"),     EPCGExAxis::Down},
				{TEXT("BOTTOM"),   EPCGExAxis::Down},
			};
			return Table;
		}
	}

	bool FAxisAccessor::MatchesToken(const FString& UpperToken, FAccessorParseResult& OutParsed) const
	{
		if (const EPCGExAxis* Axis = GetAxisTable().Find(UpperToken))
		{
			OutParsed.Axis = *Axis;
			// Legacy STRMAP_AXIS carried Quaternion as the hint; preserve it
			// so future projection code can read it consistently.
			OutParsed.SourceTypeHint = EPCGMetadataTypes::Quaternion;
			return true;
		}
		return false;
	}

	bool FAxisAccessor::ResolveOutputType(EPCGMetadataTypes InType,
	                                      const FAccessorParseResult& Parsed,
	                                      EPCGMetadataTypes& OutType) const
	{
		(void)InType;
		(void)Parsed;
		// Axis extraction always produces a Vector. For non-rotation sources
		// the legacy fallback is FVector::ForwardVector, so the contract still
		// holds.
		OutType = EPCGMetadataTypes::Vector;
		return true;
	}

	void FAxisAccessor::ApplyGet(EPCGMetadataTypes InType,
	                             const void* Source,
	                             EPCGMetadataTypes OutType,
	                             void* OutValue,
	                             const FAccessorParseResult& Parsed) const
	{
		(void)OutType; // always Vector
		check(Source != nullptr);
		check(OutValue != nullptr);

		FVector& Out = *static_cast<FVector*>(OutValue);

		switch (InType)
		{
		case EPCGMetadataTypes::Quaternion:
			Out = PCGExTypeOps::FTypeOps<FQuat>::ExtractAxis(Source, Parsed.Axis);
			break;
		case EPCGMetadataTypes::Rotator:
			Out = PCGExTypeOps::FTypeOps<FRotator>::ExtractAxis(Source, Parsed.Axis);
			break;
		case EPCGMetadataTypes::Transform:
			Out = PCGExTypeOps::FTypeOps<FTransform>::ExtractAxis(Source, Parsed.Axis);
			break;
		default:
			// Legacy fallback for non-rotation source types: ForwardVector
			// regardless of which axis was requested. Matches
			// SubSelectionImpl::ExtractAxisDefault and the constexpr-if
			// branch in TSubSelectorOpsImpl<T>::ExtractAxis.
			Out = FVector::ForwardVector;
			break;
		}
	}

	FString FAxisAccessor::GetDisplayName() const
	{
		return TEXT("Axis");
	}
}
