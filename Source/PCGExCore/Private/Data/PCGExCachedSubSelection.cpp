// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExCachedSubSelection.h"

#include "Data/PCGExData.h"
#include "Data/Buffers/PCGExBufferProperty.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Metadata/PCGMetadataCommon.h"
#include "Types/PCGExTypeOps.h"

namespace PCGExData
{
	//
	// FCachedSubSelection
	//

	FCachedSubSelection::~FCachedSubSelection()
	{
		for (FProperty* Prop : OwnedProperties) { delete Prop; }
	}

	FCachedSubSelection::FCachedSubSelection(FCachedSubSelection&& Other) noexcept
	{
		*this = MoveTemp(Other);
	}

	FCachedSubSelection& FCachedSubSelection::operator=(FCachedSubSelection&& Other) noexcept
	{
		if (this != &Other)
		{
			for (FProperty* Prop : OwnedProperties) { delete Prop; }

			bIsValid = Other.bIsValid;
			RealType = Other.RealType;
			WorkingType = Other.WorkingType;
			FinalChainType = Other.FinalChainType;
			CompiledChain = MoveTemp(Other.CompiledChain);
			ConvertRealToWorking = Other.ConvertRealToWorking;
			ConvertWorkingToReal = Other.ConvertWorkingToReal;
			ConvertFinalToWorking = Other.ConvertFinalToWorking;
			ConvertWorkingToFinal = Other.ConvertWorkingToFinal;
			RealOps = Other.RealOps;
			WorkingOps = Other.WorkingOps;
			OwnedProperties = MoveTemp(Other.OwnedProperties);
		}
		return *this;
	}

	void FCachedSubSelection::Initialize(
		const FSubSelection& Selection,
		EPCGMetadataTypes InRealType,
		EPCGMetadataTypes InWorkingType,
		const FPCGMetadataAttributeDesc* SourceDesc)
	{
		bIsValid = Selection.HasSelection();
		RealType = InRealType;
		WorkingType = InWorkingType;

		// Start from the parser-produced chain, then run the Stage 3 compiler
		// to drop/promote steps based on RealType. The compiler also fills
		// each remaining step's typed fn pointers, so the hot path has zero
		// per-call lookups. SourceDesc threads through to container-aware
		// classifiers (Stage 5b).
		// Clean up any previously owned properties from a prior Initialize.
		for (FProperty* Prop : OwnedProperties) { delete Prop; }
		OwnedProperties.Reset();

		CompiledChain = Selection.GetChain();
		CompileChainForSource(CompiledChain, RealType, SourceDesc);

		// Stage 5c: create owned FProperty instances for container-index
		// steps that need property-aware writes (CopyCompleteValue for
		// non-trivially-copyable element types like FString, nested
		// containers, UStructs). Replay the Desc strip logic: start with
		// SourceDesc, strip the outermost ContainerType entry each time a
		// container step appears. The inner-element FProperty is created
		// from the stripped Desc.
		if (SourceDesc && !CompiledChain.Steps.IsEmpty())
		{
			const ISubAccessor* ContainerIndexAccessor = FSubAccessorRegistry::GetContainerIndexAccessor();
			FPCGMetadataAttributeDesc CurrentDesc = *SourceDesc;

			for (FSubSelectionStep& Step : CompiledChain.Steps)
			{
				if (Step.Accessor == ContainerIndexAccessor && !CurrentDesc.IsSingleValue())
				{
					// Build the inner-element property from the stripped Desc.
					FPCGMetadataAttributeDesc InnerDesc = CurrentDesc;
					InnerDesc.ContainerTypes.RemoveAt(0);
					FProperty* ElementProp = FPropertyBuffer::CreateInnerPropertyFromDesc(InnerDesc);
					if (ElementProp)
					{
						OwnedProperties.Add(ElementProp);
						Step.Parsed.ContainerElementProperty = ElementProp;
					}

					// Consume the outer container layer for subsequent steps.
					CurrentDesc = InnerDesc;
				}
				else
				{
					// Non-container step (or container-count, which doesn't
					// need a property). Clear the Desc so subsequent container
					// steps on already-unwrapped values don't get spurious
					// properties.
					CurrentDesc.ContainerTypes.Reset();
				}
			}
		}

		FinalChainType = CompiledChain.Steps.IsEmpty() ? RealType : CompiledChain.Steps.Last().OutType;

		// Type ops for copy/default (callers that rely on these).
		RealOps = PCGExTypeOps::FTypeOpsRegistry::Get(RealType);
		WorkingOps = PCGExTypeOps::FTypeOpsRegistry::Get(WorkingType);

		// Conversions.
		// ConvertReal*Working are used on the identity (no-chain) path and by
		// ConvertGet/ConvertSet. ConvertFinal*Working are used on the
		// chain-active path to bridge the chain's final OutType and the
		// blender's WorkingType.
		ConvertRealToWorking  = PCGExTypeOps::FConversionTable::GetConversionFn(RealType, WorkingType);
		ConvertWorkingToReal  = PCGExTypeOps::FConversionTable::GetConversionFn(WorkingType, RealType);
		ConvertFinalToWorking = PCGExTypeOps::FConversionTable::GetConversionFn(FinalChainType, WorkingType);
		ConvertWorkingToFinal = PCGExTypeOps::FConversionTable::GetConversionFn(WorkingType, FinalChainType);
	}

