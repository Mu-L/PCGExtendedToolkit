// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Elements/PCGExCollectionToModuleInfos.h"


#include "PCGGraph.h"
#include "PCGParamData.h"
#include "PCGPin.h"
#include "Helpers/PCGExMetaHelpers.h"
#include "Core/PCGExAssetCollection.h"
#include "Elements/Grammar/PCGSubdivisionBase.h"
#include "PCGExProperty.h"

#define LOCTEXT_NAMESPACE "PCGExGraphSettings"
#define PCGEX_NAMESPACE CollectionToModuleInfos

#pragma region UPCGSettings interface

TArray<FPCGPinProperties> UPCGExCollectionToModuleInfosSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExCollectionToModuleInfosSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_PARAM(FName("ModuleInfos"), TEXT("Module infos generated from the selected collection"), Normal)
	PCGEX_PIN_PARAM(PCGExCollections::Labels::OutputCollectionMapLabel, "Collection map", Normal)
	return PinProperties;
}

FPCGElementPtr UPCGExCollectionToModuleInfosSettings::CreateElement() const { return MakeShared<FPCGExCollectionToModuleInfosElement>(); }

#pragma endregion

#define PCGEX_FOREACH_MODULE_FIELD(MACRO)\
MACRO(Symbol, FName, Infos.Symbol, NAME_None)\
MACRO(Size, double, Infos.Size, 0)\
MACRO(Scalable, bool, Infos.bScalable, true)\
MACRO(DebugColor, FVector4, Infos.DebugColor, FVector4(1,1,1,1))\
MACRO(Entry, int64, Idx, 0)\
MACRO(Category, FName, Entry->Category, NAME_None)

bool FPCGExCollectionToModuleInfosElement::IsCacheable(const UPCGSettings* InSettings) const
{
	const UPCGExCollectionToModuleInfosSettings* Settings = static_cast<const UPCGExCollectionToModuleInfosSettings*>(InSettings);
	PCGEX_GET_OPTION_STATE(Settings->CacheData, bDefaultCacheNodeOutput)
}

bool FPCGExCollectionToModuleInfosElement::AdvanceWork(FPCGExContext* InContext, const UPCGExSettings* InSettings) const
{
	PCGEX_SETTINGS_C(InContext, CollectionToModuleInfos)

	PCGExHelpers::LoadBlocking_AnyThreadTpl(Settings->AssetCollection, InContext);
	UPCGExAssetCollection* MainCollection = Settings->AssetCollection.Get();

	if (!MainCollection)
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Mesh collection failed to load."));
		return true;
	}

	TSharedPtr<PCGExCollections::FPickPacker> Packer = MakeShared<PCGExCollections::FPickPacker>(InContext);
	Packer->RegisterCollection(MainCollection);

	MainCollection->EDITOR_RegisterTrackingKeys(InContext);

	UPCGParamData* OutputModules = NewObject<UPCGParamData>();
	UPCGMetadata* Metadata = OutputModules->Metadata;

