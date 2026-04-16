// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Utils/PCGExPointIOMerger.h"

#include "Data/PCGExDataTags.h"
#include "Data/Buffers/PCGExBufferProperty.h"
#include "Data/PCGExDataHelpers.h"
#include "Details/PCGExBlendingDetails.h"

namespace PCGExPointIOMerger
{
	template <typename T>
	class FWriteAttributeScopeTask final : public PCGExMT::FTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FWriteAttributeScopeTask)

		FWriteAttributeScopeTask(const TSharedPtr<PCGExData::FPointIO>& InPointIO, const FMergeScope& InScope, const PCGExData::FAttributeIdentity& InIdentity, const TSharedPtr<PCGExData::TBuffer<T>>& InOutBuffer)
			: FTask(), PointIO(InPointIO), Scope(InScope), Identity(InIdentity), OutBuffer(InOutBuffer)
		{
		}

		const TSharedPtr<PCGExData::FPointIO> PointIO;
		const FMergeScope Scope;
		const PCGExData::FAttributeIdentity Identity;
		const TSharedPtr<PCGExData::TBuffer<T>> OutBuffer;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			ScopeMerge<T>(Scope, Identity, PointIO, OutBuffer);
		}
	};

#define PCGEX_TPL(_TYPE, _NAME, ...) template class FWriteAttributeScopeTask<_TYPE>;

	PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_TPL)

#undef PCGEX_TPL

	// Property-backed counterpart to FWriteAttributeScopeTask<T>. Used for extended/container-typed
	// attributes that aren't covered by PCGEX_FOREACH_SUPPORTEDTYPES. Not templated — relies on
	// PropertyCopyAttributeRange to do property-aware deep copy via the target buffer's CachedInnerProperty.
	class FWriteAttributePropertyScopeTask final : public PCGExMT::FTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FWriteAttributePropertyScopeTask)

		FWriteAttributePropertyScopeTask(
			const TSharedPtr<PCGExData::FPointIO>& InPointIO,
			const FMergeScope& InScope,
			const PCGExData::FAttributeIdentity& InIdentity,
			const TSharedRef<PCGExData::FPropertyArrayBuffer>& InOutBuffer)
			: FTask(), PointIO(InPointIO), Scope(InScope), Identity(InIdentity), OutBuffer(InOutBuffer)
		{
		}

		const TSharedPtr<PCGExData::FPointIO> PointIO;
		const FMergeScope Scope;
		const PCGExData::FAttributeIdentity Identity;
		const TSharedRef<PCGExData::FPropertyArrayBuffer> OutBuffer;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FWriteAttributePropertyScopeTask::ExecuteTask);
			PCGExData::Helpers::PropertyCopyAttributeRange(PointIO, Identity, OutBuffer, Scope.Read, Scope.Write, Scope.bReverse);
		}
	};

	class FCopyAttributeTask final : public PCGExMT::FPCGExIndexedTask
	{
	public:
		FCopyAttributeTask(const int32 InTaskIndex, const TSharedPtr<FPCGExPointIOMerger>& InMerger)
			: FPCGExIndexedTask(InTaskIndex), Merger(InMerger)
		{
		}

		TSharedPtr<FPCGExPointIOMerger> Merger;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FCopyAttributeTask::ExecuteTask);

			const PCGExData::FAttributeIdentity& Identity = Merger->UniqueIdentities[TaskIndex];
			const FPCGAttributeIdentifier Identifier = Identity.GetIdentifier();
			// Merger routes data-domain attributes into Elements when WantsDataToElements() — compute that here.
			const FPCGAttributeIdentifier TargetIdentifier = Merger->WantsDataToElements()
				? FPCGAttributeIdentifier(Identity.Name, PCGMetadataDomainID::Elements)
				: Identifier;
			const bool bInitDefault = Merger->WantsInitDefault();
			const bool bAllowsInterp = Identity.GetAllowsInterpolation();

			PCGExMetaHelpers::ExecuteWithRightType(
				Identity,
				[&](auto DummyValue)
				{
					// Typed path — basic legacy types covered by PCGEX_FOREACH_SUPPORTEDTYPES.
					using T = decltype(DummyValue);

					TSharedPtr<PCGExData::TBuffer<T>> Buffer = Merger->UnionDataFacade->GetWritable(
						TargetIdentifier,
						bInitDefault && Identity.Attribute
							? Identity.Attribute->GetValueFromItemKey<T>(PCGDefaultValueKey)
							: T{},
						bAllowsInterp, PCGExData::EBufferInit::New);

					for (int i = 0; i < Merger->IOSources.Num(); i++)
					{
						TSharedPtr<PCGExData::FPointIO> SourceIO = Merger->IOSources[i];
						const FPCGMetadataAttributeBase* Attribute = SourceIO->GetIn()->Metadata->GetConstAttribute(Identifier);

						if (!Attribute) { continue; }                // Missing attribute
						if (!Attribute->IsOfType<T>()) { continue; } // Type mismatch

						PCGEX_LAUNCH_INTERNAL(FWriteAttributeScopeTask<T>, SourceIO, Merger->Scopes[i], Identity, Buffer)
					}
				},
				[&]()
				{
					// Property-backed path — Struct/Enum/Object/SoftObject/Class/SoftClass/Byte/Text + containers.
					// The facade's generic GetWritable routes unknown types to FPropertyArrayBuffer, which
					// builds CachedInnerProperty from the source attribute's desc (handles container wrapping).
					if (!Identity.Attribute)
					{
						PCGE_LOG_C(Warning, GraphAndLog, TaskManager->GetContext(), FText::Format(
							FTEXT("Cannot merge attribute '{0}' — no source attribute resolved on identity (extended/container type with null Attribute pointer)."),
							FText::FromName(Identity.Name)));
						return;
					}

					const TSharedPtr<PCGExData::IBuffer> RawBuffer = Merger->UnionDataFacade->GetWritable(
						Identity.GetType(), Identity.Attribute, PCGExData::EBufferInit::New);
					if (!RawBuffer || !RawBuffer->IsPropertyBacked())
					{
						PCGE_LOG_C(Warning, GraphAndLog, TaskManager->GetContext(), FText::Format(
							FTEXT("Cannot merge attribute '{0}' — failed to create property-backed writable buffer."),
							FText::FromName(Identity.Name)));
						return;
					}
					const TSharedPtr<PCGExData::FPropertyArrayBuffer> PropBuffer = StaticCastSharedPtr<PCGExData::FPropertyArrayBuffer>(RawBuffer);

					const TSharedRef<PCGExData::FPropertyArrayBuffer> PropBufferRef = PropBuffer.ToSharedRef();
					for (int i = 0; i < Merger->IOSources.Num(); i++)
					{
						TSharedPtr<PCGExData::FPointIO> SourceIO = Merger->IOSources[i];
						const FPCGMetadataAttributeBase* Attribute = SourceIO->GetIn()->Metadata->GetConstAttribute(Identifier);
						if (!Attribute) { continue; }
						// Desc-aware mismatch — same gate the typed path applies via IsOfType<T>.
						if (!Attribute->GetAttributeDesc().IsSameType(Identity.Attribute->GetAttributeDesc())) { continue; }

						PCGEX_LAUNCH_INTERNAL(FWriteAttributePropertyScopeTask, SourceIO, Merger->Scopes[i], Identity, PropBufferRef)
					}
				});
		}
	};
}

