// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"

#include "PCGExCollectionThumbnailRenderer.generated.h"

class FCanvas;
class FRenderTarget;
class UPCGExAssetCollection;
class UTexture2D;

/**
 * Thumbnail renderer for all UPCGExAssetCollection types (registered on the base class by
 * the module). Draws an adaptive mosaic of the collection's entries in authored order:
 * 1 entry fills the tile, 2-4 entries use a 2x2 grid, 5+ a 3x3 grid. When the collection
 * holds more than 9 entries, the last cell shows the "+N" overflow count instead of a
 * thumbnail (10 entries -> "+2").
 *
 * Per-cell content resolution, cheapest-first:
 *   1. Nested subcollection (loaded) -> recursive mosaic, depth-capped at one level.
 *   2. Cached package thumbnail (ThumbnailTools) -> drawn without loading the child asset.
 *   3. Loaded child with its own thumbnail renderer -> delegated sub-rect draw.
 *   4. Placeholder tile.
 *
 * OnPropertyChange render frequency: the thumbnail pool re-renders whenever the collection
 * is edited, and the editor bakes the result into the package thumbnail on save -- so the
 * content browser shows the mosaic for unloaded assets too.
 */
UCLASS()
class PCGEXCOLLECTIONSEDITOR_API UPCGExCollectionThumbnailRenderer : public UThumbnailRenderer
{
	GENERATED_BODY()

public:
	//~ Begin UThumbnailRenderer
	virtual bool CanVisualizeAsset(UObject* Object) override;
	virtual void GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const override;
	virtual void Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* Viewport, FCanvas* Canvas, bool bAdditionalViewFamily) override;

	virtual EThumbnailRenderFrequency GetThumbnailRenderFrequency(UObject* Object) const override
	{
		return EThumbnailRenderFrequency::OnPropertyChange;
	}

	//~ End UThumbnailRenderer

protected:
	/** Nested subcollection cells recurse this many levels before falling back to a placeholder. */
	static constexpr int32 MaxRecursionDepth = 1;

	void DrawCollection(const UPCGExAssetCollection* Collection, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth);
	void DrawCell(const FSoftObjectPath& AssetPath, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth);
	void DrawPlaceholder(float X, float Y, float Width, float Height, FCanvas* Canvas, const FLinearColor& Color) const;
	void DrawOverflowCell(int32 OverflowCount, float X, float Y, float Width, float Height, FCanvas* Canvas) const;

	/**
	 * Transient texture built from an asset's cached package thumbnail (no asset load).
	 * Returns nullptr when no non-empty cached thumbnail exists. Cached per object path,
	 * invalidated when the source thumbnail's byte count changes.
	 */
	UTexture2D* GetOrBuildCachedThumbnailTexture(const FSoftObjectPath& AssetPath);

private:
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> CellTextureCache;

	/** Uncompressed byte count of the source thumbnail each cache entry was built from. */
	TMap<FString, int64> CellTextureSourceBytes;
};
