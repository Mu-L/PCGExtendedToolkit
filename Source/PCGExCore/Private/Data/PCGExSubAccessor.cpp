// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExSubAccessor.h"

#include "Data/Accessors/PCGExAxisAccessor.h"
#include "Data/Accessors/PCGExSingleFieldAccessor.h"
#include "Data/Accessors/PCGExSwizzleAccessor.h"
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

		// Stage 4: swizzle accessor registered last. Its MatchesToken only
		// accepts 2-4 char combos of {x,y,z,w}, so it never competes with
		// SingleField's single-letter entries or the longer word-like axis
		// and component aliases. Registration order ensures SingleField
		// gets first shot at "X"/"Y"/etc.; Swizzle handles "xy", "xyz", ...
		OwnedAccessors.Add(MakeUnique<FSwizzleAccessor>());
		OrderedView.Add(OwnedAccessors.Last().Get());

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

	//
	// Stage 3 chain compilation
	//

	namespace
	{
		// Build a synthesized TransformPart step for auto-promotion. The returned
		// step is compiled (InType/OutType/StepGetFn/StepSetFn populated) and
		// ready to splice into the chain ahead of a step that needed promotion.
		FSubSelectionStep MakeTransformPartStep(PCGExTypeOps::ETransformPart Part)
		{
			const ISubAccessor* Accessor = FSubAccessorRegistry::GetTransformPartAccessor();

			FSubSelectionStep Step;
			Step.Accessor = Accessor;
			Step.Parsed.Component = Part;
			// Hint mirrors the legacy STRMAP_TRANSFORM_FIELD values.
			Step.Parsed.SourceTypeHint = (Part == PCGExTypeOps::ETransformPart::Rotation)
				? EPCGMetadataTypes::Quaternion
				: EPCGMetadataTypes::Vector;
			Step.InType = EPCGMetadataTypes::Transform;
			Accessor->ResolveOutputType(Step.InType, Step.Parsed, Step.OutType);
			Step.StepGetFn = Accessor->GetStepGetFn(Step.InType);
			Step.StepSetFn = Accessor->GetStepSetFn(Step.InType);
			return Step;
		}

		// Finalize a step: resolve OutType and fill in hot-path fn pointers.
		void FinalizeCompiledStep(FSubSelectionStep& Step, EPCGMetadataTypes InType)
		{
			Step.InType = InType;
			Step.Accessor->ResolveOutputType(InType, Step.Parsed, Step.OutType);
			Step.StepGetFn = Step.Accessor->GetStepGetFn(InType);
			Step.StepSetFn = Step.Accessor->GetStepSetFn(InType);
		}
	}

	void CompileChainForSource(FSubSelectionChain& InOutChain, EPCGMetadataTypes SourceType)
	{
		// Take ownership of the parser-produced step list, rebuild in-place.
		TArray<FSubSelectionStep, TInlineAllocator<2>> Original = MoveTemp(InOutChain.Steps);
		InOutChain.Steps.Reset();

		EPCGMetadataTypes CurrentType = SourceType;

		for (FSubSelectionStep& Step : Original)
		{
			// Re-check classification at each step because auto-promoted
			// TransformPart inserts change the current type.
			for (;;)
			{
				const ISubAccessor::ECompileAction Action =
					Step.Accessor->ClassifyForInType(CurrentType, Step.Parsed);

				if (Action == ISubAccessor::ECompileAction::Keep)
				{
					FinalizeCompiledStep(Step, CurrentType);
					CurrentType = Step.OutType;
					InOutChain.Steps.Add(MoveTemp(Step));
					break;
				}

				if (Action == ISubAccessor::ECompileAction::Drop)
				{
					// Step is nonsensical for the current value type; chain
					// flows past it at the same type.
					break;
				}

				// Promote: insert TransformPart(Position|Rotation) before retrying.
				const PCGExTypeOps::ETransformPart Part =
					(Action == ISubAccessor::ECompileAction::PromoteWithRotation)
					? PCGExTypeOps::ETransformPart::Rotation
					: PCGExTypeOps::ETransformPart::Position;

				FSubSelectionStep Inserted = MakeTransformPartStep(Part);
				CurrentType = Inserted.OutType;
				InOutChain.Steps.Add(MoveTemp(Inserted));
				// Loop back to re-classify the same Step under the new CurrentType.
			}
		}

		InOutChain.SourceTypeHint = SourceType;
		InOutChain.bIsValid = !InOutChain.Steps.IsEmpty();
		InOutChain.FinalType = InOutChain.bIsValid ? InOutChain.Steps.Last().OutType : SourceType;
	}
}
