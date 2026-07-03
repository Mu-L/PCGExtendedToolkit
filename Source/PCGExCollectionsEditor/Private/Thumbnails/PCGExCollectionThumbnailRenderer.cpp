// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Thumbnails/PCGExCollectionThumbnailRenderer.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ObjectTools.h"
#include "TextureResource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Core/PCGExAssetCollection.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Misc/ObjectThumbnail.h"
#include "ThumbnailRendering/ThumbnailManager.h"

namespace PCGExCollectionThumbnail
{
	constexpr float BaseThumbnailSize = 256.f;

	// Unified dark gray shown behind the mosaic: fills the tile, the inter-cell gaps, empty
	// slots, and the overflow-count backdrop. EmptyCellColor is lifted a hair so an empty
	// slot still reads as a distinct slot rather than dissolving into the gap.
	const FLinearColor BackgroundColor = FLinearColor(0.016f, 0.016f, 0.018f);
	const FLinearColor EmptyCellColor = FLinearColor(0.024f, 0.024f, 0.027f);
	const FLinearColor DeepCollectionCellColor = FLinearColor(0.06f, 0.05f, 0.09f);

	// Resolve the FAssetData for a path without loading. Falls back to stripping the
	// "_C" suffix so actor entries (generated class paths) resolve to their Blueprint,
	// mirroring SPCGExCollectionGridTile's thumbnail lookup.
	FAssetData ResolveAssetData(const FSoftObjectPath& AssetPath)
	{
		const IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(AssetPath);

		if (!AssetData.IsValid())
		{
			FString PathString = AssetPath.ToString();
			if (PathString.EndsWith(TEXT("_C")))
			{
				PathString.LeftChopInline(2);
				AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(PathString));
			}
		}

		return AssetData;
	}
}

bool UPCGExCollectionThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
	// Empty collections keep the plain class icon instead of an empty grid.
	const UPCGExAssetCollection* Collection = Cast<UPCGExAssetCollection>(Object);
	return Collection && Collection->NumEntries() > 0;
}

void UPCGExCollectionThumbnailRenderer::GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const
{
	OutWidth = FMath::TruncToInt(Zoom * PCGExCollectionThumbnail::BaseThumbnailSize);
	OutHeight = OutWidth;
}

void UPCGExCollectionThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* Viewport, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	const UPCGExAssetCollection* Collection = Cast<UPCGExAssetCollection>(Object);
	if (!Collection || Width == 0 || Height == 0)
	{
		return;
	}

	DrawCollection(Collection, X, Y, Width, Height, Viewport, Canvas, /*Depth=*/0);
}

void UPCGExCollectionThumbnailRenderer::DrawCollection(const UPCGExAssetCollection* Collection, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth)
{
	using namespace PCGExCollectionThumbnail;

	DrawPlaceholder(X, Y, Width, Height, Canvas, BackgroundColor);

	const int32 NumEntries = Collection->NumEntries();
	if (NumEntries <= 0)
	{
		return;
	}

	// Adaptive grid: a single entry fills the tile, small collections use 2x2, larger 3x3.
	const int32 Side = NumEntries <= 1 ? 1 : NumEntries <= 4 ? 2 : 3;
	const int32 MaxCells = Side * Side;
	const bool bOverflow = NumEntries > MaxCells;
	const int32 NumEntryCells = bOverflow ? MaxCells - 1 : NumEntries;

	const float Pad = Side == 1 ? 0.f : FMath::Max(2.f, Width / 64.f);
	const float CellWidth = (Width - Pad * (Side + 1)) / Side;
	const float CellHeight = (Height - Pad * (Side + 1)) / Side;

	if (CellWidth < 1.f || CellHeight < 1.f)
	{
		return;
	}

	for (int32 CellIndex = 0; CellIndex < MaxCells; CellIndex++)
	{
		const int32 Row = CellIndex / Side;
		const int32 Col = CellIndex % Side;
		const float CellX = X + Pad + Col * (CellWidth + Pad);
		const float CellY = Y + Pad + Row * (CellHeight + Pad);

		if (CellIndex < NumEntryCells)
		{
			// Raw authored order; entries with no asset assigned keep their slot as a
			// placeholder so cell order stays truthful to the Entries array.
			const FPCGExEntryAccessResult Result = Collection->GetEntryRaw(CellIndex);
			const FSoftObjectPath CellPath = Result.IsValid() ? Result.Entry->EDITOR_GetThumbnailAssetPath() : FSoftObjectPath();
			DrawCell(CellPath, CellX, CellY, CellWidth, CellHeight, Viewport, Canvas, Depth);
		}
		else if (bOverflow && CellIndex == MaxCells - 1)
		{
			DrawOverflowCell(NumEntries - NumEntryCells, CellX, CellY, CellWidth, CellHeight, Canvas);
		}
		else
		{
			// 2x2 grid with 2-3 entries: keep unused slots as faint cells.
			DrawPlaceholder(CellX, CellY, CellWidth, CellHeight, Canvas, EmptyCellColor);
		}
	}
}

