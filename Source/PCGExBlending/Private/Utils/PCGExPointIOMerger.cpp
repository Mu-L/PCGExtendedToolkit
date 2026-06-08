// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "Utils/PCGExPointIOMerger.h"

#include "PCGExCommon.h"
#include "Data/PCGExDataTags.h"
#include "Data/PCGExDataValue.h"
#include "Data/Utils/PCGExDataFilterDetails.h"
#include "Details/PCGExBlendingDetails.h"
#include "Types/PCGExTypeTraits.h"
#include "Utils/PCGExIntTracker.h"

namespace PCGExPointIOMerger
{
	template <typename T>
	class FWriteAttributeScopeTask final : public PCGExMT::FTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FWriteAttributeScopeTask)

		FWriteAttributeScopeTask(
			const TSharedPtr<PCGExData::FPointIO>& InPointIO,
			const FMergeScope& InScope,
			const FIdentityRef& InIdentity,
			const TSharedPtr<PCGExData::TBuffer<T>>& InOutBuffer,
			const TSharedPtr<FPCGExIntTracker>& InTracker)
			: FTask()
			  , PointIO(InPointIO)
			  , Scope(InScope)
			  , Identity(InIdentity)
			  , OutBuffer(InOutBuffer)
			  , Tracker(InTracker)
		{
		}

		const TSharedPtr<PCGExData::FPointIO> PointIO;
		const FMergeScope Scope;
		const FIdentityRef Identity;
		const TSharedPtr<PCGExData::TBuffer<T>> OutBuffer;
		TSharedPtr<FPCGExIntTracker> Tracker;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			ScopeMerge<T>(Scope, Identity, PointIO, OutBuffer);
			Tracker->IncrementCompleted();
		}
	};

#define PCGEX_TPL(_TYPE, _NAME, ...) template class FWriteAttributeScopeTask<_TYPE>;

	PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_TPL)

