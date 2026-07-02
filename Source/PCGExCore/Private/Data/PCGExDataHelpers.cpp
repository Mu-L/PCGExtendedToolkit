// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/PCGExDataHelpers.h"

#include "PCGExLog.h"
#include "Data/PCGExData.h"
#include "Data/PCGExPointIO.h"
#include "Data/PCGExSubSelection.h"
#include "Data/Buffers/PCGExBufferProperty.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataDomain.h"
#include "Types/PCGExAttributeIdentity.h"
#include "Types/PCGExTypes.h"

namespace PCGExData::Helpers
{
	void CopyBuffersValues(
		const TSharedPtr<FFacade>& SourceFacade,
		const TSharedPtr<FFacade>& TargetFacade,
		const TArray<int32>& SourcePointIndices,
		const TSet<FName>* IgnoreList)
	{
		for (const TSharedPtr<IBuffer>& SrcBuffer : SourceFacade->Buffers)
		{
			if (!SrcBuffer || !SrcBuffer->IsWritable() || !SrcBuffer->IsEnabled() ||
				(IgnoreList && IgnoreList->Contains(SrcBuffer->Identifier.Name)))
			{
				continue;
			}

			// TODO: support Data domain. Currently only Elements.
			if (SrcBuffer->GetUnderlyingDomain() != EDomainType::Elements)
			{
				continue;
			}

			const FPCGMetadataAttributeBase* SrcAttr = SrcBuffer->OutAttribute;
			if (!SrcAttr)
			{
				continue;
			}

			// Attribute-driven: tier-agnostic, no EPCGMetadataTypes surfaces here.
			TSharedPtr<IBuffer> DstBuffer = TargetFacade->GetWritableFromAttribute(SrcAttr, EBufferInit::Inherit);
			if (!DstBuffer)
			{
				continue;
			}

			const int32 NumCopy = SourcePointIndices.Num();

			// Scope-based parallel: one FScopedTypedValue per worker task (not per iteration).
			// Amortizes the scoped-value construction cost across each thread's chunk.
			PCGExMT::ParallelOrSequentialScoped(
				NumCopy,
				[&](const PCGExMT::FScope& Scope)
				{
					PCGExTypes::FScopedTypedValue Temp = SrcBuffer->MakeScopedValue();
					PCGEX_SCOPE_LOOP(i)
					{
						SrcBuffer->GetVoid(SourcePointIndices[i], Temp);
						DstBuffer->SetVoid(i, Temp);
					}
				},
				1024);
		}
	}

	bool PropertyCopyAttribute(
		const FPCGMetadataAttributeBase* SourceAttr, const PCGMetadataEntryKey SourceKey,
		FPCGMetadataAttributeBase* TargetAttr, const PCGMetadataEntryKey TargetKey)
	{
		if (!SourceAttr || !TargetAttr)
		{
			return false;
		}

		const void* SrcAddr = SourceAttr->GetReadAddressFromEntryKey_Unsafe(SourceKey);
		if (!SrcAddr)
		{
			return false;
		}

		// PERF -- this allocates a transient FProperty per call (CreateInnerPropertyFromDesc walks the
		// desc + heap-allocates) and deletes it. Fine for one-shot single-value carry (DataForward,
		// PointsToBounds, BlendingHelpers per-attribute outer loop). NOT fine for per-element loops --
		// if you need that, drive the loop through an FPropertyBuffer instance whose CachedInnerProperty
		// is built once at InitForRead/InitForWrite, and call SetFromVoidProperty per element.
		FProperty* TempProp = FPropertyBuffer::CreateInnerPropertyFromDesc(SourceAttr->GetAttributeDesc());
		if (!TempProp)
		{
			return false;
		}

		TargetAttr->SetValueFromProperty(TargetKey, SrcAddr, TempProp);
		delete TempProp;
		return true;
	}