FPCGExPointIOMerger::FPCGExPointIOMerger(const TSharedRef<PCGExData::FFacade>& InUnionDataFacade)
	: UnionDataFacade(InUnionDataFacade)
{
}

FPCGExPointIOMerger::~FPCGExPointIOMerger()
{
}

PCGExPointIOMerger::FMergeScope& FPCGExPointIOMerger::Append(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExMT::FScope ReadScope, const PCGExMT::FScope WriteScope)
{
	const int32 NumPoints = InData->GetNum();

	check(ReadScope.IsValid());
	check(NumPoints > 0);
	check(ReadScope.End <= NumPoints);
	check(ReadScope.Count == WriteScope.Count)

	IOSources.Add(InData);

	PCGExPointIOMerger::FMergeScope& Scope = Scopes.Emplace_GetRef();
	Scope.Read = ReadScope;
	Scope.Write = WriteScope;

	MaxNumElements = FMath::Max(MaxNumElements, ReadScope.End);
	NumCompositePoints = FMath::Max(NumCompositePoints, WriteScope.End);

	EnumAddFlags(AllocateProperties, InData->GetAllocations());
	return Scope;
}

PCGExPointIOMerger::FMergeScope& FPCGExPointIOMerger::Append(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExMT::FScope ReadScope)
{
	check(InData->GetNum() >= ReadScope.Count);

	const int32 NumPoints = ReadScope.Count;

	if (NumPoints <= 0) { return NullScope; }

	const PCGExMT::FScope WriteScope = PCGExMT::FScope(NumCompositePoints, NumPoints);

	return Append(InData, ReadScope, WriteScope);
}

PCGExPointIOMerger::FMergeScope& FPCGExPointIOMerger::Append(const TSharedPtr<PCGExData::FPointIO>& InData)
{
	const int32 NumPoints = InData->GetNum();

	if (NumPoints <= 0) { return NullScope; }

	const PCGExMT::FScope ReadScope = PCGExMT::FScope(0, NumPoints);
	const PCGExMT::FScope WriteScope = PCGExMT::FScope(NumCompositePoints, NumPoints);

	return Append(InData, ReadScope, WriteScope);
}

void FPCGExPointIOMerger::Append(const TArray<TSharedPtr<PCGExData::FPointIO>>& InData)
{
	for (const TSharedPtr<PCGExData::FPointIO>& PointIO : InData) { Append(PointIO); }
}

