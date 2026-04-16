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
		// Indices into OrderedView for the Stage 1 parity accessors. Set by
		// Initialize(). Used by ParseChain to invoke the legacy positional rule.
		int32 GAxisIndex = INDEX_NONE;
		int32 GTransformIndex = INDEX_NONE;
		int32 GFieldIndex = INDEX_NONE;
	}

	void FSubAccessorRegistry::Initialize()
	{
		if (bInitialized) { return; }

		// Registration order = priority order. Matches the legacy Init's
		// implicit precedence (axis check first, component second, field third).
		OwnedAccessors.Add(MakeUnique<FAxisAccessor>());
		GAxisIndex = OrderedView.Add(OwnedAccessors.Last().Get());

		OwnedAccessors.Add(MakeUnique<FTransformPartAccessor>());
		GTransformIndex = OrderedView.Add(OwnedAccessors.Last().Get());

		OwnedAccessors.Add(MakeUnique<FSingleFieldAccessor>());
		GFieldIndex = OrderedView.Add(OwnedAccessors.Last().Get());

		bInitialized = true;
	}

	TConstArrayView<const ISubAccessor*> FSubAccessorRegistry::GetAll()
	{
		if (!bInitialized) { Initialize(); }
		return OrderedView;
	}

	bool FSubAccessorRegistry::ParseChain(const TArray<FString>& ExtraNames,
	                                      EPCGMetadataTypes SourceTypeHint,
	                                      FSubSelectionChain& OutChain)
	{
		if (!bInitialized) { Initialize(); }

		OutChain.Reset();
		OutChain.SourceTypeHint = SourceTypeHint;

		if (ExtraNames.IsEmpty()) { return false; }

		// Stage 1: positional-emulation parser. Mirrors the legacy Init exactly:
		//   1. Walk all tokens for an axis match.
		//   2. Walk all tokens for a component match.
		//   3. Take Names[1] (or Names[0] if Num()==1) as the field token.
		//
		// Stage 2 will rewrite this as a true left-to-right walk; for parity
		// the positional rule is what FSubSelection::Init projects to its
		// legacy flags, so the chain must reflect the same matching outcome.

		const ISubAccessor* AxisAccessor = OrderedView[GAxisIndex];
		const ISubAccessor* TransformAccessor = OrderedView[GTransformIndex];
		const ISubAccessor* FieldAccessor = OrderedView[GFieldIndex];

		// Pre-uppercase tokens once.
		TArray<FString, TInlineAllocator<4>> Upper;
		Upper.Reserve(ExtraNames.Num());
		for (const FString& T : ExtraNames) { Upper.Add(T.ToUpper()); }

		// Step A: axis (walk all).
		for (const FString& Tok : Upper)
		{
			FAccessorParseResult Parsed;
			if (AxisAccessor->MatchesToken(Tok, Parsed))
			{
				FSubSelectionStep Step;
				Step.Accessor = AxisAccessor;
				Step.Parsed = Parsed;
				Step.InType = SourceTypeHint;
				AxisAccessor->ResolveOutputType(SourceTypeHint, Parsed, Step.OutType);
				OutChain.Steps.Add(Step);
				break;
			}
		}

		// Step B: component (walk all).
		for (const FString& Tok : Upper)
		{
			FAccessorParseResult Parsed;
			if (TransformAccessor->MatchesToken(Tok, Parsed))
			{
				FSubSelectionStep Step;
				Step.Accessor = TransformAccessor;
				Step.Parsed = Parsed;
				Step.InType = SourceTypeHint;
				TransformAccessor->ResolveOutputType(SourceTypeHint, Parsed, Step.OutType);
				OutChain.Steps.Add(Step);
				break;
			}
		}

		// Step C: field at the legacy positional slot (Names[1] or Names[0]).
		{
			const FString& FieldToken = Upper.Num() > 1 ? Upper[1] : Upper[0];
			FAccessorParseResult Parsed;
			if (FieldAccessor->MatchesToken(FieldToken, Parsed))
			{
				FSubSelectionStep Step;
				Step.Accessor = FieldAccessor;
				Step.Parsed = Parsed;
				Step.InType = SourceTypeHint;
				FieldAccessor->ResolveOutputType(SourceTypeHint, Parsed, Step.OutType);
				OutChain.Steps.Add(Step);
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
