// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/Utils/PCGExDataForward.h"

#include "PCGExLog.h"
#include "Data/PCGExAttributeBroadcaster.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataHelpers.h"
#include "Data/PCGExPointIO.h"
#include "Core/PCGExMTCommon.h"

namespace PCGExData
{
	FPCGAttributeIdentifier FDataForwardHandler::GetTargetIdentifier(const FAttributeIdentity& Identity) const
	{
		switch (Domain)
		{
		case EForwardDomain::ToData:
			return FPCGAttributeIdentifier(Identity.Name, PCGMetadataDomainID::Data);
		case EForwardDomain::ToElements:
			return FPCGAttributeIdentifier(Identity.Name, PCGMetadataDomainID::Elements);
		default:
			return Identity.GetIdentifier();
		}
	}

	bool FDataForwardHandler::RedirectsDomain(const FAttributeIdentity& Identity) const
	{
		return Domain != EForwardDomain::Inherit && !(GetTargetIdentifier(Identity) == Identity.GetIdentifier());
	}

	FDataForwardHandler::FDataForwardHandler(const FPCGExForwardDetails& InDetails, const TSharedPtr<FFacade>& InSourceDataFacade, const EForwardDomain InDomain)
		: Details(InDetails)
		  , SourceDataFacade(InSourceDataFacade)
		  , TargetDataFacade(nullptr)
		  , Domain(InDomain)
	{
		if (!Details.bEnabled)
		{
			return;
		}

		Details.Init();
		FAttributeIdentity::Get(InSourceDataFacade->GetIn()->Metadata, Identities);
		Details.Filter(Identities);
	}