void FPCGExPointIOMerger::MergeAsync(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, const FPCGExCarryOverDetails* InCarryOverDetails, const TSet<FName>* InIgnoredAttributes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPointIOMerger::MergeAsync);

	bDataDomainToElements = InCarryOverDetails->bDataDomainToElements;
	bInitDefault = InCarryOverDetails->bPreserveAttributesDefaultValue;

	InCarryOverDetails->Prune(&UnionDataFacade->Source.Get());
	TMap<FPCGAttributeIdentifier, int32> ExpectedTypes;

	const int32 NumSources = IOSources.Num();

	for (PCGExPointIOMerger::FMergeScope& Scope : Scopes)
	{
		if (Scope.bReverse)
		{
			if (ReverseIndices.IsEmpty())
			{
				// Create a single reverse array of indices the size of the maximum number of elements
				PCGExArrayHelpers::ArrayOfIndices(ReverseIndices, MaxNumElements);
				Algo::Reverse(ReverseIndices);
			}
			Scope.ReadIndices = MakeArrayView(ReverseIndices.GetData() + (ReverseIndices.Num() - Scope.Read.End), Scope.Read.Count);
		}
	}

	for (int i = 0; i < NumSources; i++)
	{
		const TSharedPtr<PCGExData::FPointIO> Source = IOSources[i];
		UnionDataFacade->Source->Tags->Append(Source->Tags.ToSharedRef());

		// Discover attributes
		UPCGMetadata* Metadata = Source->GetIn()->Metadata;
		PCGExData::FAttributeIdentity::ForEach(Metadata, [&](const PCGExData::FAttributeIdentity& SourceIdentity, const int32)
		{
			if (InIgnoredAttributes && InIgnoredAttributes->Contains(SourceIdentity.Name)) { return; }

			FString StrName = SourceIdentity.Name.ToString();
			if (!InCarryOverDetails->Attributes.Test(StrName)) { return; }

			const FPCGAttributeIdentifier SourceIdentifier = SourceIdentity.GetIdentifier();
			const int32* ExpectedType = ExpectedTypes.Find(SourceIdentifier);
			if (!ExpectedType)
			{
				// No type expectations, we need to register a new identity (Attribute is already cached on it).
				UniqueIdentities.Emplace(SourceIdentity);
				ExpectedTypes.Add(SourceIdentifier, UniqueIdentities.Num() - 1);
				return;
			}

			// Desc-aware mismatch: catches Struct<A> vs Struct<B>, TArray<int> vs int, etc.
			if (!UniqueIdentities[*ExpectedType].IsSameType(SourceIdentity))
			{
				PCGE_LOG_C(Warning, GraphAndLog, TaskManager->GetContext(), FText::Format(FTEXT("Mismatching attribute types for: {0}."), FText::FromName(SourceIdentity.Name)));
			}
		});
	}

	InCarryOverDetails->Prune(&UnionDataFacade->Source.Get());

	UPCGBasePointData* OutPointData = UnionDataFacade->GetOut();
	const bool bHasAttributes = !UniqueIdentities.IsEmpty();
	if (bHasAttributes) { EnumAddFlags(AllocateProperties, EPCGPointNativeProperties::MetadataEntry); }

	PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPointData, NumCompositePoints, AllocateProperties);

	if (bHasAttributes) { OutPointData->SetMetadataEntry(PCGInvalidEntryKey); }

	PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, CopyProperties)
	CopyProperties->OnIterationCallback = [PCGEX_ASYNC_THIS_CAPTURE](int32 Index, const PCGExMT::FScope& Scope)
	{
		PCGEX_ASYNC_THIS
		This->CopyProperties(Index);
	};

	if (bHasAttributes)
	{
		CopyProperties->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE, TaskManager]()
		{
			PCGEX_ASYNC_THIS
			TaskManager->Launch(This->UniqueIdentities.Num(), [&](int32 i)
			{
				PCGEX_MAKE_SHARED(Task, PCGExPointIOMerger::FCopyAttributeTask, i, This);
				return Task;
			});
		};
	}

	CopyProperties->StartIterations(NumSources, 1);
}

void FPCGExPointIOMerger::CopyProperties(const int32 Index)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPointIOMerger::CopyProperties);

	const PCGExPointIOMerger::FMergeScope& Scope = Scopes[Index];
	const TSharedPtr<PCGExData::FPointIO> Source = IOSources[Index];
	UnionDataFacade->Source->Tags->Append(Source->Tags.ToSharedRef());

	if (Scope.bReverse)
	{
		TArray<int32> TempWriteIndices;
		PCGExArrayHelpers::ArrayOfIndices(TempWriteIndices, Scope.Write.Count, Scope.Write.Start);

		Source->GetIn()->CopyPropertiesTo(UnionDataFacade->GetOut(), Scope.ReadIndices, TempWriteIndices, Source->GetAllocations() & ~EPCGPointNativeProperties::MetadataEntry);
	}
	else
	{
		Source->GetIn()->CopyPropertiesTo(UnionDataFacade->GetOut(), Scope.Read.Start, Scope.Write.Start, Scope.Write.Count, Source->GetAllocations() & ~EPCGPointNativeProperties::MetadataEntry);
	}
}
