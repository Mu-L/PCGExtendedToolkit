// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExClusterSketchToolkit.h"

#include "AdvancedPreviewScene.h"
#include "AssetEditorModeManager.h"
#include "Sketch/PCGExClusterSketch.h"
#include "Sketch/PCGExClusterSketchEditor.h"
#include "Sketch/PCGExClusterSketchViewportClient.h"
#include "Sketch/PCGExSketchEditController.h"

FPCGExClusterSketchToolkit::FPCGExClusterSketchToolkit(UAssetEditor* InOwningAssetEditor)
	: FBaseAssetToolkit(InOwningAssetEditor)
{
	// The base ctor already built a layout under its own generic name; rebuild under a unique one so
	// this editor's saved tab state never collides with another FBaseAssetToolkit-derived editor's.
	StandaloneDefaultLayout = FTabManager::NewLayout(FName("PCGExClusterSketchEditor_Layout_v1"))
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.75f)
					->AddTab(ViewportTabID, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
				->Split
				(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.25f)
					->AddTab(DetailsTabID, ETabState::OpenedTab)
					->SetHideTabWell(true)
				)
			)
		);

	ObjectScene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues());
}

FPCGExClusterSketchToolkit::~FPCGExClusterSketchToolkit()
{
}

void FPCGExClusterSketchToolkit::CreateWidgets()
{
	// The controller must exist before the base creates the viewport client that hosts it.
	UPCGExClusterSketch* Sketch = nullptr;
	if (const UPCGExClusterSketchEditor* SketchEditor = Cast<UPCGExClusterSketchEditor>(OwningAssetEditor))
	{
		Sketch = SketchEditor->GetSketch();
	}
	EditTarget = MakeShared<FPCGExSketchAssetEditTarget>(Sketch);
	Controller = MakeShared<FPCGExSketchEditController>(EditTarget.ToSharedRef());

	FBaseAssetToolkit::CreateWidgets();
}

void FPCGExClusterSketchToolkit::CreateEditorModeManager()
{
	EditorModeManager = MakeShared<FAssetEditorModeManager>();
	// The mode manager is the authority on what world the ITF context operates in; without this,
	// anything ITF spawns lands in the level editor world.
	StaticCastSharedPtr<FAssetEditorModeManager>(EditorModeManager)->SetPreviewScene(ObjectScene.Get());
}

TSharedPtr<FEditorViewportClient> FPCGExClusterSketchToolkit::CreateEditorViewportClient() const
{
	return MakeShared<FPCGExClusterSketchViewportClient>(EditorModeManager.Get(), ObjectScene.Get(), Controller);
}

void FPCGExClusterSketchToolkit::PostInitAssetEditor()
{
	FBaseAssetToolkit::PostInitAssetEditor();

	// The viewport tab must be live for the client (and the ITF context behind it) to tick.
	if (!TabManager->FindExistingLiveTab(ViewportTabID))
	{
		TabManager->TryInvokeTab(ViewportTabID);
	}

	// Frame the sketch (or a sane default volume for an empty one).
	FBox Bounds(ForceInit);
	if (const UPCGExClusterSketchEditor* SketchEditor = Cast<UPCGExClusterSketchEditor>(OwningAssetEditor))
	{
		if (const UPCGExClusterSketch* Sketch = SketchEditor->GetSketch())
		{
			FPCGExLatticeBasis Basis;
			const bool bHasBasis = Sketch->BuildBasis(Basis);
			for (const FPCGExClusterSketchVertex& V : Sketch->Model.Vertices)
			{
				Bounds += (V.bLatticeBound && bHasBasis) ? Basis.CoordToWorld(V.LatticeCoord) : V.Transform.GetLocation();
			}
		}
	}
	if (!Bounds.IsValid || Bounds.GetExtent().IsNearlyZero())
	{
		Bounds = FBox(FVector(-300.0), FVector(300.0));
	}
	ViewportClient->FocusViewportOnBox(Bounds.ExpandBy(Bounds.GetExtent() * 0.25));

	if (ViewportClient->Viewport)
	{
		// Focused up front, or the viewport never ticks until the user clicks inside it.
		ViewportClient->ReceivedFocus(ViewportClient->Viewport);
	}
}