	FDataForwardHandler::FDataForwardHandler(const FPCGExForwardDetails& InDetails, const TSharedPtr<FFacade>& InSourceDataFacade, const TSharedPtr<FFacade>& InTargetDataFacade, const EForwardDomain InDomain, const TSet<FName>* InIgnoredAttributes)
		: Details(InDetails)
		  , SourceDataFacade(InSourceDataFacade)
		  , TargetDataFacade(InTargetDataFacade)
		  , Domain(InDomain)
	{
		Details.Init();
		FAttributeIdentity::Get(InSourceDataFacade->GetIn()->Metadata, Identities, InIgnoredAttributes);
		Details.Filter(Identities);

		const int32 NumAttributes = Identities.Num();

		// Readers/Writers stay index-aligned with Identities (null on failure) -- the typed Forward
		// paths cast by Identities[i]'s type, so a compacted array would cast to the wrong type.
		Readers.Init(nullptr, NumAttributes);
		Writers.Init(nullptr, NumAttributes);

		// Init forwarded attributes on target
		for (int i = 0; i < NumAttributes; i++)
		{
			const FAttributeIdentity& Identity = Identities[i];

			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					// Typed path -- fast, directly typed buffers stored as IBuffer.
					using T = decltype(DummyValue);
					TSharedPtr<TBuffer<T>> Reader = SourceDataFacade->GetReadable<T>(Identity.GetIdentifier());
					if (!Reader)
					{
						return;
					}
					TSharedPtr<TBuffer<T>> Writer = nullptr;
					if (RedirectsDomain(Identity))
					{
						const T DefaultValue = Identity.InDataDomain() ? Helpers::ReadDataValue<T>(Reader->InAttribute) : Reader->InAttribute->GetValueFromItemKey<T>(PCGDefaultValueKey);
						Writer = TargetDataFacade->GetWritable<T>(GetTargetIdentifier(Identity), DefaultValue, Reader->InAttribute->AllowsInterpolation(), EBufferInit::Inherit);
					}
					else
					{
						Writer = TargetDataFacade->GetWritable<T>(Reader->InAttribute, EBufferInit::Inherit);
					}
					if (!Writer)
					{
						return;
					}
					Readers[i] = Reader;
					Writers[i] = Writer;
				},
				[&]()
				{
					// Property-backed path -- extended/container types route through FFacade's
					// generic GetReadable/GetWritable which already fall back to FPropertyBuffer.
					if (!Identity.Attribute)
					{
						return;
					}
					if (RedirectsDomain(Identity))
					{
						UE_LOG(LogPCGEx, Warning, TEXT("Domain conversion not supported on property-backed attribute '%s' -- skipped."), *Identity.Name.ToString());
						return;
					}
					TSharedPtr<IBuffer> Reader = SourceDataFacade->GetReadable(Identity, EIOSide::In, false);
					if (!Reader)
					{
						return;
					}
					TSharedPtr<IBuffer> Writer = TargetDataFacade->GetWritable(Identity.GetType(), Identity.Attribute, EBufferInit::Inherit);
					if (!Writer)
					{
						return;
					}
					Readers[i] = Reader;
					Writers[i] = Writer;
				});
		}
	}

	void FDataForwardHandler::ValidateIdentities(FValidateFn&& Fn)
	{
		// Readers/Writers exist only on prepared (two-facade) handlers and must stay index-aligned with Identities.
		const bool bPrepared = Readers.Num() == Identities.Num();
		int32 WriteIndex = 0;

		for (int32 i = 0; i < Identities.Num(); i++)
		{
			if (!Fn(Identities[i]))
			{
				continue;
			}

			if (WriteIndex != i)
			{
				Identities[WriteIndex] = Identities[i];
				if (bPrepared)
				{
					Readers[WriteIndex] = Readers[i];
					Writers[WriteIndex] = Writers[i];
				}
			}

			WriteIndex++;
		}

		Identities.SetNum(WriteIndex);
		if (bPrepared)
		{
			Readers.SetNum(WriteIndex);
			Writers.SetNum(WriteIndex);
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, const int32 TargetIndex)
	{
		const int32 NumAttributes = Identities.Num();

		for (int i = 0; i < NumAttributes; i++)
		{
			const FAttributeIdentity& Identity = Identities[i];
			if (!Readers.IsValidIndex(i) || !Readers[i] || !Writers[i])
			{
				continue;
			}

			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);
					TSharedPtr<TBuffer<T>> Reader = StaticCastSharedPtr<TBuffer<T>>(Readers[i]);
					TSharedPtr<TBuffer<T>> Writer = StaticCastSharedPtr<TBuffer<T>>(Writers[i]);
					// A @Data writer (ToData policy, or an inherited @Data source) has a single slot
					Writer->SetValue(Writer->GetUnderlyingDomain() == EDomainType::Elements ? TargetIndex : 0, Reader->Read(SourceIndex));
				},
				[&]()
				{
					// Property-backed: read source slot to scratch via property reflection, then write to target.
					// Both buffers cache the same FProperty; void* + property handle deep-copy.
					PCGExTypes::FScopedTypedValue Scratch = Readers[i]->MakeScopedValue();
					Readers[i]->ReadVoid(SourceIndex, Scratch);
					Writers[i]->SetVoid(TargetIndex, Scratch);
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, const TArray<int32>& Indices)
	{
		// Prepared-target variant (requires the target-facade constructor): fans one source row out to
		// many target indices through the pre-created writers. No lazy buffer/attribute creation happens
		// here, so this is safe to call from concurrent tasks once the handler is built.
		if (Indices.IsEmpty())
		{
			return;
		}

		const int32 NumAttributes = Identities.Num();

		for (int i = 0; i < NumAttributes; i++)
		{
			const FAttributeIdentity& Identity = Identities[i];
			if (!Readers.IsValidIndex(i) || !Readers[i] || !Writers[i])
			{
				continue;
			}

			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);
					TSharedPtr<TBuffer<T>> Reader = StaticCastSharedPtr<TBuffer<T>>(Readers[i]);
					TSharedPtr<TBuffer<T>> Writer = StaticCastSharedPtr<TBuffer<T>>(Writers[i]);

					const T ForwardValue = Reader->Read(SourceIndex);

					if (Writer->GetUnderlyingDomain() == EDomainType::Elements)
					{
						TSharedPtr<TArrayBuffer<T>> ElementsWriter = StaticCastSharedPtr<TArrayBuffer<T>>(Writer);
						TArray<T>& Values = *ElementsWriter->GetOutValues();
						for (const int32 TargetIndex : Indices)
						{
							Values[TargetIndex] = ForwardValue;
						}
					}
					else
					{
						Writer->SetValue(0, ForwardValue);
					}
				},
				[&]()
				{
					// Property-backed: read the source slot once to scratch, deep-copy onto every target.
					PCGExTypes::FScopedTypedValue Scratch = Readers[i]->MakeScopedValue();
					Readers[i]->ReadVoid(SourceIndex, Scratch);
					for (const int32 TargetIndex : Indices)
					{
						Writers[i]->SetVoid(TargetIndex, Scratch);
					}
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, const PCGExMT::FScope& TargetScope)
	{
		if (!TargetScope.IsValid())
		{
			return;
		}

		const int32 NumAttributes = Identities.Num();

		for (int i = 0; i < NumAttributes; i++)
		{
			const FAttributeIdentity& Identity = Identities[i];
			if (!Readers.IsValidIndex(i) || !Readers[i] || !Writers[i])
			{
				continue;
			}

			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);
					TSharedPtr<TBuffer<T>> Reader = StaticCastSharedPtr<TBuffer<T>>(Readers[i]);
					TSharedPtr<TBuffer<T>> Writer = StaticCastSharedPtr<TBuffer<T>>(Writers[i]);

					const T ForwardValue = Reader->Read(SourceIndex);

					if (Writer->GetUnderlyingDomain() == EDomainType::Elements)
					{
						TSharedPtr<TArrayBuffer<T>> ElementsWriter = StaticCastSharedPtr<TArrayBuffer<T>>(Writer);
						TArray<T>& Values = *ElementsWriter->GetOutValues();
						for (int32 TargetIndex = TargetScope.Start; TargetIndex < TargetScope.End; TargetIndex++)
						{
							Values[TargetIndex] = ForwardValue;
						}
					}
					else
					{
						Writer->SetValue(0, ForwardValue);
					}
				},
				[&]()
				{
					PCGExTypes::FScopedTypedValue Scratch = Readers[i]->MakeScopedValue();
					Readers[i]->ReadVoid(SourceIndex, Scratch);
					for (int32 TargetIndex = TargetScope.Start; TargetIndex < TargetScope.End; TargetIndex++)
					{
						Writers[i]->SetVoid(TargetIndex, Scratch);
					}
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, const TSharedPtr<FFacade>& InTargetDataFacade)
	{
		if (Identities.IsEmpty())
		{
			return;
		}

		const UPCGBasePointData* InSourceData = SourceDataFacade->GetIn();

		if (Details.bPreserveAttributesDefaultValue)
		{
			for (const FAttributeIdentity& Identity : Identities)
			{
				PCGExMetaHelpers::ExecuteWithRightType(
					Identity,
					[&](auto DummyValue)
					{
						using T = decltype(DummyValue);

						const FPCGMetadataAttributeBase* SourceAtt = PCGExMetaHelpers::TryGetConstAttribute<T>(InSourceData, Identity.GetIdentifier());
						if (!SourceAtt)
						{
							return;
						}

						const T ForwardValue = Identity.InDataDomain() ? Helpers::ReadDataValue<T>(SourceAtt) : SourceAtt->GetValueFromItemKey<T>(InSourceData->GetMetadataEntry(SourceIndex));

						TSharedPtr<TBuffer<T>> Writer = nullptr;

						if (RedirectsDomain(Identity))
						{
							Writer = InTargetDataFacade->GetWritable<T>(GetTargetIdentifier(Identity), EBufferInit::New);
						}
						else
						{
							Writer = InTargetDataFacade->GetWritable<T>(SourceAtt, EBufferInit::New);
						}

						if (!Writer)
						{
							return;
						}

						if (Writer->GetUnderlyingDomain() == EDomainType::Elements)
						{
							TSharedPtr<TArrayBuffer<T>> ElementsWriter = StaticCastSharedPtr<TArrayBuffer<T>>(Writer);
							TArray<T>& Values = *ElementsWriter->GetOutValues();
							for (T& Value : Values)
							{
								Value = ForwardValue;
							}
						}
						else
						{
							Writer->SetValue(0, ForwardValue);
						}
					},
					[&]()
					{
						// Property-backed: read source value via void*, get property-backed writer, fan-out to all slots.
						const FPCGMetadataAttributeBase* SourceAtt = Identity.Attribute;
						if (!SourceAtt)
						{
							return;
						}

						const PCGMetadataEntryKey SourceKey = Identity.InDataDomain() ? PCGDefaultValueKey : InSourceData->GetMetadataEntry(SourceIndex);
						const void* SrcAddr = SourceAtt->GetReadAddressFromEntryKey_Unsafe(SourceKey);
						if (!SrcAddr)
						{
							return;
						}

						// Property-backed writers are built from the source attribute template, so the domain cannot be redirected.
						if (RedirectsDomain(Identity))
						{
							UE_LOG(LogPCGEx, Warning, TEXT("Domain conversion not supported on property-backed attribute '%s' -- skipped."), *Identity.Name.ToString());
							return;
						}

						TSharedPtr<IBuffer> Writer = InTargetDataFacade->GetWritable(Identity.GetType(), SourceAtt, EBufferInit::New);
						Helpers::PropertyBroadcastAttribute(SourceAtt, SourceKey, Writer);
					});
			}

			return;
		}

		for (const FAttributeIdentity& Identity : Identities)
		{
			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);

					const FPCGMetadataAttributeBase* SourceAtt = PCGExMetaHelpers::TryGetConstAttribute<T>(InSourceData, Identity.GetIdentifier());
					if (!SourceAtt)
					{
						return;
					}

					const T ForwardValue =
						Identity.InDataDomain()
						? Helpers::ReadDataValue<T>(SourceAtt)
						: SourceAtt->GetValueFromItemKey<T>(InSourceData->GetMetadataEntry(SourceIndex));

					const FPCGAttributeIdentifier Identifier = GetTargetIdentifier(Identity);

					InTargetDataFacade->Source->DeleteAttribute(Identifier);

					FPCGMetadataAttributeBase* TargetAtt = InTargetDataFacade->Source->FindOrCreateAttribute<T>(Identifier, ForwardValue, SourceAtt->AllowsInterpolation());

					if (Domain == EForwardDomain::ToData)
					{
						Helpers::SetDataValue(TargetAtt, ForwardValue);
					}
				},
				[&]()
				{
					// Property-backed forward: read source via void*, recreate target attribute matching source desc, copy.
					const FPCGMetadataAttributeBase* SourceAtt = Identity.Attribute;
					if (!SourceAtt)
					{
						return;
					}

					const PCGMetadataEntryKey SourceKey = Identity.InDataDomain() ? PCGDefaultValueKey : InSourceData->GetMetadataEntry(SourceIndex);
					const void* SrcAddr = SourceAtt->GetReadAddressFromEntryKey_Unsafe(SourceKey);
					if (!SrcAddr)
					{
						return;
					}

					if (RedirectsDomain(Identity))
					{
						UE_LOG(LogPCGEx, Warning, TEXT("Domain conversion not supported on property-backed attribute '%s' -- skipped."), *Identity.Name.ToString());
						return;
					}

					const FPCGAttributeIdentifier Identifier = Identity.GetIdentifier();
					InTargetDataFacade->Source->DeleteAttribute(Identifier);

					TSharedPtr<IBuffer> Writer = InTargetDataFacade->GetWritable(Identity.GetType(), SourceAtt, EBufferInit::New);
					Helpers::PropertyBroadcastAttribute(SourceAtt, SourceKey, Writer);
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, const TSharedPtr<FFacade>& InTargetDataFacade, const TArray<int32>& Indices)
	{
		if (Identities.IsEmpty())
		{
			return;
		}

		const UPCGBasePointData* InSourceData = SourceDataFacade->GetIn();

		for (const FAttributeIdentity& Identity : Identities)
		{
			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);

					const FPCGMetadataAttributeBase* SourceAtt = PCGExMetaHelpers::TryGetConstAttribute<T>(InSourceData, Identity.GetIdentifier());
					if (!SourceAtt)
					{
						return;
					}

					const T ForwardValue = Identity.InDataDomain() ? Helpers::ReadDataValue<T>(SourceAtt) : SourceAtt->GetValueFromItemKey<T>(InSourceData->GetMetadataEntry(SourceIndex));

					TSharedPtr<TBuffer<T>> Writer = nullptr;
					if (RedirectsDomain(Identity))
					{
						const T DefaultValue = Identity.InDataDomain() ? ForwardValue : SourceAtt->GetValueFromItemKey<T>(PCGDefaultValueKey);
						Writer = InTargetDataFacade->GetWritable<T>(GetTargetIdentifier(Identity), DefaultValue, SourceAtt->AllowsInterpolation(), EBufferInit::Inherit);
					}
					else
					{
						Writer = InTargetDataFacade->GetWritable<T>(SourceAtt, EBufferInit::Inherit);
					}
					if (!Writer)
					{
						return;
					}

					if (Writer->GetUnderlyingDomain() == EDomainType::Elements)
					{
						TSharedPtr<TArrayBuffer<T>> ElementsWriter = StaticCastSharedPtr<TArrayBuffer<T>>(Writer);
						TArray<T>& Values = *ElementsWriter->GetOutValues();
						for (int32 Index : Indices)
						{
							Values[Index] = ForwardValue;
						}
					}
					else
					{
						Writer->SetValue(0, ForwardValue);
					}
				},
				[&]()
				{
					// Property-backed: scatter source value to specified target indices via property reflection.
					const FPCGMetadataAttributeBase* SourceAtt = Identity.Attribute;
					if (!SourceAtt)
					{
						return;
					}

					if (RedirectsDomain(Identity))
					{
						UE_LOG(LogPCGEx, Warning, TEXT("Domain conversion not supported on property-backed attribute '%s' -- skipped."), *Identity.Name.ToString());
						return;
					}

					const PCGMetadataEntryKey SourceKey = Identity.InDataDomain() ? PCGDefaultValueKey : InSourceData->GetMetadataEntry(SourceIndex);
					const void* SrcAddr = SourceAtt->GetReadAddressFromEntryKey_Unsafe(SourceKey);
					if (!SrcAddr)
					{
						return;
					}

					TSharedPtr<IBuffer> Writer = InTargetDataFacade->GetWritable(Identity.GetType(), SourceAtt, EBufferInit::Inherit);
					if (Writer && Writer->GetUnderlyingDomain() == EDomainType::Elements)
					{
						Helpers::PropertyScatterAttribute(SourceAtt, SourceKey, Writer, Indices);
					}
					else
					{
						Helpers::PropertyBroadcastAttribute(SourceAtt, SourceKey, Writer);
					}
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, UPCGMetadata* InTargetMetadata)
	{
		if (Identities.IsEmpty())
		{
			return;
		}

		const UPCGBasePointData* InSourceData = SourceDataFacade->GetIn();

		for (const FAttributeIdentity& Identity : Identities)
		{
			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);

					const FPCGMetadataAttributeBase* SourceAtt = PCGExMetaHelpers::TryGetConstAttribute<T>(InSourceData, Identity.GetIdentifier());
					if (!SourceAtt)
					{
						return;
					}

					const T ForwardValue = Identity.InDataDomain() ? Helpers::ReadDataValue<T>(SourceAtt) : SourceAtt->GetValueFromItemKey<T>(InSourceData->GetMetadataEntry(SourceIndex));

					const FPCGAttributeIdentifier Identifier = GetTargetIdentifier(Identity);

					InTargetMetadata->DeleteAttribute(Identifier);
					FPCGMetadataAttributeBase* TargetAtt = InTargetMetadata->FindOrCreateAttribute<T>(Identifier, ForwardValue, SourceAtt->AllowsInterpolation(), true, true);
					if (Domain == EForwardDomain::ToData)
					{
						Helpers::SetDataValue(TargetAtt, ForwardValue);
					}
				},
				[&]()
				{
					// Property-backed: create matching target attribute via desc, copy via PropertyCopyAttribute helper.
					const FPCGMetadataAttributeBase* SourceAtt = Identity.Attribute;
					if (!SourceAtt || !InTargetMetadata)
					{
						return;
					}

					const FPCGAttributeIdentifier Identifier = GetTargetIdentifier(Identity);

					InTargetMetadata->DeleteAttribute(Identifier);
					FPCGMetadataAttributeBase* TargetAtt = InTargetMetadata->CreateAttribute(
						Identifier, SourceAtt->GetAttributeDesc(), SourceAtt->AllowsInterpolation(), /*bOverrideParent=*/true);
					if (!TargetAtt)
					{
						return;
					}

					const PCGMetadataEntryKey SourceKey = Identity.InDataDomain() ? PCGDefaultValueKey : InSourceData->GetMetadataEntry(SourceIndex);
					Helpers::PropertyCopyAttribute(SourceAtt, SourceKey, TargetAtt, PCGDefaultValueKey);
				});
		}
	}

	void FDataForwardHandler::Forward(const int32 SourceIndex, UPCGMetadata* InTargetMetadata, const int64 TargetKey)
	{
		if (Identities.IsEmpty())
		{
			return;
		}

		const UPCGBasePointData* InSourceData = SourceDataFacade->GetIn();

		for (const FAttributeIdentity& Identity : Identities)
		{
			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					using T = decltype(DummyValue);

					const FPCGMetadataAttributeBase* SourceAtt = PCGExMetaHelpers::TryGetConstAttribute<T>(InSourceData, Identity.GetIdentifier());
					if (!SourceAtt)
					{
						return;
					}

					const T ForwardValue = Identity.InDataDomain() ? Helpers::ReadDataValue<T>(SourceAtt) : SourceAtt->GetValueFromItemKey<T>(InSourceData->GetMetadataEntry(SourceIndex));

					// Single target entry on the element (per-row) domain. Find-or-create (not delete+create):
					// successive calls forward distinct rows into the same target metadata.
					const FPCGAttributeIdentifier TargetIdentifier(Identity.Name);
					FPCGMetadataAttribute<T>* TargetAtt = InTargetMetadata->FindOrCreateAttribute<T>(TargetIdentifier, T{}, SourceAtt->AllowsInterpolation());
					if (TargetAtt)
					{
						TargetAtt->SetValue(TargetKey, ForwardValue);
					}
				},
				[&]()
				{
					// Property-backed (Struct/Enum/Object/container) source -> deep-copy the single source value onto TargetKey.
					const FPCGMetadataAttributeBase* SourceAtt = Identity.Attribute;
					if (!SourceAtt || !InTargetMetadata)
					{
						return;
					}

					const FPCGAttributeIdentifier TargetIdentifier(Identity.Name);
					FPCGMetadataAttributeBase* TargetAtt = InTargetMetadata->GetMutableAttribute(TargetIdentifier);
					if (!TargetAtt)
					{
						TargetAtt = InTargetMetadata->CreateAttribute(TargetIdentifier, SourceAtt->GetAttributeDesc(), SourceAtt->AllowsInterpolation(), /*bOverrideParent=*/true);
					}
					if (!TargetAtt)
					{
						return;
					}

					const PCGMetadataEntryKey SourceKey = Identity.InDataDomain() ? PCGDefaultValueKey : InSourceData->GetMetadataEntry(SourceIndex);
					Helpers::PropertyCopyAttribute(SourceAtt, SourceKey, TargetAtt, TargetKey);
				});
		}
	}
}