#undef PCGEX_TPL

	// Fills one source's disjoint output range with a single resolved value (the tag fallback used by
	// FCopyAttributeTask: one source's tag value, broadcast across that source's points).
	template <typename T>
	class FWriteConvertedTagScopeTask final : public PCGExMT::FTask
	{
	public:
		PCGEX_ASYNC_TASK_NAME(FWriteConvertedTagScopeTask)

		FWriteConvertedTagScopeTask(
			const FMergeScope& InScope,
			const T& InValue,
			const TSharedPtr<PCGExData::TBuffer<T>>& InOutBuffer,
			const TSharedPtr<FPCGExIntTracker>& InTracker)
			: FTask()
			  , Scope(InScope)
			  , Value(InValue)
			  , OutBuffer(InOutBuffer)
			  , Tracker(InTracker)
		{
		}

		const FMergeScope Scope;
		const T Value;
		const TSharedPtr<PCGExData::TBuffer<T>> OutBuffer;
		TSharedPtr<FPCGExIntTracker> Tracker;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			for (int Index = Scope.Write.Start; Index < Scope.Write.End; Index++)
			{
				OutBuffer->SetValue(Index, Value);
			}
			Tracker->IncrementCompleted();
		}
	};

	// Builds one output attribute. Each source that carries the real attribute is read normally; where
	// a source lacks it (or for a tag-only synthetic identity), a same-named tag value composites in --
	// converted to the output type via PCGExTypeOps (best-effort, no type gate). The real attribute
	// always wins the type, and any point whose source actually has it.
	class FCopyAttributeTask final : public PCGExMT::FPCGExIndexedTask
	{
	public:
		FCopyAttributeTask(
			const int32 InTaskIndex,
			const TSharedPtr<FPCGExPointIOMerger>& InMerger)
			: FPCGExIndexedTask(InTaskIndex)
			  , Merger(InMerger)
		{
		}

		TSharedPtr<FPCGExPointIOMerger> Merger;

		virtual void ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager) override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(FCopyAttributeTask::ExecuteTask);

			const FIdentityRef& Identity = Merger->UniqueIdentities[TaskIndex];

			// Tag-only synthetic identities have no backing attribute; they are fed purely from tags and
			// never consult source metadata (so a filtered-out same-named real attribute is ignored).
			const bool bTagOnly = (Identity.Attribute == nullptr);
			const TArray<TSharedPtr<PCGExData::IDataValue>>* TagValues = Merger->TagValuesByName.Find(Identity.Identifier.Name);

			PCGExMetaHelpers::ExecuteWithRightType(Identity.UnderlyingType, [&](auto DummyValue)
			{
				using T = decltype(DummyValue);

				TSharedPtr<PCGExData::TBuffer<T>> Buffer = Merger->UnionDataFacade->GetWritable(
					Merger->WantsDataToElements() ? Identity.ElementsIdentifier : Identity.Identifier,
					Identity.bInitDefault ? static_cast<const FPCGMetadataAttribute<T>*>(Identity.Attribute)->GetValue(PCGDefaultValueKey) : T{},
					Identity.bAllowsInterpolation, PCGExData::EBufferInit::New);

				if (!Buffer)
				{
					return;
				}

				for (int i = 0; i < Merger->IOSources.Num(); i++)
				{
					const TSharedPtr<PCGExData::FPointIO>& SourceIO = Merger->IOSources[i];

					// A real attribute on this source wins (its type must match the resolved type).
					if (!bTagOnly)
					{
						const FPCGMetadataAttributeBase* Attribute = SourceIO->GetIn()->Metadata->GetConstAttribute(Identity.Identifier);
						if (Attribute && Identity.IsA(Attribute->GetTypeId()))
						{
							Merger->InternalTracker->IncrementPending();
							PCGEX_LAUNCH_INTERNAL(FWriteAttributeScopeTask<T>, SourceIO, Merger->Scopes[i], Identity, Buffer, Merger->InternalTracker)
							continue;
						}
						// No usable attribute on this source -> fall through to its tag value, if any.
					}

					if (!TagValues)
					{
						continue;
					}

					const TSharedPtr<PCGExData::IDataValue>& TagValue = (*TagValues)[i];
					if (!TagValue)
					{
						continue;
					} // This source doesn't carry the tag.

					const FMergeScope& Scope = Merger->Scopes[i];
					if (Scope.Write.Count <= 0)
					{
						continue;
					}

					// No type gate -- always convert. A same-type tag is taken verbatim (so e.g. an int64
					// originator keeps full precision instead of round-tripping through double); a cross-type
					// tag uses PCGExTypeOps' own best-effort conversion. No PCG broadcastability map involved.
					T ConvertedValue{};
					if (TagValue->GetTypeId() == PCGExTypes::TTraits<T>::Type)
					{
						ConvertedValue = static_cast<PCGExData::TDataValue<T>*>(TagValue.Get())->Value;
					}
					else
					{
						ConvertedValue = TagValue->GetValue<T>();
					}

					Merger->InternalTracker->IncrementPending();
					PCGEX_LAUNCH_INTERNAL(FWriteConvertedTagScopeTask<T>, Scope, ConvertedValue, Buffer, Merger->InternalTracker)
				}
			});

			Merger->InternalTracker->IncrementCompleted();
		}
	};
}

PCGExPointIOMerger::FIdentityRef::FIdentityRef()
	: FAttributeIdentity()
{
}

PCGExPointIOMerger::FIdentityRef::FIdentityRef(const FIdentityRef& Other)
	: FAttributeIdentity(Other)
{
}

PCGExPointIOMerger::FIdentityRef::FIdentityRef(const FAttributeIdentity& Other)
	: FAttributeIdentity(Other)
{
}

PCGExPointIOMerger::FIdentityRef::FIdentityRef(const FName InName, const EPCGMetadataTypes InUnderlyingType, const bool InAllowsInterpolation)
	: FAttributeIdentity(InName, InUnderlyingType, InAllowsInterpolation)
{
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

	if (NumPoints <= 0)
	{
		return NullScope;
	}

	const PCGExMT::FScope WriteScope = PCGExMT::FScope(NumCompositePoints, NumPoints);

	return Append(InData, ReadScope, WriteScope);
}

PCGExPointIOMerger::FMergeScope& FPCGExPointIOMerger::Append(const TSharedPtr<PCGExData::FPointIO>& InData)
{
	const int32 NumPoints = InData->GetNum();

	if (NumPoints <= 0)
	{
		return NullScope;
	}

	const PCGExMT::FScope ReadScope = PCGExMT::FScope(0, NumPoints);
	const PCGExMT::FScope WriteScope = PCGExMT::FScope(NumCompositePoints, NumPoints);

	return Append(InData, ReadScope, WriteScope);
}