	bool PropertyBroadcastAttribute(
		const FPCGMetadataAttributeBase* SourceAttr, const PCGMetadataEntryKey SourceKey,
		const TSharedPtr<IBuffer>& TargetWriter)
	{
		if (!SourceAttr || !TargetWriter)
		{
			return false;
		}

		const void* SrcAddr = SourceAttr->GetReadAddressFromEntryKey_Unsafe(SourceKey);
		if (!SrcAddr)
		{
			return false;
		}

		PCGExTypes::FScopedTypedValue Scratch = TargetWriter->MakeScopedValue();
		const FProperty* Prop = Scratch.GetProperty();
		if (!Prop)
		{
			return false;
		}

		Prop->CopyCompleteValue(Scratch.GetRaw(), SrcAddr);
		const int32 NumSlots = TargetWriter->GetNumValues(EIOSide::Out);
		for (int32 s = 0; s < NumSlots; s++)
		{
			TargetWriter->SetVoid(s, Scratch);
		}
		return NumSlots > 0;
	}

	bool PropertyScatterAttribute(
		const FPCGMetadataAttributeBase* SourceAttr, const PCGMetadataEntryKey SourceKey,
		const TSharedPtr<IBuffer>& TargetWriter, TArrayView<const int32> Indices)
	{
		if (!SourceAttr || !TargetWriter || Indices.IsEmpty())
		{
			return false;
		}

		const void* SrcAddr = SourceAttr->GetReadAddressFromEntryKey_Unsafe(SourceKey);
		if (!SrcAddr)
		{
			return false;
		}

		PCGExTypes::FScopedTypedValue Scratch = TargetWriter->MakeScopedValue();
		const FProperty* Prop = Scratch.GetProperty();
		if (!Prop)
		{
			return false;
		}

		Prop->CopyCompleteValue(Scratch.GetRaw(), SrcAddr);
		for (int32 Index : Indices)
		{
			TargetWriter->SetVoid(Index, Scratch);
		}
		return true;
	}

	bool PropertyCopyAttributeRange(
		const TSharedPtr<FPointIO>& SourceIO, const FAttributeIdentity& SourceIdentity,
		const TSharedRef<FPropertyArrayBuffer>& TargetBuffer,
		const PCGExMT::FScope& ReadScope, const PCGExMT::FScope& WriteScope, const bool bReverse)
	{
		check(ReadScope.Count == WriteScope.Count);

		if (!SourceIO || ReadScope.Count <= 0)
		{
			return false;
		}

		const UPCGBasePointData* SourceData = SourceIO->GetIn();
		if (!SourceData || !SourceData->Metadata)
		{
			return false;
		}

		const FPCGMetadataAttributeBase* SourceAttr = SourceData->Metadata->GetConstAttribute(SourceIdentity.GetIdentifier());
		if (!SourceAttr)
		{
			return false;
		}

		auto EntryKeys = SourceData->GetConstMetadataEntryValueRange();
		if (EntryKeys.Num() < ReadScope.End)
		{
			return false;
		}

		bool bAnyCopied = false;
		for (int32 i = 0; i < ReadScope.Count; i++)
		{
			const int32 ReadIdx = bReverse ? (ReadScope.End - 1 - i) : (ReadScope.Start + i);
			const PCGMetadataEntryKey EntryKey = EntryKeys[ReadIdx];
			const void* SrcAddr = SourceAttr->GetReadAddressFromEntryKey_Unsafe(EntryKey);
			if (!SrcAddr)
			{
				continue;
			}
			TargetBuffer->SetFromVoidProperty(WriteScope.Start + i, SrcAddr);
			bAnyCopied = true;
		}
		return bAnyCopied;
	}

	template <typename T>
	T ReadDataValue(const FPCGMetadataAttributeBase* Attribute)
	{
		// Read a single value from a @Data domain attribute (one value per dataset, not per-point).
		// If the attribute has no local entries, the value lives on an ancestor in the metadata
		// inheritance chain.
		
		if (!ensure(Attribute))
		{
			// Should not happen, callsite need to gate against reading nothing
			return T();
		}
		
		if (Attribute->GetNumberOfEntries())
		{
			return Attribute->GetValueFromItemKey<T>(PCGFirstEntryKey);
		}

		const FPCGMetadataDomain* Domain = Attribute->GetMetadataDomain();
		const FPCGAttributeIdentifier Identifier(Attribute->Name, Domain->GetDomainID());

		TWeakObjectPtr<const UPCGMetadata> ParentMeta = Domain->GetTopMetadata()->GetParentPtr();
		while (const UPCGMetadata* Meta = ParentMeta.Get())
		{
			const FPCGMetadataAttributeBase* Ancestor = Meta->GetConstAttribute(Identifier);
			if (!Ancestor || !Ancestor->GetAttributeDesc().IsSameType(Attribute->GetAttributeDesc()))
			{
				// Chain broken by an upstream filter/recreation -- nothing further to inherit.
				break;
			}

			if (Ancestor->GetNumberOfEntries())
			{
				return Ancestor->GetValueFromItemKey<T>(PCGFirstEntryKey);
			}

			ParentMeta = Meta->GetParentPtr();
		}

		if (Attribute->GetParent())
		{
			// A raw parent exists but the live weak chain carried no value: the value-holding
			// ancestor was garbage collected (or dropped the attribute). The value is lost.
			UE_LOG(LogPCGEx, Warning,
			       TEXT("PCGEx: @Data attribute '%s' could not be resolved -- its ancestor metadata was garbage collected or the attribute was removed upstream. Falling back to the attribute's default value."),
			       *Attribute->Name.ToString());
		}

		return Attribute->GetValueFromItemKey<T>(Attribute->GetValueKey(PCGDefaultValueKey));
	}

