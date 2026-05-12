// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Blenders/PCGExUnionOpsManager.h"
#include "Core/PCGExMTCommon.h"
#include "Data/PCGExDataHelpers.h"
#include "Data/PCGExPointIO.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Types/PCGExAttributeIdentity.h"

#include "UObject/Object.h"

struct FPCGExCarryOverDetails;

namespace PCGExMT
{
	class FTaskManager;
}

namespace PCGExData
{
	class FPointIO;
	class FFacade;
}

namespace PCGExPointIOMerger
{
	struct PCGEXBLENDING_API FMergeScope
	{
		PCGExMT::FScope Read;
		PCGExMT::FScope Write;
		bool bReverse = false;
		TArrayView<int32> ReadIndices;

		FMergeScope() = default;
	};
}

class PCGEXBLENDING_API FPCGExPointIOMerger final : public TSharedFromThis<FPCGExPointIOMerger>
{
	friend class FPCGExAttributeMergeTask;

public:
	TArray<PCGExData::FAttributeIdentity> UniqueIdentities;
	TSharedRef<PCGExData::FFacade> UnionDataFacade;
	TArray<TSharedPtr<PCGExData::FPointIO>> IOSources;
	TArray<PCGExPointIOMerger::FMergeScope> Scopes;

	explicit FPCGExPointIOMerger(const TSharedRef<PCGExData::FFacade>& InUnionDataFacade);
	~FPCGExPointIOMerger();

	PCGExPointIOMerger::FMergeScope& Append(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExMT::FScope ReadScope, const PCGExMT::FScope WriteScope);
	PCGExPointIOMerger::FMergeScope& Append(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExMT::FScope ReadScope);
	PCGExPointIOMerger::FMergeScope& Append(const TSharedPtr<PCGExData::FPointIO>& InData);
	void Append(const TArray<TSharedPtr<PCGExData::FPointIO>>& InData);
	void MergeAsync(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, const FPCGExCarryOverDetails* InCarryOverDetails, const TSet<FName>* InIgnoredAttributes = nullptr);

	bool WantsDataToElements() const
	{
		return bDataDomainToElements;
	}

	bool WantsInitDefault() const
	{
		return bInitDefault;
	}

protected:
	void CopyProperties(const int32 Index);
	PCGExPointIOMerger::FMergeScope NullScope;
	bool bDataDomainToElements = false;
	// Merger-wide: whether output buffers should be initialized from each attribute's default value
	// (vs. a zero-init T{}). Sourced from FPCGExCarryOverDetails::bPreserveAttributesDefaultValue.
	bool bInitDefault = false;
	int32 NumCompositePoints = 0;
	EPCGPointNativeProperties AllocateProperties = EPCGPointNativeProperties::None;

	// Utils
	int32 MaxNumElements = 0;
	TArray<int32> ReverseIndices;
};

namespace PCGExPointIOMerger
{
	template <typename T>
	static void ScopeMerge(const FMergeScope& Scope, const PCGExData::FAttributeIdentity& Identity, const TSharedPtr<PCGExData::FPointIO>& SourceIO, const TSharedPtr<PCGExData::TBuffer<T>>& OutBuffer)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExPointIOMerger::ScopeMerge);

		UPCGMetadata* InMetadata = SourceIO->GetIn()->Metadata;

		const FPCGMetadataAttributeBase* TypedInAttribute = PCGExMetaHelpers::TryGetConstAttribute<T>(InMetadata, Identity.GetIdentifier());
		if (!TypedInAttribute)
		{
			return;
		}

		TSharedPtr<PCGExData::TArrayBuffer<T>> OutElementsBuffer = StaticCastSharedPtr<PCGExData::TArrayBuffer<T>>(OutBuffer);
		TSharedPtr<PCGExData::TSingleValueBuffer<T>> OutDataBuffer = StaticCastSharedPtr<PCGExData::TSingleValueBuffer<T>>(OutBuffer);

		if (OutElementsBuffer)
		{
			// We are writing to elements domain

			if (TypedInAttribute->GetMetadataDomain()->GetDomainID().Flag == EPCGMetadataDomainFlag::Data)
			{
				// From a data domain
				const T Value = PCGExData::Helpers::ReadDataValue<T>(TypedInAttribute);
				for (int Index = Scope.Write.Start; Index < Scope.Write.End; Index++)
				{
					OutElementsBuffer->SetValue(Index, Value);
				}
			}
			else
			{
				check(Scope.Read.Count == Scope.Write.Count)

				// From elements domain
				TUniquePtr<const IPCGAttributeAccessor> InAccessor = PCGAttributeAccessorHelpers::CreateConstAccessor(TypedInAttribute, TypedInAttribute->GetMetadataDomain());

				if (!InAccessor.IsValid())
				{
					return;
				}

				TArrayView<T> InRange = MakeArrayView(OutElementsBuffer->GetOutValues()->GetData() + Scope.Write.Start, Scope.Write.Count);

				if (Scope.bReverse)
				{
					TArray<T> ReadData;
					PCGExArrayHelpers::InitArray(ReadData, Scope.Write.Count);

					InAccessor->GetRange<T>(ReadData, Scope.Read.Start, *SourceIO->GetInKeys());
					for (int i = 0; i < Scope.Read.Count; i++)
					{
						InRange[i] = ReadData.Last(i);
					}
				}
				else
				{
					InAccessor->GetRange<T>(InRange, Scope.Read.Start, *SourceIO->GetInKeys());
				}
			}
		}
		else if (OutDataBuffer)
		{
			// We are writing to data domain

			if (TypedInAttribute->GetMetadataDomain()->GetDomainID().Flag == EPCGMetadataDomainFlag::Data)
			{
				// From data domain
				OutDataBuffer->SetValue(0, PCGExData::Helpers::ReadDataValue<T>(TypedInAttribute));
			}
			else
			{
				// From elements domain
				TUniquePtr<const IPCGAttributeAccessor> InAccessor = PCGAttributeAccessorHelpers::CreateConstAccessor(TypedInAttribute, TypedInAttribute->GetMetadataDomain());
				if (!InAccessor.IsValid())
				{
					return;
				}
				if (T Value = T{};
					InAccessor->Get(Value, Scope.Read.Start, *SourceIO->GetInKeys()))
				{
					OutDataBuffer->SetValue(0, Value);
				}
			}
		}
	}
}
