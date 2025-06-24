﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "PCGExFactoryProvider.h"
#include "Graph/PCGExCluster.h"
#include "Graph/PCGExGraph.h"
#include "PCGExOperation.h"


#include "PCGExVtxPropertyFactoryProvider.generated.h"

#define PCGEX_VTX_EXTRA_CREATE \
	NewOperation->Config = Config;

namespace PCGExVtxProperty
{
	const FName SourcePropertyLabel = TEXT("Properties");
	const FName OutputPropertyLabel = TEXT("Property");
}

USTRUCT(BlueprintType)
struct FPCGExSimpleEdgeOutputSettings
{
	GENERATED_BODY()
	virtual ~FPCGExSimpleEdgeOutputSettings() = default;

	FPCGExSimpleEdgeOutputSettings()
	{
	}

	explicit FPCGExSimpleEdgeOutputSettings(const FString& Name)
		: DirectionAttribute(FName(FText::Format(FText::FromString(TEXT("{0}Dir")), FText::FromString(Name)).ToString())),
		  LengthAttribute(FName(FText::Format(FText::FromString(TEXT("{0}Len")), FText::FromString(Name)).ToString()))
	{
	}

	/**  */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, InlineEditConditionToggle))
	bool bWriteDirection = false;

	/** Name of the attribute to output the direction to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="bWriteDirection"))
	FName DirectionAttribute = "Direction";
	TSharedPtr<PCGExData::TBuffer<FVector>> DirWriter;

	/** Invert the direction */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, DisplayName=" └─ Invert", EditCondition="bWriteDirection", HideEditConditionToggle))
	bool bInvertDirection = false;

	/**  */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, InlineEditConditionToggle))
	bool bWriteLength = false;

	/** Name of the attribute to output the length to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="bWriteLength"))
	FName LengthAttribute = "Length";
	TSharedPtr<PCGExData::TBuffer<double>> LengthWriter;

	virtual bool Validate(const FPCGContext* InContext) const
	{
		if (bWriteDirection) { PCGEX_VALIDATE_NAME_C(InContext, DirectionAttribute); }
		if (bWriteLength) { PCGEX_VALIDATE_NAME_C(InContext, LengthAttribute); }
		return true;
	}

	virtual void Init(const TSharedRef<PCGExData::FFacade>& InFacade)
	{
		if (bWriteDirection) { DirWriter = InFacade->GetWritable<FVector>(DirectionAttribute, PCGExData::EBufferInit::New); }
		if (bWriteLength) { LengthWriter = InFacade->GetWritable<double>(LengthAttribute, PCGExData::EBufferInit::New); }
	}

	void Set(const int32 EntryIndex, const double InLength, const FVector& InDir) const
	{
		if (DirWriter) { DirWriter->SetValue(EntryIndex, bInvertDirection ? InDir * -1 : InDir); }
		if (LengthWriter) { LengthWriter->SetValue(EntryIndex, InLength); }
	}

	virtual void Set(const int32 EntryIndex, const PCGExCluster::FAdjacencyData& Data) const
	{
		if (DirWriter) { DirWriter->SetValue(EntryIndex, bInvertDirection ? Data.Direction * -1 : Data.Direction); }
		if (LengthWriter) { LengthWriter->SetValue(EntryIndex, Data.Length); }
	}
};

USTRUCT(BlueprintType)
struct FPCGExEdgeOutputWithIndexSettings : public FPCGExSimpleEdgeOutputSettings
{
	GENERATED_BODY()

	FPCGExEdgeOutputWithIndexSettings()
		: FPCGExSimpleEdgeOutputSettings()
	{
	}

	explicit FPCGExEdgeOutputWithIndexSettings(const FString& Name)
		: FPCGExSimpleEdgeOutputSettings(Name),
		  EdgeIndexAttribute(FName(FText::Format(FText::FromString(TEXT("{0}EdgeIndex")), FText::FromString(Name)).ToString())),
		  VtxIndexAttribute(FName(FText::Format(FText::FromString(TEXT("{0}VtxIndex")), FText::FromString(Name)).ToString())),
		  NeighborCountAttribute(FName(FText::Format(FText::FromString(TEXT("{0}NeighborCount")), FText::FromString(Name)).ToString()))
	{
	}

	/**  */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, InlineEditConditionToggle, DisplayAfter="LengthAttribute"))
	bool bWriteEdgeIndex = false;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="bWriteEdgeIndex", DisplayAfter="bWriteEdgeIndex"))
	FName EdgeIndexAttribute = "EdgeIndex";
	TSharedPtr<PCGExData::TBuffer<int32>> EIdxWriter;

	/**  */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, InlineEditConditionToggle, DisplayAfter="EdgeIndexAttribute"))
	bool bWriteVtxIndex = false;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="bWriteVtxIndex", DisplayAfter="bWriteVtxIndex"))
	FName VtxIndexAttribute = "VtxIndex";
	TSharedPtr<PCGExData::TBuffer<int32>> VIdxWriter;

	/**  */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, InlineEditConditionToggle, DisplayAfter="VtxIndexAttribute"))
	bool bWriteNeighborCount = false;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="bWriteNeighborCount", DisplayAfter="bWriteNeighborCount"))
	FName NeighborCountAttribute = "Count";
	TSharedPtr<PCGExData::TBuffer<int32>> NCountWriter;

	virtual bool Validate(const FPCGContext* InContext) const override
	{
		if (!FPCGExSimpleEdgeOutputSettings::Validate(InContext)) { return false; }
		if (bWriteEdgeIndex) { PCGEX_VALIDATE_NAME_C(InContext, EdgeIndexAttribute); }
		if (bWriteVtxIndex) { PCGEX_VALIDATE_NAME_C(InContext, VtxIndexAttribute); }
		if (bWriteNeighborCount) { PCGEX_VALIDATE_NAME_C(InContext, NeighborCountAttribute); }
		return true;
	}

	virtual void Init(const TSharedRef<PCGExData::FFacade>& InFacade) override
	{
		FPCGExSimpleEdgeOutputSettings::Init(InFacade);
		if (bWriteEdgeIndex) { EIdxWriter = InFacade->GetWritable<int32>(EdgeIndexAttribute, PCGExData::EBufferInit::New); }
		if (bWriteVtxIndex) { VIdxWriter = InFacade->GetWritable<int32>(VtxIndexAttribute, PCGExData::EBufferInit::New); }
		if (bWriteNeighborCount) { NCountWriter = InFacade->GetWritable<int32>(NeighborCountAttribute, PCGExData::EBufferInit::New); }
	}

	void Set(const int32 EntryIndex, const double InLength, const FVector& InDir, const int32 EIndex, const int32 VIndex, const int32 NeighborCount) const
	{
		FPCGExSimpleEdgeOutputSettings::Set(EntryIndex, InLength, InDir);
		if (EIdxWriter) { EIdxWriter->SetValue(EntryIndex, EIndex); }
		if (VIdxWriter) { VIdxWriter->SetValue(EntryIndex, VIndex); }
		if (NCountWriter) { NCountWriter->SetValue(EntryIndex, NeighborCount); }
	}

	virtual void Set(const int32 EntryIndex, const PCGExCluster::FAdjacencyData& Data) const override
	{
		FPCGExSimpleEdgeOutputSettings::Set(EntryIndex, Data);
		if (EIdxWriter) { EIdxWriter->SetValue(EntryIndex, Data.EdgeIndex); }
		if (VIdxWriter) { VIdxWriter->SetValue(EntryIndex, Data.NodePointIndex); }
	}

	virtual void Set(const int32 EntryIndex, const PCGExCluster::FAdjacencyData& Data, const int32 NeighborCount)
	{
		Set(EntryIndex, Data);
		if (NCountWriter) { NCountWriter->SetValue(EntryIndex, NeighborCount); }
	}
};

