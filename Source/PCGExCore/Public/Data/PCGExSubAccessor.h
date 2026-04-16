// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Math/PCGExMathAxis.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "Types/PCGExTypeOps.h"

/**
 * Phase 6 Stage 1 -- Sub-accessor abstraction (scaffolding).
 *
 * Pluggable, chainable replacement for the closed STRMAP/per-type-dispatch
 * sub-selection system. In Stage 1 this layer co-exists with the legacy
 * FSubSelection::Init STRMAP path; in Stage 2 the per-type dispatch is
 * collapsed into accessor implementations; Stage 3 wires the chain into
 * FCachedSubSelection's hot path; Stage 4 adds new accessor classes
 * (swizzles, container index, etc.) without touching downstream layers.
 *
 * Contract for Stage 1:
 *   - The types defined here are NOT YET wired into FSubSelection. Adding
 *     this header to a TU does not change runtime behavior.
 *   - FSubAccessorRegistry::ParseChain is a stub in Stage 1 (returns false,
 *     produces an empty chain). Step 3 of Stage 1 fills in the three
 *     parity accessors and registers them.
 */

namespace PCGExData
{
	/**
	 * FAccessorParseResult
	 *
	 * Per-step parsed payload. POD, additive layout: each accessor reads
	 * only the fields it owns. Stage 4+ extends this struct with new fields
	 * (container index, swizzle mask, struct-field name, ...) without
	 * breaking ABI.
	 */
	struct PCGEXCORE_API FAccessorParseResult
	{
		// FSingleFieldAccessor writes Field + FieldIndex.
		PCGExTypeOps::ESingleField Field = PCGExTypeOps::ESingleField::X;
		int32 FieldIndex = 0; // 0..3 for X/Y/Z/W; can be -1 for derived (Length/Sum/...)

		// FAxisAccessor writes Axis.
		EPCGExAxis Axis = EPCGExAxis::Forward;

		// FTransformPartAccessor writes Component.
		PCGExTypeOps::ETransformPart Component = PCGExTypeOps::ETransformPart::Position;

		// Hint type the matched token implies for the source attribute, e.g. "X" -> Vector,
		// "W" -> Vector4, "Roll" -> Quaternion. Used for legacy PossibleSourceType
		// projection. Unknown when the accessor doesn't supply a hint.
		EPCGMetadataTypes SourceTypeHint = EPCGMetadataTypes::Unknown;
	};

	// Forward decl for FSubSelectionStep.
	class ISubAccessor;

	/**
	 * FSubSelectionStep
	 *
	 * One step in a parsed chain: the accessor that matched, its parsed
	 * payload, and the input/output types for this step in the pipeline.
	 *
	 * In Stage 1, InType/OutType may stay Unknown -- the chain is built
	 * positionally and these fields are populated later in Stage 2 when the
	 * chain becomes the hot-path source of truth.
	 */
	struct PCGEXCORE_API FSubSelectionStep
	{
		const ISubAccessor* Accessor = nullptr;
		FAccessorParseResult Parsed;
		EPCGMetadataTypes InType = EPCGMetadataTypes::Unknown;
		EPCGMetadataTypes OutType = EPCGMetadataTypes::Unknown;
	};

	/**
	 * FSubSelectionChain
	 *
	 * Ordered sequence of accessor steps that derives a sub-selected value
	 * from a source. An empty chain is the identity. Most chains are 1-2
	 * steps (Position.X, Rotation.Forward); the inline allocator avoids
	 * heap traffic for the common case.
	 */
	struct PCGEXCORE_API FSubSelectionChain
	{
		TArray<FSubSelectionStep, TInlineAllocator<2>> Steps;
		EPCGMetadataTypes FinalType = EPCGMetadataTypes::Unknown;
		EPCGMetadataTypes SourceTypeHint = EPCGMetadataTypes::Unknown;
		bool bIsValid = false;

		FORCEINLINE bool IsEmpty() const { return Steps.Num() == 0; }
		void Reset();
	};