	void FCachedSubSelection::ApplyGet(const void* Source, void* OutValue) const
	{
		if (CompiledChain.Steps.IsEmpty())
		{
			// No sub-selection applies (either user asked for nothing or the
			// compiler dropped every step as nonsensical). Plain convert.
			if (ConvertRealToWorking) { ConvertRealToWorking(Source, OutValue); }
			return;
		}

		// Walk the chain with double-buffered intermediates. Step N's input
		// is either Source (step 0) or the previous step's output buffer.
		// For the last step, we write directly to OutValue only if no
		// FinalChainType -> WorkingType conversion is needed. Otherwise we
		// land the chain output in a buffer and run ConvertFinalToWorking.
		alignas(16) uint8 BufA[96];
		alignas(16) uint8 BufB[96];
		void* Bufs[2] = { BufA, BufB };

		const void* CurrentIn = Source;
		int32 BufIdx = 0;
		const int32 LastIdx = CompiledChain.Steps.Num() - 1;

		for (int32 i = 0; i < LastIdx; ++i)
		{
			const FSubSelectionStep& Step = CompiledChain.Steps[i];
			check(Step.StepGetFn);

			void* StepOut = Bufs[BufIdx];
			Step.StepGetFn(CurrentIn, StepOut, Step.Parsed);
			CurrentIn = StepOut;
			BufIdx = 1 - BufIdx;
		}

		const FSubSelectionStep& Last = CompiledChain.Steps[LastIdx];
		check(Last.StepGetFn);

		if (FinalChainType == WorkingType)
		{
			// Direct write: chain's final output is already the working type.
			Last.StepGetFn(CurrentIn, OutValue, Last.Parsed);
			return;
		}

		// Indirect: write to intermediate buffer, then convert.
		void* FinalBuf = Bufs[BufIdx];
		Last.StepGetFn(CurrentIn, FinalBuf, Last.Parsed);
		if (ConvertFinalToWorking) { ConvertFinalToWorking(FinalBuf, OutValue); }
	}

	void FCachedSubSelection::ApplySet(void* Target, const void* Source) const
	{
		if (CompiledChain.Steps.IsEmpty())
		{
			if (ConvertWorkingToReal) { ConvertWorkingToReal(Source, Target); }
			return;
		}

		const int32 LastIdx = CompiledChain.Steps.Num() - 1;

		// Any step without a SetFn (e.g., axis) disqualifies the whole chain
		// for inject. Defensive check -- AppliesToTargetWrite also guards.
		for (const FSubSelectionStep& Step : CompiledChain.Steps)
		{
			if (!Step.StepSetFn) { return; }
		}

		// Stage 3 uses a fixed-size intermediate buffer array. A compiled
		// chain should never exceed ~3 steps in practice (a user-written
		// pair can auto-promote by 1 step on Transform source). 4 slots
		// gives headroom; assertion makes any overflow loud.
		constexpr int32 MaxSteps = 4;
		checkf(CompiledChain.Steps.Num() <= MaxSteps,
			TEXT("FCachedSubSelection chain exceeded MaxSteps (%d > %d)"),
			CompiledChain.Steps.Num(), MaxSteps);

		alignas(16) uint8 Buffers[MaxSteps][96];

		// 1) Extract phase: walk forward to populate Buffers[0..LastIdx-1]
		//    with the current state at each depth. Buffers[LastIdx-1] is the
		//    parent of the last (injection) step.
		{
			const void* CurrentIn = Target;
			for (int32 i = 0; i < LastIdx; ++i)
			{
				const FSubSelectionStep& Step = CompiledChain.Steps[i];
				Step.StepGetFn(CurrentIn, Buffers[i], Step.Parsed);
				CurrentIn = Buffers[i];
			}
		}

		// 2) Convert Source (WorkingType) to FinalChainType so it matches
		//    the last step's expected NewChild type. Skip the conversion
		//    when the two types are identical.
		alignas(16) uint8 NewChildBuf[96];
		const void* NewChild = Source;
		if (WorkingType != FinalChainType)
		{
			if (ConvertWorkingToFinal) { ConvertWorkingToFinal(Source, NewChildBuf); }
			else { return; } // Can't bridge -- bail out.
			NewChild = NewChildBuf;
		}

		// 3) Inject phase: mutate the last-step's parent with the converted
		//    NewChild. Then walk backward, propagating each mutation up.
		void* LastParent = (LastIdx == 0) ? Target : Buffers[LastIdx - 1];
		const FSubSelectionStep& Last = CompiledChain.Steps[LastIdx];
		Last.StepSetFn(LastParent, NewChild, Last.Parsed);

		for (int32 i = LastIdx - 1; i >= 0; --i)
		{
			const FSubSelectionStep& Step = CompiledChain.Steps[i];
			void* Parent = (i == 0) ? Target : Buffers[i - 1];
			const void* Child = Buffers[i];
			Step.StepSetFn(Parent, Child, Step.Parsed);
		}
	}
}
