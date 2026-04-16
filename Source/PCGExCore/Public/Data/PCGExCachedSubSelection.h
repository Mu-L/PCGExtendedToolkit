// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExSubAccessor.h"
#include "PCGExSubSelection.h"
#include "Types/PCGExTypeOps.h"

struct FPCGMetadataAttributeDesc;

/**
 * PCGEx Cached Sub-Selection Operations
 *
 * FCachedSubSelection caches a compiled sub-accessor chain plus the
 * conversion fn pointers needed to move values between the attribute's
 * real type, the chain's final type, and the blender's working type.
 *
 * Stage 3 rewrote this struct around chain walking: Initialize runs
 * CompileChainForSource against the parsed chain to drop/promote steps
 * based on the real type, resolves per-step fn pointers once, and the
 * hot path ApplyGet/ApplySet just walk the compiled steps without any
 * flag-driven branching or vtable dispatch on accessors.
 *
 * Usage:
 *   // At proxy construction:
 *   CachedSubSelection.Initialize(SubSelection, RealType, WorkingType);
 *
 *   // At runtime (hot path):
 *   CachedSubSelection.ApplyGet(Source, OutValue);  // No lookups!
 *   CachedSubSelection.ApplySet(Target, Source);    // No lookups!
 */

namespace PCGExData
{
	/**
	 * Legacy fn-pointer typedefs retained for any external helper that
	 * still references them. FCachedSubSelection no longer uses these
	 * internally.
	 */
	using FExtractFieldFn = double (*)(const void* Value, PCGExTypeOps::ESingleField Field);
	using FInjectFieldFn = void (*)(void* Target, double Value, PCGExTypeOps::ESingleField Field);
	using FExtractAxisFn = FVector (*)(const void* Value, EPCGExAxis Axis);

	namespace SubSelectionImpl
	{
		static FORCEINLINE FVector ExtractAxisDefault(const void* Value, EPCGExAxis Axis) { return FVector::ForwardVector; }

		PCGEXCORE_API FExtractFieldFn GetExtractFieldFn(EPCGMetadataTypes Type);
		PCGEXCORE_API FInjectFieldFn GetInjectFieldFn(EPCGMetadataTypes Type);
		PCGEXCORE_API FExtractAxisFn GetExtractAxisFn(EPCGMetadataTypes Type);
	}

	/**
	 * FCachedSubSelection -- pre-compiled chain + type conversions.
	 *
	 * Memory layout (approx): one FSubSelectionChain (48 bytes + inline
	 * storage for up to 2 steps, each ~56 bytes) + ~8 fn pointers + a few
	 * enums. Stays under ~300 bytes for typical 1-2-step chains.
	 *
	 * Max supported chain length is 4 steps (auto-promotion can add one
	 * step ahead of a user-written pair, for a total of 3 in practice;
	 * the extra slot is a safety margin). Asserted at hot-path time.
	 */
	struct PCGEXCORE_API FCachedSubSelection
	{
		// Set to Selection.bIsValid at Initialize time. External callers
		// still read this to decide whether sub-selection is active at
		// all. True iff the pre-compile chain had any steps.
		bool bIsValid = false;

		// Type info
		EPCGMetadataTypes RealType = EPCGMetadataTypes::Unknown;
		EPCGMetadataTypes WorkingType = EPCGMetadataTypes::Unknown;
		EPCGMetadataTypes FinalChainType = EPCGMetadataTypes::Unknown;

		// Compiled chain. Empty after compilation means no sub-selection
		// applies (either user-empty, or every step got dropped as
		// nonsensical for RealType). Hot path short-circuits to plain
		// Real<->Working conversion.
		FSubSelectionChain CompiledChain;

		// Identity conversions (used when CompiledChain is empty, and by
		// ConvertGet/ConvertSet).
		PCGExTypeOps::FConvertFn ConvertRealToWorking = nullptr;
		PCGExTypeOps::FConvertFn ConvertWorkingToReal = nullptr;

		// Post-chain / pre-chain conversions (used when CompiledChain is
		// non-empty). FinalChainType <-> WorkingType.
		PCGExTypeOps::FConvertFn ConvertFinalToWorking = nullptr;
		PCGExTypeOps::FConvertFn ConvertWorkingToFinal = nullptr;

		// Type ops kept for callers that already used these for default/copy
		// semantics via FCachedSubSelection (e.g., IBufferProxy).
		const PCGExTypeOps::ITypeOpsBase* RealOps = nullptr;
		const PCGExTypeOps::ITypeOpsBase* WorkingOps = nullptr;

		// Stage 5c: compile-time-created FProperty instances for container
		// steps that need property-aware reads/writes (e.g., CopyCompleteValue
		// for non-trivially-copyable element types). Each compiled container
		// step stores a non-owning pointer in Parsed.ContainerElementProperty;
		// this array owns the actual FProperty lifetime. Cleaned up in the
		// destructor.
		TArray<FProperty*> OwnedProperties;

		FCachedSubSelection() = default;
		~FCachedSubSelection();

		// Non-copyable (owns FProperty pointers). Movable.
		FCachedSubSelection(const FCachedSubSelection&) = delete;
		FCachedSubSelection& operator=(const FCachedSubSelection&) = delete;
		FCachedSubSelection(FCachedSubSelection&& Other) noexcept;
		FCachedSubSelection& operator=(FCachedSubSelection&& Other) noexcept;

		/**
		 * Initialize from a parsed FSubSelection + Real/Working types.
		 * Runs CompileChainForSource against RealType, caches all per-step
		 * fn pointers, resolves conversions. After this call the hot path
		 * uses only cached state.
		 *
		 * SourceDesc (Stage 5b): optional Desc view of the source attribute.
		 * Only consulted when the chain contains container-aware steps
		 * (FContainerIndex / FContainerCount). Scalar sources pass nullptr.
		 */
		void Initialize(const FSubSelection& Selection,
		                EPCGMetadataTypes InRealType,
		                EPCGMetadataTypes InWorkingType,
		                const FPCGMetadataAttributeDesc* SourceDesc = nullptr);

		/**
		 * True iff the compiled chain has at least one step (i.e., the
		 * sub-selection actually changes the output).
		 */
		FORCEINLINE bool AppliesToSourceRead() const { return !CompiledChain.Steps.IsEmpty(); }
		FORCEINLINE bool AppliesToTargetWrite() const
		{
			// Inject needs every step to have a writable SetFn. Axis-only
			// chains (read-only) can't drive inject.
			if (CompiledChain.Steps.IsEmpty()) { return false; }
			for (const FSubSelectionStep& Step : CompiledChain.Steps)
			{
				if (!Step.StepSetFn) { return false; }
			}
			return true;
		}

		/**
		 * Hot path extract. Reads Source (RealType), walks the compiled
		 * chain, converts the chain's final output to WorkingType into
		 * OutValue.
		 */
		void ApplyGet(const void* Source, void* OutValue) const;

		/**
		 * Hot path inject. Reads Source (WorkingType), walks the chain in
		 * extract-modify-inject order to mutate Target (RealType).
		 */
		void ApplySet(void* Target, const void* Source) const;

		/** No sub-selection: just convert RealType -> WorkingType. */
		FORCEINLINE void ConvertGet(const void* Source, void* OutValue) const
		{
			if (ConvertRealToWorking) { ConvertRealToWorking(Source, OutValue); }
		}

		/** No sub-selection: just convert WorkingType -> RealType. */
		FORCEINLINE void ConvertSet(void* Target, const void* Source) const
		{
			if (ConvertWorkingToReal) { ConvertWorkingToReal(Source, Target); }
		}
	};
}