	/**
	 * ISubAccessor
	 *
	 * A pluggable accessor that knows how to match tokens, resolve the
	 * resulting type for a given source, and extract/inject values.
	 *
	 * Stateless by convention. Owned for-process by FSubAccessorRegistry.
	 *
	 * Stage 1 implementations (FSingleFieldAccessor, FAxisAccessor,
	 * FTransformPartAccessor) wrap the existing FTypeOps<T> primitives so
	 * the new code path produces identical results to the legacy STRMAP
	 * path. Stage 2 absorbs the per-type switches in-house.
	 */
	class PCGEXCORE_API ISubAccessor
	{
	public:
		virtual ~ISubAccessor() = default;

		/**
		 * Token matching. The registry calls this once per token in
		 * ExtraNames. The accessor decides whether the token is one of
		 * "its" tokens and, if so, populates OutParsed with whatever
		 * per-step state it needs.
		 *
		 * @param UpperToken The token uppercased (callers normalize once).
		 * @param OutParsed  Populated on a successful match.
		 * @return true if this accessor matches.
		 */
		virtual bool MatchesToken(const FString& UpperToken, FAccessorParseResult& OutParsed) const = 0;

		/**
		 * Compute the output type when this accessor's step is applied to
		 * a source of InType with the given parsed payload. Returns false
		 * if the accessor cannot apply to this source (e.g. axis on FString).
		 */
		virtual bool ResolveOutputType(EPCGMetadataTypes InType,
		                               const FAccessorParseResult& Parsed,
		                               EPCGMetadataTypes& OutType) const = 0;

		/**
		 * Hot path: extract the sub-selected value from Source (typed as
		 * InType) into OutValue (typed as OutType). Caller guarantees
		 * OutValue is large enough for OutType.
		 */
		virtual void ApplyGet(EPCGMetadataTypes InType,
		                      const void* Source,
		                      EPCGMetadataTypes OutType,
		                      void* OutValue,
		                      const FAccessorParseResult& Parsed) const = 0;

		/**
		 * Optional inject path. Default no-op for read-only accessors.
		 * Implementations that own a writable slot (field, component) may
		 * override.
		 */
		virtual void ApplySet(EPCGMetadataTypes InType,
		                      void* TargetInOut,
		                      EPCGMetadataTypes SourceType,
		                      const void* Source,
		                      const FAccessorParseResult& Parsed) const
		{
			(void)InType;
			(void)TargetInOut;
			(void)SourceType;
			(void)Source;
			(void)Parsed;
		}

		/**
		 * Diagnostic name. Used for chain-structure tests and error logs.
		 * Should be a short stable identifier, not a localized string.
		 */
		virtual FString GetDisplayName() const = 0;
	};

	/**
	 * FSubAccessorRegistry
	 *
	 * Process-lifetime owner of accessor instances + entry point for
	 * parsing ExtraNames into a chain. Mirrors the lazy-init pattern of
	 * FSubSelectorRegistry (PCGExSubSelectionOps.h).
	 *
	 * Registration order is priority order: when multiple accessors could
	 * match a token, the earlier-registered one wins. Stage 1 order is
	 * Axis -> TransformPart -> SingleField, matching the legacy Init's
	 * implicit precedence (axis check first, component second, field third).
	 */
	class PCGEXCORE_API FSubAccessorRegistry
	{
	public:
		/** Idempotent. Builds the accessor list on first call. */
		static void Initialize();

		/** Registration-order view of all known accessors. */
		static TConstArrayView<const ISubAccessor*> GetAll();

		/**
		 * Parse a list of extra-name tokens into a chain.
		 *
		 * Stage 1: STUB. Returns false and produces an empty chain. Step 3
		 * of Stage 1 fills this in using the registered accessors and the
		 * legacy positional rule (axis/component walk all names; field
		 * looks at Names[1] when Names.Num()>1, else Names[0]).
		 *
		 * @param ExtraNames     Tokens from FPCGAttributePropertyInputSelector::GetExtraNames().
		 * @param SourceTypeHint Optional hint about the source attribute's type.
		 *                       Unknown when not yet known.
		 * @param OutChain       Populated on a successful parse; reset on failure.
		 * @return true if any step was produced.
		 */
		static bool ParseChain(const TArray<FString>& ExtraNames,
		                       EPCGMetadataTypes SourceTypeHint,
		                       FSubSelectionChain& OutChain);

	private:
		static TArray<TUniquePtr<ISubAccessor>> OwnedAccessors;
		static TArray<const ISubAccessor*> OrderedView;
		static bool bInitialized;
	};
}