/**
 * 
 */
class FPCGExVtxPropertyOperation : public FPCGExOperation
{
public:
	virtual bool PrepareForCluster(FPCGExContext* InContext, TSharedPtr<PCGExCluster::FCluster> InCluster, const TSharedPtr<PCGExData::FFacade>& InVtxDataFacade, const TSharedPtr<PCGExData::FFacade>& InEdgeDataFacade);
	virtual bool IsOperationValid();

	virtual void ProcessNode(PCGExCluster::FNode& Node, const TArray<PCGExCluster::FAdjacencyData>& Adjacency);

protected:
	const PCGExCluster::FCluster* Cluster = nullptr;
	bool bIsValidOperation = true;
};

UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Data")
class UPCGExVtxPropertyFactoryData : public UPCGExFactoryData
{
	GENERATED_BODY()

public:
	virtual PCGExFactories::EType GetFactoryType() const override { return PCGExFactories::EType::VtxProperty; }
	virtual TSharedPtr<FPCGExVtxPropertyOperation> CreateOperation(FPCGExContext* InContext) const;
};

UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|VtxProperty", meta=(PCGExNodeLibraryDoc="TBD"))
class UPCGExVtxPropertyProviderSettings : public UPCGExFactoryProviderSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(AbstractVtxProperty, "Vtx : Abstract", "Abstract vtx extra settings.")
	virtual FLinearColor GetNodeTitleColor() const override { return GetDefault<UPCGExGlobalSettings>()->NodeColorSamplerNeighbor; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	//~End UPCGSettings

public:
	virtual FName GetMainOutputPin() const override { return PCGExVtxProperty::OutputPropertyLabel; }
	virtual UPCGExFactoryData* CreateFactory(FPCGExContext* InContext, UPCGExFactoryData* InFactory) const override;

#if WITH_EDITOR
	virtual FString GetDisplayName() const override;
#endif

	/** Priority for sampling order. Higher values are processed last. */
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, DisplayPriority=-1))
	//int32 Priority = 0;
};
