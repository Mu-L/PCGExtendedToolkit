// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExSubAccessor.h"

#include "Data/Accessors/PCGExAxisAccessor.h"
#include "Data/Accessors/PCGExSingleFieldAccessor.h"
#include "Data/Accessors/PCGExTransformPartAccessor.h"

namespace PCGExData
{
	//
	// FSubSelectionChain
	//

	void FSubSelectionChain::Reset()
	{
		Steps.Reset();
		FinalType = EPCGMetadataTypes::Unknown;
		SourceTypeHint = EPCGMetadataTypes::Unknown;
		bIsValid = false;
	}

	//
	// FSubAccessorRegistry
	//

	TArray<TUniquePtr<ISubAccessor>> FSubAccessorRegistry::OwnedAccessors;
	TArray<const ISubAccessor*> FSubAccessorRegistry::OrderedView;
	bool FSubAccessorRegistry::bInitialized = false;

	namespace
	{
		// Cached pointers for typed getters. Set by Initialize().
		const ISubAccessor* GAxisAccessor = nullptr;
		const ISubAccessor* GTransformPartAccessor = nullptr;
		const ISubAccessor* GSingleFieldAccessor = nullptr;
	}

	void FSubAccessorRegistry::Initialize()
	{
		if (bInitialized) { return; }

		// Registration order = priority order: when a token could match
		// multiple accessors, the earlier-registered one wins. Stage 1's
		// order Axis -> TransformPart -> SingleField is preserved here
		// because the legacy parser had the same implicit precedence and
		// no token in the Stage 1 lookup tables overlaps anyway.
		OwnedAccessors.Add(MakeUnique<FAxisAccessor>());
		GAxisAccessor = OwnedAccessors.Last().Get();
		OrderedView.Add(GAxisAccessor);

		OwnedAccessors.Add(MakeUnique<FTransformPartAccessor>());
		GTransformPartAccessor = OwnedAccessors.Last().Get();
		OrderedView.Add(GTransformPartAccessor);

		OwnedAccessors.Add(MakeUnique<FSingleFieldAccessor>());
		GSingleFieldAccessor = OwnedAccessors.Last().Get();
		OrderedView.Add(GSingleFieldAccessor);

		bInitialized = true;
	}

	TConstArrayView<const ISubAccessor*> FSubAccessorRegistry::GetAll()
	{
		if (!bInitialized) { Initialize(); }
		return OrderedView;
	}

	const ISubAccessor* FSubAccessorRegistry::GetAxisAccessor()
	{
		if (!bInitialized) { Initialize(); }
		return GAxisAccessor;
	}

	const ISubAccessor* FSubAccessorRegistry::GetTransformPartAccessor()
	{
		if (!bInitialized) { Initialize(); }
		return GTransformPartAccessor;
	}

	const ISubAccessor* FSubAccessorRegistry::GetSingleFieldAccessor()
	{
		if (!bInitialized) { Initialize(); }
		return GSingleFieldAccessor;
	}

	int32 GetNumFieldsForType(EPCGMetadataTypes Type)
	{
		switch (Type)
		{
		case EPCGMetadataTypes::Vector2:    return 2;
		case EPCGMetadataTypes::Vector:
		case EPCGMetadataTypes::Rotator:    return 3;
		case EPCGMetadataTypes::Vector4:
		case EPCGMetadataTypes::Quaternion: return 4;
		case EPCGMetadataTypes::Transform:  return 9;
		default:                            return 1;
		}
	}

	bool FSubAccessorRegistry::ParseChain(const TArray<FString>& ExtraNames,
	                                      EPCGMetadataTypes SourceTypeHint,
	                                      FSubSelectionChain& OutChain)
	{
		if (!bInitialized) { Initialize(); }

		OutChain.Reset();
		OutChain.SourceTypeHint = SourceTypeHint;

		if (ExtraNames.IsEmpty()) { return false; }

		// Stage 2: true left-to-right walk. For each token in input order,
		// try each accessor in registration order; first match wins. Steps
		// are appended to the chain as they're matched, so chain order
		// mirrors token order. Each step's InType chains from the previous
		// step's OutType (greedy resolution).
		//
		// This drops Stage 1's positional emulation (axis-walk-all,
		// component-walk-all, field-at-Names[1]). The behavior change is
		// silent for well-formed inputs (the existing parity tests still
		// pass) and only affects malformed inputs like {Roll, Garbage}
		// where the legacy parser refused to recognize a leading field
		// token. PossibleSourceType has zero external readers so the
		// projection divergence is also silent at the consumer surface.
		EPCGMetadataTypes CurrentType = SourceTypeHint;

		for (const FString& Token : ExtraNames)
		{
			const FString Upper = Token.ToUpper();
			for (const ISubAccessor* Accessor : OrderedView)
			{
				FAccessorParseResult Parsed;
				if (!Accessor->MatchesToken(Upper, Parsed)) { continue; }

				FSubSelectionStep Step;
				Step.Accessor = Accessor;
				Step.Parsed = Parsed;
				Step.InType = CurrentType;
				Accessor->ResolveOutputType(CurrentType, Parsed, Step.OutType);
				OutChain.Steps.Add(Step);

				CurrentType = Step.OutType;
				break;
			}
		}

		OutChain.bIsValid = !OutChain.Steps.IsEmpty();
		if (OutChain.bIsValid)
		{
			OutChain.FinalType = OutChain.Steps.Last().OutType;
		}
		return OutChain.bIsValid;
	}
}
