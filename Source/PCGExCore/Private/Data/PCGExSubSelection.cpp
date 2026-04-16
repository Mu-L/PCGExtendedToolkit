// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExSubSelection.h"

#include "CoreMinimal.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Types/PCGExTypeOpsImpl.h"

namespace PCGExData
{
	//
	// FSubSelection constructors
	//

	FSubSelection::FSubSelection(const TArray<FString>& ExtraNames)
	{
		Init(ExtraNames);
	}

	FSubSelection::FSubSelection(const FPCGAttributePropertyInputSelector& InSelector)
	{
		Init(InSelector.GetExtraNames());
	}

	FSubSelection::FSubSelection(const FString& Path, const UPCGData* InData)
	{
		FPCGAttributePropertyInputSelector ProxySelector = FPCGAttributePropertyInputSelector();
		ProxySelector.Update(Path);
		if (InData) { ProxySelector = ProxySelector.CopyAndFixLast(InData); }
		Init(ProxySelector.GetExtraNames());
	}

	//
	// Classifier methods (Stage 5 -- chain-backed)
	//

	bool FSubSelection::HasSelection() const
	{
		return !ParsedChain.Steps.IsEmpty();
	}

	bool FSubSelection::IsFieldSelection() const
	{
		const ISubAccessor* Target = FSubAccessorRegistry::GetSingleFieldAccessor();
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == Target) { return true; }
		}
		return false;
	}

	bool FSubSelection::IsAxisSelection() const
	{
		const ISubAccessor* Target = FSubAccessorRegistry::GetAxisAccessor();
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == Target) { return true; }
		}
		return false;
	}

	bool FSubSelection::IsComponentSelection() const
	{
		const ISubAccessor* Target = FSubAccessorRegistry::GetTransformPartAccessor();
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == Target) { return true; }
		}
		return false;
	}

	bool FSubSelection::IsContainerIndexSelection() const
	{
		const ISubAccessor* Target = FSubAccessorRegistry::GetContainerIndexAccessor();
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == Target) { return true; }
		}
		return false;
	}

	bool FSubSelection::IsContainerCountSelection() const
	{
		const ISubAccessor* Target = FSubAccessorRegistry::GetContainerCountAccessor();
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == Target) { return true; }
		}
		return false;
	}

	EPCGMetadataTypes FSubSelection::GetSubType(const EPCGMetadataTypes Fallback) const
	{
		if (!HasSelection()) { return Fallback; }
		if (IsFieldSelection()) { return EPCGMetadataTypes::Double; }
		if (IsAxisSelection()) { return EPCGMetadataTypes::Vector; }

		// Stage 5b: ContainerCount always yields Double (the count). Checked
		// after field/axis so a chain like `.1.X` (ContainerIndex + Field)
		// still reports Double via the field path.
		if (IsContainerCountSelection()) { return EPCGMetadataTypes::Double; }

		// Component selection: Vector (Pos/Scale) or Quaternion (Rotation).
		// Gated on IsComponentSelection() so chains without a component step
		// (e.g. container-index-only) fall through to the Fallback instead
		// of reading the default-initialized Component member.
		if (IsComponentSelection())
		{
			switch (Component)
			{
			case PCGExTypeOps::ETransformPart::Position:
			case PCGExTypeOps::ETransformPart::Scale:    return EPCGMetadataTypes::Vector;
			case PCGExTypeOps::ETransformPart::Rotation: return EPCGMetadataTypes::Quaternion;
			}
		}

		// ContainerIndex-only (and any other non-type-changing chain) falls
		// through to Fallback: the element type of a container equals the
		// attribute's reported RealType.
		return Fallback;
	}

	void FSubSelection::SetComponent(const PCGExTypeOps::ETransformPart InComponent)
	{
		// Rebuild the chain with a single TransformPart step so classifier
		// methods and dispatch both see a valid component-selection state.
		// Stage 5: the chain is the source of truth; pure flag mutations are gone.
		const ISubAccessor* Accessor = FSubAccessorRegistry::GetTransformPartAccessor();

		FSubSelectionStep Step;
		Step.Accessor = Accessor;
		Step.Parsed.Component = InComponent;
		Step.Parsed.SourceTypeHint = (InComponent == PCGExTypeOps::ETransformPart::Rotation)
			? EPCGMetadataTypes::Quaternion
			: EPCGMetadataTypes::Vector;
		Step.InType = EPCGMetadataTypes::Transform;
		Accessor->ResolveOutputType(Step.InType, Step.Parsed, Step.OutType);
		Step.StepGetFn = Accessor->GetStepGetFn(Step.InType);
		Step.StepSetFn = Accessor->GetStepSetFn(Step.InType);

		ParsedChain.Reset();
		ParsedChain.Steps.Add(MoveTemp(Step));
		ParsedChain.bIsValid = true;
		ParsedChain.FinalType = ParsedChain.Steps.Last().OutType;

		Component = InComponent;
		PossibleSourceType = ParsedChain.Steps.Last().Parsed.SourceTypeHint;
	}

	bool FSubSelection::SetFieldIndex(const int32 InFieldIndex)
	{
		if (InFieldIndex < 0 || InFieldIndex > 3)
		{
			// Invalid index: clear the chain so HasSelection()/IsFieldSelection()
			// return false. Matches the legacy "bIsFieldSet = false" path for
			// out-of-range indices.
			ParsedChain.Reset();
			return false;
		}

		PCGExTypeOps::ESingleField NewField = PCGExTypeOps::ESingleField::X;
		switch (InFieldIndex)
		{
		case 0: NewField = PCGExTypeOps::ESingleField::X; break;
		case 1: NewField = PCGExTypeOps::ESingleField::Y; break;
		case 2: NewField = PCGExTypeOps::ESingleField::Z; break;
		case 3: NewField = PCGExTypeOps::ESingleField::W; break;
		}

		const ISubAccessor* Accessor = FSubAccessorRegistry::GetSingleFieldAccessor();

		FSubSelectionStep Step;
		Step.Accessor = Accessor;
		Step.Parsed.Field = NewField;
		Step.Parsed.FieldIndex = InFieldIndex;
		Step.Parsed.SourceTypeHint = EPCGMetadataTypes::Vector; // default -- real hint comes from parsing aliases
		Step.InType = EPCGMetadataTypes::Unknown;
		Accessor->ResolveOutputType(Step.InType, Step.Parsed, Step.OutType);
		Step.StepGetFn = nullptr;
		Step.StepSetFn = nullptr;

		ParsedChain.Reset();
		ParsedChain.Steps.Add(MoveTemp(Step));
		ParsedChain.bIsValid = true;
		ParsedChain.FinalType = EPCGMetadataTypes::Double;

		Field = NewField;
		return true;
	}

	void FSubSelection::Init(const TArray<FString>& ExtraNames)
	{
		// Chain-backed parser. Builds a step sequence via FSubAccessorRegistry,
		// then populates Field/Axis/Component/PossibleSourceType for
		// dispatch-time reads.
		//
		// Stage 5: the SetFieldIndex(0) unconditional side-effect is GONE.
		// HasSelection() is now true iff the parser actually matched at least
		// one token. Malformed inputs like {Garbage} produce an empty chain.
		// This fixes the long-standing latent bug where every non-empty Init
		// was treated as a field selection regardless of whether a field
		// token was actually present.
		ParsedChain.Reset();

		if (ExtraNames.IsEmpty()) { return; }

		FSubAccessorRegistry::ParseChain(ExtraNames, EPCGMetadataTypes::Unknown, ParsedChain);

		// Locate steps for populating the legacy dispatch fields.
		const ISubAccessor* AxisAccessor = FSubAccessorRegistry::GetAxisAccessor();
		const ISubAccessor* TransformAccessor = FSubAccessorRegistry::GetTransformPartAccessor();
		const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();

		const FSubSelectionStep* AxisStep = nullptr;
		const FSubSelectionStep* CompStep = nullptr;
		const FSubSelectionStep* FieldStep = nullptr;
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			if (Step.Accessor == AxisAccessor) { AxisStep = &Step; }
			else if (Step.Accessor == TransformAccessor) { CompStep = &Step; }
			else if (Step.Accessor == FieldAccessor) { FieldStep = &Step; }
		}

		Axis = AxisStep ? AxisStep->Parsed.Axis : EPCGExAxis::Forward;
		Field = FieldStep ? FieldStep->Parsed.Field : PCGExTypeOps::ESingleField::X;
		// Component default stays Rotation for parity with legacy dispatch --
		// some ApplyGet branches read Component when bIsComponentSet was true,
		// but the chain-based classifier methods gate that read now.
		Component = CompStep ? CompStep->Parsed.Component : PCGExTypeOps::ETransformPart::Rotation;

		// PossibleSourceType: component hint wins, else field hint, else axis hint, else Unknown.
		// Axis tokens are parsed with a Quaternion hint -- an axis alone strongly
		// suggests the source is rotational (FQuat/FRotator/FTransform).
		if (CompStep) { PossibleSourceType = CompStep->Parsed.SourceTypeHint; }
		else if (FieldStep) { PossibleSourceType = FieldStep->Parsed.SourceTypeHint; }
		else if (AxisStep) { PossibleSourceType = AxisStep->Parsed.SourceTypeHint; }
		else { PossibleSourceType = EPCGMetadataTypes::Unknown; }
	}

	//
	// Type-Erased Interface Implementation
	//

	void FSubSelection::ApplyGet(EPCGMetadataTypes SourceType, const void* Source,
	                             void* OutValue, EPCGMetadataTypes& OutType) const
	{
		// Identity: no sub-selection active, copy the whole source through.
		if (!HasSelection())
		{
			const PCGExTypeOps::ITypeOpsBase* Ops = PCGExTypeOps::FTypeOpsRegistry::Get(SourceType);
			if (Ops)
			{
				Ops->Copy(Source, OutValue);
				OutType = SourceType;
			}
			else
			{
				OutType = EPCGMetadataTypes::Unknown;
			}
			return;
		}

		const bool bHasComponent = IsComponentSelection();
		const bool bHasAxis = IsAxisSelection();
		const bool bHasField = IsFieldSelection();

		// FTransform with a component selection: extract the component first,
		// then apply axis/field on the component result.
		//
		// Axis is only meaningful for the Rotation component (a position or
		// scale vector has no orientation). For Position/Scale, axis is
		// silently ignored; dispatch falls through to the field path.
		if (SourceType == EPCGMetadataTypes::Transform && bHasComponent)
		{
			const ISubAccessor* TransformAccessor = FSubAccessorRegistry::GetTransformPartAccessor();
			FAccessorParseResult ComponentParsed;
			ComponentParsed.Component = Component;

			EPCGMetadataTypes ComponentOutType = EPCGMetadataTypes::Unknown;
			TransformAccessor->ResolveOutputType(SourceType, ComponentParsed, ComponentOutType);

			alignas(16) uint8 ComponentBuffer[96];
			TransformAccessor->ApplyGet(SourceType, Source, ComponentOutType, ComponentBuffer, ComponentParsed);

			const bool bIsRotationComponent = (Component == PCGExTypeOps::ETransformPart::Rotation);

			if (bIsRotationComponent && bHasAxis)
			{
				const ISubAccessor* AxisAccessor = FSubAccessorRegistry::GetAxisAccessor();
				FAccessorParseResult AxisParsed;
				AxisParsed.Axis = Axis;
				AxisAccessor->ApplyGet(ComponentOutType, ComponentBuffer,
				                       EPCGMetadataTypes::Vector, OutValue, AxisParsed);
				OutType = EPCGMetadataTypes::Vector;
				return;
			}

			if (bHasField)
			{
				const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
				FAccessorParseResult FieldParsed;
				FieldParsed.Field = Field;
				FieldAccessor->ApplyGet(ComponentOutType, ComponentBuffer,
				                        EPCGMetadataTypes::Double, OutValue, FieldParsed);
				OutType = EPCGMetadataTypes::Double;
				return;
			}

			// Component-only: output the whole component value.
			const PCGExTypeOps::ITypeOpsBase* Ops = PCGExTypeOps::FTypeOpsRegistry::Get(ComponentOutType);
			if (Ops) { Ops->Copy(ComponentBuffer, OutValue); }
			OutType = ComponentOutType;
			return;
		}

		// Non-Transform (or Transform without component): axis wins over field.
		if (bHasAxis)
		{
			const ISubAccessor* AxisAccessor = FSubAccessorRegistry::GetAxisAccessor();
			FAccessorParseResult AxisParsed;
			AxisParsed.Axis = Axis;
			AxisAccessor->ApplyGet(SourceType, Source, EPCGMetadataTypes::Vector, OutValue, AxisParsed);
			OutType = EPCGMetadataTypes::Vector;
			return;
		}

		if (bHasField)
		{
			const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
			FAccessorParseResult FieldParsed;
			FieldParsed.Field = Field;
			FieldAccessor->ApplyGet(SourceType, Source, EPCGMetadataTypes::Double, OutValue, FieldParsed);
			OutType = EPCGMetadataTypes::Double;
			return;
		}

		// Fallback identity copy.
		const PCGExTypeOps::ITypeOpsBase* Ops = PCGExTypeOps::FTypeOpsRegistry::Get(SourceType);
		if (Ops)
		{
			Ops->Copy(Source, OutValue);
			OutType = SourceType;
		}
		else
		{
			OutType = EPCGMetadataTypes::Unknown;
		}
	}

	void FSubSelection::ApplySet(EPCGMetadataTypes TargetType, void* Target,
	                             EPCGMetadataTypes SourceType, const void* Source) const
	{
		if (!HasSelection())
		{
			PCGExTypeOps::FConversionTable::Convert(SourceType, Source, TargetType, Target);
			return;
		}

		const bool bHasComponent = IsComponentSelection();
		const bool bHasField = IsFieldSelection();

		// FTransform with a component selection: read component, mutate via
		// field accessor if applicable, write component back.
		if (TargetType == EPCGMetadataTypes::Transform && bHasComponent)
		{
			const ISubAccessor* TransformAccessor = FSubAccessorRegistry::GetTransformPartAccessor();
			FAccessorParseResult ComponentParsed;
			ComponentParsed.Component = Component;

			if (bHasField)
			{
				// Extract current component into a buffer, inject field, write back.
				EPCGMetadataTypes ComponentType = EPCGMetadataTypes::Unknown;
				TransformAccessor->ResolveOutputType(TargetType, ComponentParsed, ComponentType);

				alignas(16) uint8 ComponentBuffer[96];
				TransformAccessor->ApplyGet(TargetType, Target, ComponentType, ComponentBuffer, ComponentParsed);

				const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
				FAccessorParseResult FieldParsed;
				FieldParsed.Field = Field;
				FieldAccessor->ApplySet(ComponentType, ComponentBuffer,
				                        SourceType, Source, FieldParsed);

				TransformAccessor->ApplySet(TargetType, Target,
				                            ComponentType, ComponentBuffer, ComponentParsed);
			}
			else
			{
				// Whole-component inject.
				TransformAccessor->ApplySet(TargetType, Target, SourceType, Source, ComponentParsed);
			}
			return;
		}

		if (bHasField)
		{
			const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
			FAccessorParseResult FieldParsed;
			FieldParsed.Field = Field;
			FieldAccessor->ApplySet(TargetType, Target, SourceType, Source, FieldParsed);
			return;
		}

		// Fallback: convert + copy.
		PCGExTypeOps::FConversionTable::Convert(SourceType, Source, TargetType, Target);
	}

	double FSubSelection::ExtractFieldToDouble(EPCGMetadataTypes SourceType, const void* Source) const
	{
		const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
		FAccessorParseResult Parsed;
		Parsed.Field = Field;

		double Out = 0.0;
		FieldAccessor->ApplyGet(SourceType, Source, EPCGMetadataTypes::Double, &Out, Parsed);
		return Out;
	}

	void FSubSelection::InjectFieldFromDouble(EPCGMetadataTypes TargetType, void* Target, double Value) const
	{
		const ISubAccessor* FieldAccessor = FSubAccessorRegistry::GetSingleFieldAccessor();
		FAccessorParseResult Parsed;
		Parsed.Field = Field;

		FieldAccessor->ApplySet(TargetType, Target, EPCGMetadataTypes::Double, &Value, Parsed);
	}

	//
	// Legacy Type-Erased Interface (GetVoid/SetVoid)
	//
	// These implement the original signature but use the new type-erased system internally.
	// NOTE: For performance, prefer using FCachedSubSelection in IBufferProxy instead.
	//

	void FSubSelection::GetVoid(EPCGMetadataTypes SourceType, const void* Source, EPCGMetadataTypes WorkingType, void* Target) const
	{
		if (!HasSelection())
		{
			// No sub-selection - just convert
			PCGExTypeOps::FConversionTable::Convert(SourceType, Source, WorkingType, Target);
			return;
		}

		// Apply the sub-selection to get an intermediate value
		alignas(16) uint8 IntermediateBuffer[96];
		EPCGMetadataTypes IntermediateType = EPCGMetadataTypes::Unknown;

		ApplyGet(SourceType, Source, IntermediateBuffer, IntermediateType);

		// Convert from intermediate to working type if needed
		if (IntermediateType == WorkingType)
		{
			// Direct copy using type ops (handles strings, etc.)
			const PCGExTypeOps::ITypeOpsBase* TypeOps = PCGExTypeOps::FTypeOpsRegistry::Get(IntermediateType);
			if (TypeOps)
			{
				TypeOps->Copy(IntermediateBuffer, Target);
			}
		}
		else if (IntermediateType != EPCGMetadataTypes::Unknown)
		{
			// Need conversion
			PCGExTypeOps::FConversionTable::Convert(IntermediateType, IntermediateBuffer, WorkingType, Target);
		}
		else
		{
			// ApplyGet didn't produce valid output, fallback to direct conversion
			PCGExTypeOps::FConversionTable::Convert(SourceType, Source, WorkingType, Target);
		}
	}

	void FSubSelection::SetVoid(EPCGMetadataTypes TargetType, void* Target,
	                            EPCGMetadataTypes SourceType, const void* Source) const
	{
		if (!HasSelection())
		{
			// No sub-selection - just convert
			PCGExTypeOps::FConversionTable::Convert(SourceType, Source, TargetType, Target);
			return;
		}

		// Use the sub-selector ops to apply the set
		ApplySet(TargetType, Target, SourceType, Source);
	}

	//
	// Helper functions for type resolution
	//

	bool TryGetType(const FPCGAttributePropertyInputSelector& InputSelector, const UPCGData* InData, EPCGMetadataTypes& OutType)
	{
		OutType = EPCGMetadataTypes::Unknown;

		if (!IsValid(InData)) { return false; }

		const FPCGAttributePropertyInputSelector FixedSelector = InputSelector.CopyAndFixLast(InData);
		if (!FixedSelector.IsValid()) { return false; }

		if (FixedSelector.GetSelection() == EPCGAttributePropertySelection::Attribute)
		{
			if (!InData->Metadata) { return false; }
			if (const FPCGMetadataAttributeBase* AttributeBase = InData->Metadata->GetConstAttribute(PCGExMetaHelpers::GetAttributeIdentifier(FixedSelector, InData)))
			{
				OutType = static_cast<EPCGMetadataTypes>(AttributeBase->GetTypeId());
			}
		}
		else if (FixedSelector.GetSelection() == EPCGAttributePropertySelection::ExtraProperty)
		{
			OutType = PCGExMetaHelpers::GetPropertyType(FixedSelector.GetExtraProperty());
		}
		else if (FixedSelector.GetSelection() == EPCGAttributePropertySelection::Property)
		{
			OutType = PCGExMetaHelpers::GetPropertyType(FixedSelector.GetPointProperty());
		}

		return OutType != EPCGMetadataTypes::Unknown;
	}

	bool TryGetTypeAndSource(const FPCGAttributePropertyInputSelector& InputSelector, const TSharedPtr<FFacade>& InDataFacade, EPCGMetadataTypes& OutType, EIOSide& InOutSide)
	{
		OutType = EPCGMetadataTypes::Unknown;
		if (InOutSide == EIOSide::In)
		{
			if (!TryGetType(InputSelector, InDataFacade->GetIn(), OutType))
			{
				if (TryGetType(InputSelector, InDataFacade->GetOut(), OutType)) { InOutSide = EIOSide::Out; }
			}
		}
		else
		{
			if (!TryGetType(InputSelector, InDataFacade->GetOut(), OutType))
			{
				if (TryGetType(InputSelector, InDataFacade->GetIn(), OutType)) { InOutSide = EIOSide::In; }
			}
		}

		return OutType != EPCGMetadataTypes::Unknown;
	}

	bool TryGetTypeAndSource(const FName AttributeName, const TSharedPtr<FFacade>& InDataFacade, EPCGMetadataTypes& OutType, EIOSide& InOutSource)
	{
		FPCGAttributePropertyInputSelector Selector;
		Selector.SetAttributeName(AttributeName);
		return TryGetTypeAndSource(Selector, InDataFacade, OutType, InOutSource);
	}
}
