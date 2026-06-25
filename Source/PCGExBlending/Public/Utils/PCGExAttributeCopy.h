// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "Core/PCGExMTCommon.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataHelpers.h"
#include "Data/PCGExPointIO.h"
#include "Helpers/PCGExArrayHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"

// Shared, domain-aware single-attribute copy. This is the single source of truth for the
// source-domain x output-domain matrix (elements<->data), used by both FPCGExPointIOMerger
// (via ScopeMerge) and the union/metadata blenders' data-domain carry-over path.
//
// Blending is element-domain; a @Data attribute is a per-data singleton. Copying respects the
// output buffer's domain so a @Data value lands in the single data slot (write-once) or is
// broadcast to every element (data->elements promotion), never indexed by point index.

namespace PCGExBlending
{
	// Neutral mirror of PCGExPointIOMerger::FMergeScope so this header doesn't depend back on the merger.
	struct FAttributeCopyScope
	{
		PCGExMT::FScope Read;
		PCGExMT::FScope Write;
		bool bReverse = false;
		TArrayView<int32> ReadIndices;
	};

	// Copy InAttribute's value(s) from SourceIO into OutBuffer, honoring both domains:
	//   out=elements, in=data     -> broadcast the single value to every written element
	//   out=elements, in=elements -> range copy (optionally reversed)
	//   out=data,     in=data     -> write the single value to slot 0
	//   out=data,     in=elements -> reduce: take the first read element into slot 0
	template <typename T>
	void CopyAttributeScoped(
		const FAttributeCopyScope& Scope,
		const FPCGMetadataAttributeBase* InAttribute,
		const TSharedPtr<PCGExData::FPointIO>& SourceIO,
		const TSharedPtr<PCGExData::TBuffer<T>>& OutBuffer)
	{
		if (!InAttribute || !OutBuffer) { return; }

		const bool bSourceIsData = InAttribute->GetMetadataDomain()->GetDomainID().Flag == EPCGMetadataDomainFlag::Data;

		if (OutBuffer->GetUnderlyingDomain() == PCGExData::EDomainType::Elements)
		{
			const TSharedPtr<PCGExData::TArrayBuffer<T>> OutElementsBuffer = StaticCastSharedPtr<PCGExData::TArrayBuffer<T>>(OutBuffer);

			if (bSourceIsData)
			{
				// Data -> Elements: broadcast the single value to every written element.
				const T Value = PCGExData::Helpers::ReadDataValue<T>(InAttribute);
				for (int Index = Scope.Write.Start; Index < Scope.Write.End; Index++)
				{
					OutElementsBuffer->SetValue(Index, Value);
				}
			}
			else
			{
				check(Scope.Read.Count == Scope.Write.Count)

				TUniquePtr<const IPCGAttributeAccessor> InAccessor = PCGAttributeAccessorHelpers::CreateConstAccessor(InAttribute, InAttribute->GetMetadataDomain());
				if (!InAccessor.IsValid()) { return; }

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
		else if (OutBuffer->GetUnderlyingDomain() == PCGExData::EDomainType::Data)
		{
			const TSharedPtr<PCGExData::TSingleValueBuffer<T>> OutDataBuffer = StaticCastSharedPtr<PCGExData::TSingleValueBuffer<T>>(OutBuffer);

			if (bSourceIsData)
			{
				OutDataBuffer->SetValue(0, PCGExData::Helpers::ReadDataValue<T>(InAttribute));
			}
			else
			{
				TUniquePtr<const IPCGAttributeAccessor> InAccessor = PCGAttributeAccessorHelpers::CreateConstAccessor(InAttribute, InAttribute->GetMetadataDomain());
				if (!InAccessor.IsValid()) { return; }
				if (T Value = T{};
					InAccessor->Get(Value, Scope.Read.Start, *SourceIO->GetInKeys()))
				{
					OutDataBuffer->SetValue(0, Value);
				}
			}
		}
	}
}
