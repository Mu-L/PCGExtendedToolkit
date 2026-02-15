// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

#include "PCGExValencyEditorCommon.generated.h"

namespace PCGExValencyFolders
{
	const FName CagesFolder = FName(TEXT("Valency/Cages"));
	const FName VolumesFolder = FName(TEXT("Valency/Volumes"));
}

namespace PCGExValencyTags
{
	const FName GhostMeshTag = FName(TEXT("PCGEx_Valency_Ghost"));
}

/** Bitmask for selecting which content types to mirror from a source */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EPCGExMirrorContent : uint8
{
	None       = 0 UMETA(Hidden),
	Assets     = 1 << 0,
	Connectors = 1 << 1,
	Properties = 1 << 2,
	Tags       = 1 << 3,

	All = Assets | Connectors | Properties | Tags UMETA(Hidden)
};
ENUM_CLASS_FLAGS(EPCGExMirrorContent);

/** A mirror source entry with per-type control over what content to mirror and recurse */
USTRUCT(BlueprintType)
struct FPCGExMirrorSource
{
	GENERATED_BODY()

	/** The source cage or palette actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mirror",
		meta=(AllowedClasses="/Script/PCGExElementsValencyEditor.PCGExValencyCage, /Script/PCGExElementsValencyEditor.PCGExValencyAssetPalette"))
	TObjectPtr<AActor> Source;

	/** What content to mirror from this source */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mirror",
		meta=(Bitmask, BitmaskEnum="/Script/PCGExElementsValencyEditor.EPCGExMirrorContent"))
	uint8 MirrorFlags = static_cast<uint8>(EPCGExMirrorContent::All);

	/** Which content types to resolve recursively through nested mirror sources */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mirror",
		meta=(Bitmask, BitmaskEnum="/Script/PCGExElementsValencyEditor.EPCGExMirrorContent"))
	uint8 RecursiveFlags = static_cast<uint8>(EPCGExMirrorContent::All);

	bool IsValid() const { return Source != nullptr; }
	bool ShouldMirror(EPCGExMirrorContent Type) const { return (MirrorFlags & static_cast<uint8>(Type)) != 0; }
	bool ShouldRecurse(EPCGExMirrorContent Type) const { return (RecursiveFlags & static_cast<uint8>(Type)) != 0; }
};
