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

	// Forward decls.
	class ISubAccessor;
	using FStepGetFn = void (*)(const void* Parent, void* ChildOut, const FAccessorParseResult& Parsed);
	using FStepSetFn = void (*)(void* ParentInOut, const void* NewChild, const FAccessorParseResult& Parsed);

	/**
	 * FSubSelectionStep
	 *
	 * One step in a parsed chain: the accessor that matched, its parsed
	 * payload, and the input/output types for this step in the pipeline.
	 *
	 * After CompileChainForSource, InType/OutType are resolved and
	 * StepGetFn/StepSetFn carry direct fn pointers so the hot path can
	 * invoke each step without vtable dispatch on the accessor.
	 */
	struct PCGEXCORE_API FSubSelectionStep
	{
		const ISubAccessor* Accessor = nullptr;
		FAccessorParseResult Parsed;
		EPCGMetadataTypes InType = EPCGMetadataTypes::Unknown;
		EPCGMetadataTypes OutType = EPCGMetadataTypes::Unknown;

		// Populated by CompileChainForSource. Hot path calls these directly.
		FStepGetFn StepGetFn = nullptr;
		FStepSetFn StepSetFn = nullptr;
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
	 * Stage 3 hot-path fn-pointer signatures.
	 *
	 * FStepGetFn reads Parent (at this step's InType) and writes the
	 * extracted Child (at this step's OutType) to ChildOut.
	 *
	 * FStepSetFn takes a ParentInOut (at this step's InType) and a NewChild
	 * (at this step's OutType) and modifies Parent so that applying the
	 * extract again would produce NewChild. Read-only accessors (Axis)
	 * return nullptr from GetStepSetFn.
	 */
	using FStepGetFn = void (*)(const void* Parent, void* ChildOut, const FAccessorParseResult& Parsed);
	using FStepSetFn = void (*)(void* ParentInOut, const void* NewChild, const FAccessorParseResult& Parsed);

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
	 * path. Stage 2 consolidates flag-driven dispatch at the FSubSelection
	 * level. Stage 3 adds typed fn-pointer getters (GetStepGetFn /
	 * GetStepSetFn) so FCachedSubSelection can cache per-step direct
	 * function calls without vtable dispatch.
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

		//
		// Stage 3 compiled-chain hot path
		//

		/**
		 * Return a direct fn pointer for the extract direction at this
		 * step's InType. Called once per step during FCachedSubSelection
		 * Initialize; the returned pointer is cached in the compiled step
		 * and called without vtable dispatch at hot-path time.
		 *
		 * Returns nullptr if this accessor cannot operate on InType (the
		 * chain compiler drops such steps before they reach the hot path).
		 */
		virtual FStepGetFn GetStepGetFn(EPCGMetadataTypes InType) const = 0;

		/**
		 * Inject counterpart to GetStepGetFn. Returns nullptr for read-only
		 * accessors (Axis) and for types where inject is not meaningful.
		 */
		virtual FStepSetFn GetStepSetFn(EPCGMetadataTypes InType) const { (void)InType; return nullptr; }

		/**
		 * Chain compilation classifier for the parser-time chain vs the
		 * compile-time chain. Three outcomes:
		 *   - Keep: the step applies to SourceType as-is.
		 *   - Drop: the step is nonsensical for SourceType; the compiler
		 *     removes it (chain flows on to the next step at SourceType).
		 *   - PromoteToTransformPart_{Position,Rotation}: user shortcut --
		 *     auto-insert a TransformPart step before this one. Used when
		 *     SourceType is Transform and the user asked for axis/field
		 *     without explicitly picking a component first.
		 */
		enum class ECompileAction : uint8
		{
			Keep,
			Drop,
			PromoteWithPosition,
			PromoteWithRotation,
		};

		virtual ECompileAction ClassifyForInType(EPCGMetadataTypes InType,
		                                         const FAccessorParseResult& Parsed) const = 0;
	};

	/**
	 * FSubAccessorRegistry
	 *
	 * Process-lifetime owner of accessor instances + entry point for
	 * parsing ExtraNames into a chain.
	 *
	 * Registration order is priority order: when multiple accessors could
	 * match a token, the earlier-registered one wins. Stage 1 order is
	 * Axis -> TransformPart -> SingleField.
	 */
	class PCGEXCORE_API FSubAccessorRegistry
	{
	public:
		/** Idempotent. Builds the accessor list on first call. */
		static void Initialize();

		/** Registration-order view of all known accessors. */
		static TConstArrayView<const ISubAccessor*> GetAll();

		/** Typed accessor getters. Stable for the process lifetime after Initialize(). */
		static const ISubAccessor* GetAxisAccessor();
		static const ISubAccessor* GetTransformPartAccessor();
		static const ISubAccessor* GetSingleFieldAccessor();

		/**
		 * Parse a list of extra-name tokens into a chain. Stage 2: true
		 * left-to-right walk; each token tries every accessor in
		 * registration order, first match wins. Chain order mirrors
		 * token order.
		 *
		 * @param ExtraNames     Tokens from FPCGAttributePropertyInputSelector::GetExtraNames().
		 * @param SourceTypeHint Optional hint about the source attribute's type.
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

	/**
	 * Number of distinct field positions a source type exposes.
	 * Used by callers that split an attribute into per-field sub-pieces
	 * (attribute remapper, noise generator, proxy-data helper).
	 *
	 *   Vector2            -> 2
	 *   Vector / Rotator   -> 3
	 *   Vector4 / Quat     -> 4
	 *   Transform          -> 9  (3 pos + 3 rot + 3 scale)
	 *   everything else    -> 1
	 */
	PCGEXCORE_API int32 GetNumFieldsForType(EPCGMetadataTypes Type);

	/**
	 * Compile a parsed chain for a concrete source type.
	 *
	 * Walks the chain left-to-right. For each step, classifies against the
	 * current value type (starting from SourceType; each kept step updates
	 * the current type to its OutType):
	 *   - Keep:   step is compatible; appended to the compiled chain as-is.
	 *   - Drop:   step is nonsensical for the current type; removed.
	 *   - Promote: insert an implicit TransformPart step before this one,
	 *              then retry (e.g., `.Forward` on a Transform source gets
	 *              promoted to `.Rotation.Forward`; `.X` on a Transform
	 *              source gets promoted to `.Position.X`).
	 *
	 * Also fills in each step's InType, OutType, StepGetFn, StepSetFn so
	 * the compiled chain is ready for direct invocation without further
	 * accessor lookups.
	 */
	PCGEXCORE_API void CompileChainForSource(FSubSelectionChain& InOutChain,
	                                         EPCGMetadataTypes SourceType);
}
