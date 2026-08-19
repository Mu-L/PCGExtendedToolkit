// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

class FPrimitiveDrawInterface;
class FPCGExSketchEditController;

/**
 * Renders a sketch edit controller's state through a bare PDI -- host-agnostic like the rest of the
 * controller seam, so the standalone viewport and any later level-viewport host draw identically.
 * Everything draws in SDPG_Foreground: this is an authoring overlay, never occluded by the scene.
 */
struct PCGEXGRAPHSEDITOR_API FPCGExSketchDrawHelper
{
	static void Draw(const FPCGExSketchEditController& Controller, FPrimitiveDrawInterface* PDI);
};