	template <typename T>
	T ReadDataValue(const FPCGMetadataAttributeBase* Attribute, T Fallback)
	{
		T Value = Fallback;
		// Container/extended types fall through (no meaningful conversion to templated T) -- fallback wins.
		PCGExMetaHelpers::ExecuteWithRightType(Attribute, [&](auto DummyValue)
		{
			using T_VALUE = decltype(DummyValue);
			Value = PCGExTypeOps::Convert<T_VALUE, T>(ReadDataValue<T_VALUE>(static_cast<const FPCGMetadataAttribute<T_VALUE>*>(Attribute)));
		});
		return Value;
	}

	template <typename T>
	void SetDataValue(FPCGMetadataAttributeBase* Attribute, const T Value)
	{
		Attribute->SetValue(PCGFirstEntryKey, Value);
		Attribute->SetDefaultValue(Value);
	}

	template <typename T>
	void SetDataValue(UPCGData* InData, FName Name, const T Value)
	{
		FPCGAttributePropertyInputSelector SafetySelector;
		SafetySelector.Update(Name.ToString());

		if (SafetySelector.GetSelection() != EPCGAttributePropertySelection::Attribute)
		{
			UE_LOG(LogPCGEx, Error, TEXT("Attempting to write @Data value to a non-attribute domain."))
			return;
		}

		FPCGAttributeIdentifier Identifier = FPCGAttributeIdentifier(SafetySelector.GetAttributeName(), EPCGMetadataDomainFlag::Data);
		SetDataValue<T>(InData->Metadata->FindOrCreateAttribute<T>(Identifier, Value, true, true), Value);
	}

	template <typename T>
	void SetDataValue(UPCGData* InData, FPCGAttributeIdentifier Identifier, const T Value)
	{
		SetDataValue<T>(InData, Identifier.Name, Value);
	}