void UPCGExCollectionThumbnailRenderer::DrawCell(const FSoftObjectPath& AssetPath, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth)
{
	using namespace PCGExCollectionThumbnail;

	if (AssetPath.IsNull())
	{
		DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);
		return;
	}

	UObject* LoadedObject = AssetPath.ResolveObject();

	// Nested subcollection -> recursive mosaic. Handled explicitly (never through renderer
	// delegation below) both for the depth cap and to avoid unbounded recursion on cycles.
	if (const UPCGExAssetCollection* SubCollection = Cast<UPCGExAssetCollection>(LoadedObject))
	{
		if (Depth < MaxRecursionDepth && SubCollection->NumEntries() > 0)
		{
			DrawCollection(SubCollection, X, Y, Width, Height, Viewport, Canvas, Depth + 1);
		}
		else
		{
			DrawPlaceholder(X, Y, Width, Height, Canvas, DeepCollectionCellColor);
		}
		return;
	}

	// Cached package thumbnail -- cheap, no asset load, matches what the content browser
	// shows for unloaded assets.
	if (UTexture2D* CellTexture = GetOrBuildCachedThumbnailTexture(AssetPath))
	{
		FCanvasTileItem Tile(FVector2D(X, Y), CellTexture->GetResource(), FVector2D(Width, Height), FLinearColor::White);
		Tile.BlendMode = SE_BLEND_Opaque;
		Canvas->DrawItem(Tile);
		return;
	}

	// No cached thumbnail (e.g. freshly created, never-saved asset): delegate to the
	// child's own renderer when the asset is loaded and supports one.
	if (LoadedObject)
	{
		if (FThumbnailRenderingInfo* RenderInfo = UThumbnailManager::Get().GetRenderingInfo(LoadedObject);
			RenderInfo && RenderInfo->Renderer && RenderInfo->Renderer->CanVisualizeAsset(LoadedObject))
		{
			RenderInfo->Renderer->Draw(
				LoadedObject,
				FMath::TruncToInt(X), FMath::TruncToInt(Y),
				FMath::TruncToInt(Width), FMath::TruncToInt(Height),
				Viewport, Canvas, /*bAdditionalViewFamily=*/true);
			return;
		}
	}

	DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);
}

void UPCGExCollectionThumbnailRenderer::DrawPlaceholder(float X, float Y, float Width, float Height, FCanvas* Canvas, const FLinearColor& Color) const
{
	FCanvasTileItem Tile(FVector2D(X, Y), FVector2D(Width, Height), Color);
	Tile.BlendMode = SE_BLEND_Opaque;
	Canvas->DrawItem(Tile);
}

void UPCGExCollectionThumbnailRenderer::DrawOverflowCell(int32 OverflowCount, float X, float Y, float Width, float Height, FCanvas* Canvas) const
{
	using namespace PCGExCollectionThumbnail;

	DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);

	if (!GEngine)
	{
		return;
	}

	FCanvasTextItem TextItem(
		FVector2D(X + Width * 0.5f, Y + Height * 0.5f),
		FText::FromString(FString::Printf(TEXT("+%d"), OverflowCount)),
		GEngine->GetLargeFont(),
		FLinearColor(0.85f, 0.85f, 0.85f));

	TextItem.bCentreX = true;
	TextItem.bCentreY = true;
	TextItem.EnableShadow(FLinearColor::Black);
	TextItem.Scale = FVector2D(FMath::Max(1.f, Width / 42.f));
	Canvas->DrawItem(TextItem);
}

UTexture2D* UPCGExCollectionThumbnailRenderer::GetOrBuildCachedThumbnailTexture(const FSoftObjectPath& AssetPath)
{
	const FAssetData AssetData = PCGExCollectionThumbnail::ResolveAssetData(AssetPath);
	if (!AssetData.IsValid())
	{
		return nullptr;
	}

	// ConditionallyLoadThumbnailsForObjects checks in-memory package thumbnail maps first
	// and only falls back to a package-header read for unloaded content. OnPropertyChange
	// render frequency keeps this off any per-frame path.
	const FName ObjectFullName = FName(*AssetData.GetFullName());
	FThumbnailMap ThumbnailMap;
	if (!ThumbnailTools::ConditionallyLoadThumbnailsForObjects({ObjectFullName}, ThumbnailMap))
	{
		return nullptr;
	}

	const FObjectThumbnail* ObjectThumbnail = ThumbnailMap.Find(ObjectFullName);
	if (!ObjectThumbnail || ObjectThumbnail->IsEmpty())
	{
		return nullptr;
	}

	const int32 ImageWidth = ObjectThumbnail->GetImageWidth();
	const int32 ImageHeight = ObjectThumbnail->GetImageHeight();
	const TArray<uint8>& ImageData = ObjectThumbnail->GetUncompressedImageData();

	if (ImageWidth <= 0 || ImageHeight <= 0 || ImageData.Num() != ImageWidth * ImageHeight * 4)
	{
		return nullptr;
	}

	const FString CacheKey = AssetData.GetSoftObjectPath().ToString();

	// Reuse the cached texture unless the source thumbnail changed (byte-count heuristic:
	// re-captured thumbnails virtually never keep the exact same uncompressed size AND
	// dimensions, and a false negative only costs a texture rebuild).
	if (const TObjectPtr<UTexture2D>* Existing = CellTextureCache.Find(CacheKey))
	{
		const int64* SourceBytes = CellTextureSourceBytes.Find(CacheKey);
		if (*Existing && SourceBytes && *SourceBytes == ImageData.Num()
			&& (*Existing)->GetSizeX() == ImageWidth && (*Existing)->GetSizeY() == ImageHeight)
		{
			return *Existing;
		}
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(ImageWidth, ImageHeight, PF_B8G8R8A8);
	if (!Texture)
	{
		return nullptr;
	}

	void* MipData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, ImageData.GetData(), ImageData.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();

	Texture->SRGB = true;
	Texture->NeverStream = true;
	Texture->UpdateResource();

	CellTextureCache.Add(CacheKey, Texture);
	CellTextureSourceBytes.Add(CacheKey, ImageData.Num());

	return Texture;
}
