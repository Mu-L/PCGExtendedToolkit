// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExSubSelection.h"

#include "CoreMinimal.h"
#include "Data/PCGExSubSelectionOpsImpl.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"

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

	EPCGMetadataTypes FSubSelection::GetSubType(const EPCGMetadataTypes Fallback) const
	{
		if (!bIsValid) { return Fallback; }
		if (bIsFieldSet) { return EPCGMetadataTypes::Double; }
		if (bIsAxisSet) { return EPCGMetadataTypes::Vector; }

		switch (Component)
		{
		case PCGExTypeOps::ETransformPart::Position:
		case PCGExTypeOps::ETransformPart::Scale: return EPCGMetadataTypes::Vector;
		case PCGExTypeOps::ETransformPart::Rotation: return EPCGMetadataTypes::Quaternion;
		}

		return Fallback;
	}

	void FSubSelection::SetComponent(const PCGExTypeOps::ETransformPart InComponent)
	{
		bIsValid = true;
		bIsComponentSet = true;
		Component = InComponent;
	}

	bool FSubSelection::SetFieldIndex(const int32 InFieldIndex)
	{
		if (InFieldIndex < 0 || InFieldIndex > 3)
		{
			bIsFieldSet = false;
			return false;
		}

		bIsValid = true;
		bIsFieldSet = true;

		if (InFieldIndex == 0) { Field = PCGExTypeOps::ESingleField::X; }
		else if (InFieldIndex == 1) { Field = PCGExTypeOps::ESingleField::Y; }
		else if (InFieldIndex == 2) { Field = PCGExTypeOps::ESingleField::Z; }
		else if (InFieldIndex == 3) { Field = PCGExTypeOps::ESingleField::W; }

		return true;
	}

	void FSubSelection::Init(const TArray<FString>& ExtraNames)
	{
		// Chain-backed parser. Builds a step sequence via FSubAccessorRegistry,
		// then projects it onto the legacy flag layout so external readers
		// (BlendOpFactory, AttributeRemap, ProxyData, FCachedSubSelection)
		// keep working unchanged.
		//
		// The flag-population sequence below intentionally mirrors the
		// pre-refactor STRMAP parser exactly, including the SetFieldIndex(0)
		// side-effect that always sets bIsValid+bIsFieldSet for non-empty
		// inputs and the {Rotation, Quaternion} component default. See the
		// migration doc (.claude/migration_5.8_phase6_subaccessor_stage1.md)
		// for the rationale and a note on the latent bugs locked in by
		// parity.

		ParsedChain.Reset();

		if (ExtraNames.IsEmpty())
		{
			bIsValid = false;
			return;
		}

		FSubAccessorRegistry::ParseChain(ExtraNames, EPCGMetadataTypes::Unknown, ParsedChain);

		// Locate steps by accessor identity. ParseChain produces at most one
		// of each kind under the Stage 1 positional rule.
		const FSubSelectionStep* AxisStep = nullptr;
		const FSubSelectionStep* CompStep = nullptr;
		const FSubSelectionStep* FieldStep = nullptr;
		for (const FSubSelectionStep& Step : ParsedChain.Steps)
		{
			const FString Name = Step.Accessor->GetDisplayName();
			if (Name == TEXT("Axis")) { AxisStep = &Step; }
			else if (Name == TEXT("TransformPart")) { CompStep = &Step; }
			else if (Name == TEXT("SingleField")) { FieldStep = &Step; }
		}

		// Axis.
		bIsAxisSet = (AxisStep != nullptr);
		Axis = AxisStep ? AxisStep->Parsed.Axis : EPCGExAxis::Forward;

		// Component, with legacy default {Rotation, Quaternion} when no match.
		bIsComponentSet = (CompStep != nullptr);
		const PCGExTypeOps::ETransformPart ComponentLocal =
			CompStep ? CompStep->Parsed.Component : PCGExTypeOps::ETransformPart::Rotation;
		const EPCGMetadataTypes ComponentHint =
			CompStep ? CompStep->Parsed.SourceTypeHint : EPCGMetadataTypes::Quaternion;

		// bIsValid pre-field.
		if (bIsAxisSet) { bIsValid = true; }
		else { bIsValid = bIsComponentSet; }

		Component = ComponentLocal;
		PossibleSourceType = ComponentHint;

		// Field with legacy default {X, Unknown, 0} when no match.
		bIsFieldSet = (FieldStep != nullptr);
		const PCGExTypeOps::ESingleField FieldLocal =
			FieldStep ? FieldStep->Parsed.Field : PCGExTypeOps::ESingleField::X;
		const int32 FieldIndexLocal = FieldStep ? FieldStep->Parsed.FieldIndex : 0;
		const EPCGMetadataTypes FieldHint =
			FieldStep ? FieldStep->Parsed.SourceTypeHint : EPCGMetadataTypes::Unknown;

		Field = FieldLocal;

		// SetFieldIndex side-effect: always called with the parsed (or default 0)
		// index. This is what forces bIsValid=true and bIsFieldSet=true on every
		// non-empty Init -- a load-bearing legacy behavior.
		SetFieldIndex(FieldIndexLocal);

		// Post-SetFieldIndex: bIsFieldSet is now true.
		if (bIsFieldSet)
		{
			bIsValid = true;
			if (!bIsComponentSet) { PossibleSourceType = FieldHint; }
		}
	}

	//
	// Type-Erased Interface Implementation
	//

	void FSubSelection::ApplyGet(EPCGMetadataTypes SourceType, const void* Source,
	                             void* OutValue, EPCGMetadataTypes& OutType) const
	{
		const ISubSelectorOps* Ops = FSubSelectorRegistry::Get(SourceType);
		if (!Ops)
		{
			OutType = EPCGMetadataTypes::Unknown;
			return;
		}

		Ops->ApplyGetSelection(Source, *this, OutValue, OutType);
	}

	void FSubSelection::ApplySet(EPCGMetadataTypes TargetType, void* Target,
	                             EPCGMetadataTypes SourceType, const void* Source) const
	{
		const ISubSelectorOps* Ops = FSubSelectorRegistry::Get(TargetType);
		if (!Ops) { return; }

		Ops->ApplySetSelection(Target, *this, Source, SourceType);
	}

	double FSubSelection::ExtractFieldToDouble(EPCGMetadataTypes SourceType, const void* Source) const
	{
		const ISubSelectorOps* Ops = FSubSelectorRegistry::Get(SourceType);
		if (!Ops) { return 0.0; }

		return Ops->ExtractField(Source, Field);
	}

	void FSubSelection::InjectFieldFromDouble(EPCGMetadataTypes TargetType, void* Target, double Value) const
	{
		const ISubSelectorOps* Ops = FSubSelectorRegistry::Get(TargetType);
		if (!Ops) { return; }

		Ops->InjectField(Target, Value, Field);
	}

	//
	// Legacy Type-Erased Interface (GetVoid/SetVoid)
	//
	// These implement the original signature but use the new type-erased system internally.
	// NOTE: For performance, prefer using FCachedSubSelection in IBufferProxy instead.
	//

	void FSubSelection::GetVoid(EPCGMetadataTypes SourceType, const void* Source, EPCGMetadataTypes WorkingType, void* Target) const
	{
		if (!bIsValid)
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
		if (!bIsValid)
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