	void LocalizeDataValues(UPCGData* InData)
	{
		if (!InData || !InData->Metadata)
		{
			return;
		}

		UPCGMetadata* Metadata = InData->MutableMetadata();
		const FPCGMetadataDomain* DataDomain = Metadata->GetConstMetadataDomain(PCGMetadataDomainID::Data);
		if (!DataDomain)
		{
			return;
		}

		TArray<FName> AttributeNames;
		TArray<EPCGMetadataTypes> AttributeTypes;
		DataDomain->GetAttributes(AttributeNames, AttributeTypes);

		for (int32 i = 0; i < AttributeNames.Num(); i++)
		{
			const FPCGAttributeIdentifier Identifier(AttributeNames[i], PCGMetadataDomainID::Data);

			// Fast typed path for plain scalar types. bTypedHandled stays false when the type isn't
			// in the supported set OR the typed lookup fails (property-backed attribute reporting a
			// scalar type) -- both fall through to the type-erased carry below.
			bool bTypedHandled = false;
			PCGExMetaHelpers::ExecuteWithRightType(AttributeTypes[i], [&](auto DummyValue)
			{
				using T = decltype(DummyValue);
				FPCGMetadataAttributeBase* Attribute = PCGExMetaHelpers::TryGetMutableAttribute<T>(Metadata, Identifier);
				if (!Attribute)
				{
					return;
				}

				bTypedHandled = true;
				if (Attribute->GetNumberOfEntries())
				{
					return;
				}
				SetDataValue<T>(Attribute, ReadDataValue<T>(Attribute));
			});

			if (bTypedHandled)
			{
				continue;
			}

			// Container/extended (property-backed) types: type-erased single-value carry from the
			// nearest live, value-carrying ancestor -- same GC-safe weak-chain walk as ReadDataValue.
			FPCGMetadataAttributeBase* Attribute = Metadata->GetMutableAttribute(Identifier);
			if (!Attribute || Attribute->GetNumberOfEntries())
			{
				continue;
			}

			const FPCGMetadataAttributeBase* ValueAncestor = nullptr;
			TWeakObjectPtr<const UPCGMetadata> ParentMeta = Metadata->GetParentPtr();
			while (const UPCGMetadata* Meta = ParentMeta.Get())
			{
				const FPCGMetadataAttributeBase* Ancestor = Meta->GetConstAttribute(Identifier);
				if (!Ancestor || !Ancestor->GetAttributeDesc().IsSameType(Attribute->GetAttributeDesc()))
				{
					break;
				}

				if (Ancestor->GetNumberOfEntries())
				{
					ValueAncestor = Ancestor;
					break;
				}

				ParentMeta = Meta->GetParentPtr();
			}

			if (!ValueAncestor)
			{
				continue;
			}

			// Mirror SetDataValue semantics: materialize the local entry AND align the default value.
			PropertyCopyAttribute(ValueAncestor, PCGFirstEntryKey, Attribute, PCGFirstEntryKey);
			PropertyCopyAttribute(ValueAncestor, PCGFirstEntryKey, Attribute, PCGDefaultValueKey);
		}
	}

#define PCGEX_TPL(_TYPE, _NAME, ...) \
template PCGEXCORE_API _TYPE ReadDataValue<_TYPE>(const FPCGMetadataAttributeBase* Attribute); \
template PCGEXCORE_API _TYPE ReadDataValue<_TYPE>(const FPCGMetadataAttributeBase* Attribute, _TYPE Fallback); \
template PCGEXCORE_API void SetDataValue<_TYPE>(FPCGMetadataAttributeBase* Attribute, const _TYPE Value); \
template PCGEXCORE_API void SetDataValue<_TYPE>(UPCGData* InData, FName Name, const _TYPE Value); \
template PCGEXCORE_API void SetDataValue<_TYPE>(UPCGData* InData, FPCGAttributeIdentifier Identifier, const _TYPE Value);
	PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_TPL)
