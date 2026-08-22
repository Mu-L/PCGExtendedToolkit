// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "PCGExClusterSketchAuthoringSettings.generated.h"

/**
 * What the authoring GESTURES do on the user's behalf, where UPCGExClusterSketchStyleSettings is only
 * how a sketch looks. Surfaced both in Editor Preferences and on the sketch panel's Options page.
 *
 * Per-user on purpose: these are drafting habits, not a project fact. EditorPerProjectUserSettings also
 * persists through plain SaveConfig(), where a DefaultConfig class would need
 * TryUpdateDefaultConfigFile() -- which warns and returns false whenever its ini is checked in read-only,
 * so a panel toggle would appear to take and then revert on restart.
 */
UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "PCGEx | Cluster Sketch Authoring"))
class PCGEXELEMENTSCLUSTERSSKETCHEDITOR_API UPCGExClusterSketchAuthoringSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	static const UPCGExClusterSketchAuthoringSettings* Get();

	//~ Begin UDeveloperSettings
	virtual FName GetContainerName() const override { return "Editor"; }
	virtual FName GetCategoryName() const override { return "Plugins"; }
	virtual FName GetSectionName() const override { return FName("PCGEx | Cluster Sketch Authoring"); }
	//~ End UDeveloperSettings

	/** Extruding hands the new vertex the source's data record, so a drafted chain carries its authored
	 *  values instead of resolving every field to the schema default. */
	UPROPERTY(EditAnywhere, Config, Category = "Extrude")
	bool bExtrudeInheritsVertexData = true;

	/** Extruding hands the new edge the record of the source vertex's ONLY edge; a junction or a loose
	 *  end has no unambiguous parent and inherits nothing. */
	UPROPERTY(EditAnywhere, Config, Category = "Extrude")
	bool bExtrudeInheritsEdgeData = false;
};