void FPCGExPointIOMerger::Append(const TArray<TSharedPtr<PCGExData::FPointIO>>& InData)
{
	for (const TSharedPtr<PCGExData::FPointIO>& PointIO : InData)
	{
		Append(PointIO);
	}
}

void FPCGExPointIOMerger::MergeAsync(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, const FPCGExCarryOverDetails* InCarryOverDetails, const TSet<FName>* InIgnoredAttributes, const bool bWriteUnion, const FPCGExNameFiltersDetails* InTagsToAttributes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExPointIOMerger::MergeAsync);

	bWriteFacade = bWriteUnion;
	bDataDomainToElements = InCarryOverDetails->bDataDomainToElements;

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
		PCGExData::FAttributeIdentity::ForEach(
			Metadata,
			[&](const PCGExData::FAttributeIdentity& SourceIdentity, const int32)
			{
				if (InIgnoredAttributes && InIgnoredAttributes->Contains(SourceIdentity.Identifier.Name))
				{
					return;
				}

				FString StrName = SourceIdentity.Identifier.Name.ToString();
				if (!InCarryOverDetails->Attributes.Test(StrName))
				{
					return;
				}

				const int32* ExpectedType = ExpectedTypes.Find(SourceIdentity.Identifier);
				if (!ExpectedType)
				{
					// No type expectations, we need to register a new attribute ref
					PCGExPointIOMerger::FIdentityRef& SourceRef = UniqueIdentities.Emplace_GetRef(SourceIdentity);
					SourceRef.Attribute = Metadata->GetConstAttribute(SourceIdentity.Identifier);
					SourceRef.bInitDefault = InCarryOverDetails->bPreserveAttributesDefaultValue;

					SourceRef.ElementsIdentifier.Name = SourceIdentity.Identifier.Name;
					SourceRef.ElementsIdentifier.MetadataDomain = PCGMetadataDomainID::Elements;

					ExpectedTypes.Add(SourceRef.Identifier, UniqueIdentities.Num() - 1);

					return;
				}

				// Notify type/name mismatch if needed
				if (UniqueIdentities[*ExpectedType].UnderlyingType != SourceIdentity.UnderlyingType)
				{
					PCGE_LOG_C(Warning, GraphAndLog, TaskManager->GetContext(), FText::Format(FTEXT("Mismatching attribute types for: {0}."), FText::FromName(SourceIdentity.Identifier.Name)));
				}
			});
	}

	// Build the tag-to-attribute plan (opt-in via InTagsToAttributes), now that carried attributes are
	// known. A tag passing the filter is intent: it is consumed (stripped from the output tags) whatever
	// the outcome. If its name is a carried real attribute it composites in -- filling points whose source
	// lacks the attribute, when the tag's type broadcasts to the attribute's type (incompatible tags are
	// dropped at write time). Otherwise it becomes a tag-only attribute of its own resolved type, ignoring
	// any same-named attribute the Carry-Over filter excluded.
	if (InTagsToAttributes)
	{
		// Names already owned by a carried real attribute (composite targets).
		TSet<FName> CarriedNames;
		CarriedNames.Reserve(UniqueIdentities.Num());
		for (const PCGExPointIOMerger::FIdentityRef& Identity : UniqueIdentities)
		{
			CarriedNames.Add(Identity.Identifier.Name);
		}

		// Resolved output type for tag-only names (first typed value wins; presence-only stays Boolean).
		TMap<FName, EPCGMetadataTypes> TagOnlyTypes;

		for (int i = 0; i < NumSources; i++)
		{
			const TSharedPtr<PCGExData::FPointIO>& Source = IOSources[i];

			for (const FName TagName : Source->Tags->FlattenToArrayOfNames(false))
			{
				const FString TagStr = TagName.ToString();

				// Never convert reserved PCGEx data-recognition tags (cluster pairing, etc.).
				if (TagStr.StartsWith(PCGExCommon::PCGExPrefix))
				{
					continue;
				}
				if (!InTagsToAttributes->Test(TagStr))
				{
					continue;
				}

				// Intent: this tag is consumed from the output regardless of how it resolves.
				ConvertedTagNames.Add(TagName);

				// This source's value: the typed tag value, or Boolean 'true' for a valueless (presence) tag.
				TSharedPtr<PCGExData::IDataValue> Value = Source->Tags->GetValue(TagStr);
				if (!Value)
				{
					Value = MakeShared<PCGExData::TDataValue<bool>>(true);
				}

				TArray<TSharedPtr<PCGExData::IDataValue>>& PerSource = TagValuesByName.FindOrAdd(TagName);
				if (PerSource.IsEmpty())
				{
					PerSource.SetNum(NumSources);
				}
				PerSource[i] = Value;

				// Track the tag-only output type (only used when the name isn't a carried attribute).
				if (!CarriedNames.Contains(TagName))
				{
					EPCGMetadataTypes& TagOnlyType = TagOnlyTypes.FindOrAdd(TagName, EPCGMetadataTypes::Boolean);
					if (TagOnlyType == EPCGMetadataTypes::Boolean && Value->GetTypeId() != EPCGMetadataTypes::Boolean)
					{
						TagOnlyType = Value->GetTypeId();
					}
				}
			}
		}

		// One synthetic identity per tag-only name, so the unified FCopyAttributeTask path builds it.
		for (const TPair<FName, EPCGMetadataTypes>& Pair : TagOnlyTypes)
		{
			// Integer tags originate as int64. An int64 tag may still be cast (with loss) into an
			// existing narrower int32 attribute -- but when the tag is the originating type, it must
			// not be silently narrowed to int32.
			EPCGMetadataTypes EntryType = Pair.Value;
			if (EntryType == EPCGMetadataTypes::Integer32)
			{
				EntryType = EPCGMetadataTypes::Integer64;
			}

			PCGExPointIOMerger::FIdentityRef& Entry = UniqueIdentities.Emplace_GetRef(Pair.Key, EntryType, true);
			Entry.Attribute = nullptr; // tag-only marker (no backing metadata attribute)
			Entry.bInitDefault = false;
			Entry.Identifier = FPCGAttributeIdentifier(Pair.Key, PCGMetadataDomainID::Elements);
			Entry.ElementsIdentifier = FPCGAttributeIdentifier(Pair.Key, PCGMetadataDomainID::Elements);
		}
	}

	InCarryOverDetails->Prune(&UnionDataFacade->Source.Get());

	UPCGBasePointData* OutPointData = UnionDataFacade->GetOut();
	const bool bHasAttributes = !UniqueIdentities.IsEmpty();
	if (bHasAttributes)
	{
		EnumAddFlags(AllocateProperties, EPCGPointNativeProperties::MetadataEntry);
	}

	PCGExPointArrayDataHelpers::SetNumPointsAllocated(OutPointData, NumCompositePoints, AllocateProperties);

	if (bHasAttributes)
	{
		OutPointData->SetMetadataEntry(PCGInvalidEntryKey);
	}

	PCGEX_ASYNC_GROUP_CHKD_VOID(TaskManager, CopyProperties)
	CopyProperties->OnIterationCallback = [PCGEX_ASYNC_THIS_CAPTURE](int32 Index, const PCGExMT::FScope& Scope)
	{
		PCGEX_ASYNC_THIS
		This->CopyProperties(Index);
	};

	if (bHasAttributes)
	{
		InternalTracker = MakeShared<FPCGExIntTracker>(
		[PCGEX_ASYNC_THIS_CAPTURE, TaskManager]()
		{
			PCGEX_ASYNC_THIS

			// Tags marked for conversion are consumed into attributes; drop them from the merged
			// data-domain tags so they aren't duplicated (and flattened) on the output.
			if (!This->ConvertedTagNames.IsEmpty())
			{
				This->UnionDataFacade->Source->Tags->Remove(This->ConvertedTagNames);
			}

			if (This->bWriteFacade)
			{
				This->UnionDataFacade->WriteFastest(TaskManager);
			}
		});

		CopyProperties->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE, TaskManager]()
		{
			PCGEX_ASYNC_THIS
			This->InternalTracker->IncrementPending(This->UniqueIdentities.Num());
			TaskManager->Launch(This->UniqueIdentities.Num(), [&](int32 i)
			{
				PCGEX_MAKE_SHARED(Task, PCGExPointIOMerger::FCopyAttributeTask, i, This);
				return Task;
			});
		};
	}
	else
	{
		CopyProperties->OnCompleteCallback = [PCGEX_ASYNC_THIS_CAPTURE, TaskManager]()
		{
			PCGEX_ASYNC_THIS
			if (This->bWriteFacade)
			{
				This->UnionDataFacade->WriteFastest(TaskManager);
			}
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