#define PCGEX_DECLARE_ATT(_NAME, _TYPE, _GETTER, _DEFAULT) \
	FPCGMetadataAttribute<_TYPE>* _NAME##Attribute = nullptr; \
	PCGEX_VALIDATE_NAME(Settings->_NAME##AttributeName) _NAME##Attribute = Metadata->FindOrCreateAttribute<_TYPE>(PCGExMetaHelpers::GetAttributeIdentifier(Settings->_NAME##AttributeName, OutputModules), _DEFAULT, false, true);
	PCGEX_FOREACH_MODULE_FIELD(PCGEX_DECLARE_ATT);
#undef PCGEX_DECLARE_ATT

	TSet<FName> UniqueSymbols;
	UniqueSymbols.Reserve(100);

	TMap<const FPCGExAssetCollectionEntry*, double> SizeCache;
	SizeCache.Reserve(100);

	TArray<PCGExCollectionToGrammar::FModule> Modules;
	Modules.Reserve(100);

	FlattenCollection(Packer, MainCollection, Settings, Modules, UniqueSymbols, SizeCache);

	// Prepare custom property outputs: clone a mutable writer FInstancedStruct per configured property
	// and create its corresponding metadata attribute.
	struct FCustomPropertyWriter
	{
		FName PropertyName;
		FInstancedStruct WriterInstance;
		FPCGMetadataAttributeBase* Attribute = nullptr;
	};

	TArray<FCustomPropertyWriter> CustomWriters;
	if (Settings->PropertyOutputSettings.HasOutputs())
	{
		for (const FPCGExPropertyOutputConfig& Config : Settings->PropertyOutputSettings.Configs)
		{
			if (!Config.IsValid()) { continue; }

			const FName OutputName = Config.GetEffectiveOutputName();
			if (OutputName.IsNone()) { continue; }

			// Find a prototype: check root collection's schema first, fall back to scanning entries
			const FInstancedStruct* Prototype = MainCollection->CollectionProperties.GetPropertyByName(Config.PropertyName);
			if (!Prototype || !Prototype->IsValid())
			{
				for (const PCGExCollectionToGrammar::FModule& Module : Modules)
				{
					if (!Module.Host) { continue; }
					Prototype = Module.Host->CollectionProperties.GetPropertyByName(Config.PropertyName);
					if (Prototype && Prototype->IsValid()) { break; }
				}
			}

			if (!Prototype || !Prototype->IsValid())
			{
				PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::FromString(FString::Printf(TEXT("Property '%s' not found in collection schema."), *Config.PropertyName.ToString())));
				continue;
			}

			const FPCGExProperty* PrototypeProp = Prototype->GetPtr<FPCGExProperty>();
			if (!PrototypeProp || !PrototypeProp->SupportsOutput()) { continue; }

			FCustomPropertyWriter& Writer = CustomWriters.Emplace_GetRef();
			Writer.PropertyName = Config.PropertyName;
			Writer.WriterInstance = *Prototype;

			if (FPCGExProperty* MutableWriter = Writer.WriterInstance.GetMutablePtr<FPCGExProperty>())
			{
				Writer.Attribute = MutableWriter->CreateMetadataAttribute(Metadata, OutputName);
			}

			if (!Writer.Attribute)
			{
				CustomWriters.Pop(EAllowShrinking::No);
			}
		}
	}

	for (const PCGExCollectionToGrammar::FModule& Module : Modules)
	{
		const int64 Key = Metadata->AddEntry();
#define PCGEX_MODULE_OUT(_NAME, _TYPE, _GETTER, _DEFAULT) _NAME##Attribute->SetValue(Key, Module._GETTER);
		PCGEX_FOREACH_MODULE_FIELD(PCGEX_MODULE_OUT)
#undef PCGEX_MODULE_OUT

		for (FCustomPropertyWriter& Writer : CustomWriters)
		{
			// Resolve source: entry override first, then host collection default
			const FInstancedStruct* Source = nullptr;
			if (Module.Entry)
			{
				Source = Module.Entry->PropertyOverrides.GetOverride(Writer.PropertyName);
			}
			if ((!Source || !Source->IsValid()) && Module.Host)
			{
				Source = Module.Host->CollectionProperties.GetPropertyByName(Writer.PropertyName);
			}

			if (!Source || !Source->IsValid()) { continue; }

			const FPCGExProperty* SourceProp = Source->GetPtr<FPCGExProperty>();
			FPCGExProperty* WriterProp = Writer.WriterInstance.GetMutablePtr<FPCGExProperty>();
			if (!SourceProp || !WriterProp) { continue; }

			// Only copy when source and writer have matching concrete types
			if (Source->GetScriptStruct() != Writer.WriterInstance.GetScriptStruct()) { continue; }

			WriterProp->CopyValueFrom(SourceProp);
			WriterProp->WriteMetadataValue(Writer.Attribute, Key);
		}
	}

	FPCGTaggedData& ModulesData = InContext->OutputData.TaggedData.Emplace_GetRef();
	ModulesData.Pin = FName("ModuleInfos");
	ModulesData.Data = OutputModules;

	UPCGParamData* OutputMap = NewObject<UPCGParamData>();
	Packer->PackToDataset(OutputMap);

	FPCGTaggedData& CollectionMapData = InContext->OutputData.TaggedData.Emplace_GetRef();
	CollectionMapData.Pin = PCGExCollections::Labels::OutputCollectionMapLabel;
	CollectionMapData.Data = OutputMap;

	InContext->Done();
	return InContext->TryComplete();
}

void FPCGExCollectionToModuleInfosElement::FlattenCollection(
	const TSharedPtr<PCGExCollections::FPickPacker>& Packer,
	const UPCGExAssetCollection* Collection,
	const UPCGExCollectionToModuleInfosSettings* Settings,
	TArray<PCGExCollectionToGrammar::FModule>& OutModules,
	TSet<FName>& UniqueSymbols,
	TMap<const FPCGExAssetCollectionEntry*, double>& SizeCache) const
{
	if (!Collection) { return; }

	const PCGExAssetCollection::FCache* Cache = const_cast<UPCGExAssetCollection*>(Collection)->LoadCache();
	const int32 NumEntries = Cache->Main->Order.Num();

	const FPCGExAssetCollectionEntry* Entry = nullptr;

	for (int i = 0; i < NumEntries; i++)
	{
		FPCGExEntryAccessResult Result = Collection->GetEntryAt(i);
		Entry = Result.Entry;

		if (!Entry) { continue; }

		if (Entry->bIsSubCollection && Entry->SubGrammarMode == EPCGExGrammarSubCollectionMode::Flatten)
		{
			FlattenCollection(Packer, Entry->GetSubCollection<UPCGExAssetCollection>(), Settings, OutModules, UniqueSymbols, SizeCache);
			continue;
		}

		PCGExCollectionToGrammar::FModule& Module = OutModules.Emplace_GetRef();
		if (!Entry->FixModuleInfos(Collection, Module.Infos) || (Settings->bSkipEmptySymbol && Module.Infos.Symbol.IsNone()))
		{
			OutModules.Pop(EAllowShrinking::No);
			continue;
		}

		bool bIsAlreadyInSet = false;
		UniqueSymbols.Add(Module.Infos.Symbol, &bIsAlreadyInSet);

		if (bIsAlreadyInSet && !Settings->bAllowDuplicates)
		{
			OutModules.Pop(EAllowShrinking::No);
			continue;
		}

		Module.Entry = Entry;
		Module.Host = Result.Host;
		Module.Idx = Packer->GetPickIdx(Result.Host, Entry->Staging.InternalIndex, 0);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
#undef PCGEX_FOREACH_MODULE_FIELD
