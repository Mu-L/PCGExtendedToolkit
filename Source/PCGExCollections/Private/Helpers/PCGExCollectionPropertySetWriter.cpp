// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExCollectionPropertySetWriter.h"

#include "Core/PCGExAssetCollection.h"
#include "Core/PCGExContext.h"
#include "Metadata/PCGMetadata.h"
#include "PCGExProperty.h"

namespace PCGExCollections
{
	const FInstancedStruct* FindPrototypeProperty(const FName PropertyName, TConstArrayView<const UPCGExAssetCollection*> Collections)
	{
		for (const UPCGExAssetCollection* Collection : Collections)
		{
			if (!Collection) { continue; }
			if (const FInstancedStruct* Found = Collection->CollectionProperties.GetPropertyByName(PropertyName);
				Found && Found->IsValid())
			{
				return Found;
			}
		}
		return nullptr;
	}

	const FInstancedStruct* ResolveEntrySourceProperty(const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Host, const FName PropertyName)
	{
		if (Entry)
		{
			if (const FInstancedStruct* Override = Entry->PropertyOverrides.GetOverride(PropertyName);
				Override && Override->IsValid())
			{
				return Override;
			}
		}
		if (Host)
		{
			if (const FInstancedStruct* Default = Host->CollectionProperties.GetPropertyByName(PropertyName);
				Default && Default->IsValid())
			{
				return Default;
			}
		}
		return nullptr;
	}

	bool FPCGExCollectionPropertySetWriter::Initialize(
		FPCGExContext* InContext,
		const FPCGExPropertyOutputSettings& OutputSettings,
		const UPCGExAssetCollection* RootCollection,
		TConstArrayView<const UPCGExAssetCollection*> FallbackHosts,
		UPCGMetadata* Metadata)
	{
		Writers.Reset();

		if (!Metadata || !OutputSettings.HasOutputs()) { return false; }

		// Build a flat search list: root + fallback hosts (skipping dupes of the root).
		TArray<const UPCGExAssetCollection*> SearchOrder;
		SearchOrder.Reserve(FallbackHosts.Num() + 1);
		if (RootCollection) { SearchOrder.Add(RootCollection); }
		for (const UPCGExAssetCollection* Host : FallbackHosts)
		{
			if (Host && Host != RootCollection) { SearchOrder.Add(Host); }
		}

		for (const FPCGExPropertyOutputConfig& Config : OutputSettings.Configs)
		{
			if (!Config.IsValid()) { continue; }

			const FName OutputName = Config.GetEffectiveOutputName();
			if (OutputName.IsNone()) { continue; }

			const FInstancedStruct* Prototype = FindPrototypeProperty(Config.PropertyName, SearchOrder);
			if (!Prototype)
			{
				PCGE_LOG_C(Warning, GraphAndLog, InContext,
					FText::FromString(FString::Printf(TEXT("Property '%s' not found in collection schema."), *Config.PropertyName.ToString())));
				continue;
			}

			const FPCGExProperty* PrototypeProp = Prototype->GetPtr<FPCGExProperty>();
			if (!PrototypeProp || !PrototypeProp->SupportsOutput()) { continue; }

			FWriter& Writer = Writers.Emplace_GetRef();
			Writer.PropertyName = Config.PropertyName;
			Writer.WriterInstance = *Prototype;

			if (FPCGExProperty* MutableWriter = Writer.WriterInstance.GetMutablePtr<FPCGExProperty>())
			{
				Writer.Attribute = MutableWriter->CreateMetadataAttribute(Metadata, OutputName);
			}

			if (!Writer.Attribute) { Writers.Pop(EAllowShrinking::No); }
		}

		return HasOutputs();
	}

	void FPCGExCollectionPropertySetWriter::WriteEntry(const int64 Key, const FPCGExAssetCollectionEntry* Entry, const UPCGExAssetCollection* Host)
	{
		if (Writers.IsEmpty()) { return; }

		for (FWriter& Writer : Writers)
		{
			const FInstancedStruct* Source = ResolveEntrySourceProperty(Entry, Host, Writer.PropertyName);
			if (!Source) { continue; }

			const FPCGExProperty* SourceProp = Source->GetPtr<FPCGExProperty>();
			FPCGExProperty* WriterProp = Writer.WriterInstance.GetMutablePtr<FPCGExProperty>();
			if (!SourceProp || !WriterProp) { continue; }

			// Only copy when source and writer have matching concrete types.
			if (Source->GetScriptStruct() != Writer.WriterInstance.GetScriptStruct()) { continue; }

			WriterProp->CopyValueFrom(SourceProp);
			WriterProp->WriteMetadataValue(Writer.Attribute, Key);
		}
	}
}