#undef PCGEX_TPL

	template <typename T>
	bool TryReadDataValue(FPCGExContext* InContext, const UPCGData* InData, const FPCGAttributePropertyInputSelector& InSelector, T& OutValue, const bool bQuiet)
	{
		bool bSuccess = false;
		const UPCGMetadata* InMetadata = InData->Metadata;

		if (!InMetadata)
		{
			return false;
		}

		FSubSelection SubSelection(InSelector);
		FPCGAttributeIdentifier SanitizedIdentifier = PCGExMetaHelpers::GetAttributeIdentifier(InSelector, InData);
		SanitizedIdentifier.MetadataDomain = EPCGMetadataDomainFlag::Data; // Force data domain

		if (const FPCGMetadataAttributeBase* SourceAttribute = InMetadata->GetConstAttribute(SanitizedIdentifier))
		{
			// Container/extended source types: TryReadDataValue<T> can't represent them; falls through to bSuccess=false.
			PCGExMetaHelpers::ExecuteWithRightType(SourceAttribute, [&](auto DummyValue)
			{
				using T_VALUE = decltype(DummyValue);

				const T_VALUE Value = ReadDataValue<T_VALUE>(SourceAttribute);

				if (SubSelection.HasSelection())
				{
					OutValue = SubSelection.Get<T_VALUE, T>(Value);
				}
				else
				{
					OutValue = PCGExTypeOps::Convert<T_VALUE, T>(Value);
				}

				bSuccess = true;
			});
		}
		else
		{
			if (!bQuiet && InContext)
			{
				PCGEX_LOG_INVALID_SELECTOR_C(InContext, Attribute, InSelector)
			}
			return false;
		}

		return bSuccess;
	}

	template <typename T>
	bool TryReadDataValue(FPCGExContext* InContext, const UPCGData* InData, const FName& InName, T& OutValue, const bool bQuiet)
	{
		FPCGAttributePropertyInputSelector Selector;
		Selector.Update(InName.ToString());
		return TryReadDataValue<T>(InContext, InData, Selector.CopyAndFixLast(InData), OutValue, bQuiet);
	}

	template <typename T>
	bool TryReadDataValue(const TSharedPtr<FPointIO>& InIO, const FName& InName, T& OutValue, const bool bQuiet)
	{
		return TryReadDataValue(InIO->GetContext(), InIO->GetIn(), InName, OutValue, bQuiet);
	}

	template <typename T>
	bool TryReadDataValue(const TSharedPtr<FPointIO>& InIO, const FPCGAttributePropertyInputSelector& InSelector, T& OutValue, const bool bQuiet)
	{
		return TryReadDataValue(InIO->GetContext(), InIO->GetIn(), InSelector, OutValue, bQuiet);
	}

	template <typename T>
	bool TryGetSettingDataValue(FPCGExContext* InContext, const UPCGData* InData, const EPCGExInputValueType Input, const FPCGAttributePropertyInputSelector& InSelector, const T& InConstant, T& OutValue, const bool bQuiet)
	{
		if (Input == EPCGExInputValueType::Constant)
		{
			OutValue = InConstant;
			return true;
		}

		return TryReadDataValue<T>(InContext, InData, InSelector, OutValue, bQuiet);
	}

	template <typename T>
	bool TryGetSettingDataValue(FPCGExContext* InContext, const UPCGData* InData, const EPCGExInputValueType Input, const FName& InName, const T& InConstant, T& OutValue, const bool bQuiet)
	{
		if (Input == EPCGExInputValueType::Constant)
		{
			OutValue = InConstant;
			return true;
		}

		FPCGAttributePropertyInputSelector Selector;
		Selector.Update(InName.ToString());
		return TryReadDataValue<T>(InContext, InData, Selector.CopyAndFixLast(InData), OutValue, bQuiet);
	}

	template <typename T>
	bool TryGetSettingDataValue(const TSharedPtr<FPointIO>& InIO, const EPCGExInputValueType Input, const FName& InName, const T& InConstant, T& OutValue, const bool bQuiet)
	{
		return TryGetSettingDataValue(InIO->GetContext(), InIO->GetIn(), Input, InName, InConstant, OutValue, bQuiet);
	}

	template <typename T>
	bool TryGetSettingDataValue(const TSharedPtr<FPointIO>& InIO, const EPCGExInputValueType Input, const FPCGAttributePropertyInputSelector& InSelector, const T& InConstant, T& OutValue, const bool bQuiet)
	{
		return TryGetSettingDataValue(InIO->GetContext(), InIO->GetIn(), Input, InSelector, InConstant, OutValue, bQuiet);
	}

#define PCGEX_TPL(_TYPE, _NAME, ...) \
template PCGEXCORE_API bool TryReadDataValue<_TYPE>(FPCGExContext* InContext, const UPCGData* InData, const FPCGAttributePropertyInputSelector& InSelector, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryReadDataValue<_TYPE>(FPCGExContext* InContext, const UPCGData* InData, const FName& InName, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryReadDataValue<_TYPE>(const TSharedPtr<PCGExData::FPointIO>& InIO, const FName& InName, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryReadDataValue<_TYPE>(const TSharedPtr<PCGExData::FPointIO>& InIO, const FPCGAttributePropertyInputSelector& InSelector, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryGetSettingDataValue<_TYPE>( FPCGExContext* InContext, const UPCGData* InData, const EPCGExInputValueType Input, const FPCGAttributePropertyInputSelector& InSelector, const _TYPE& InConstant, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryGetSettingDataValue<_TYPE>( FPCGExContext* InContext, const UPCGData* InData, const EPCGExInputValueType Input, const FName& InName, const _TYPE& InConstant, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryGetSettingDataValue<_TYPE>( const TSharedPtr<PCGExData::FPointIO>& InIO, const EPCGExInputValueType Input, const FName& InName, const _TYPE& InConstant, _TYPE& OutValue, const bool bQuiet); \
template PCGEXCORE_API bool TryGetSettingDataValue<_TYPE>( const TSharedPtr<PCGExData::FPointIO>& InIO, const EPCGExInputValueType Input, const FPCGAttributePropertyInputSelector& InSelector, const _TYPE& InConstant, _TYPE& OutValue, const bool bQuiet);
	PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_TPL)
#undef PCGEX_TPL
}
