﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "Misc/ScopeRWLock.h"
#include "UObject/ObjectPtr.h"
#include "Templates/SharedPointer.h"
#include "Templates/SharedPointerFwd.h"
#include "Curves/CurveFloat.h"
#include "Math/GenericOctree.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMeshResources.h"

#include "PCGExHelpers.h"

#include "PCGExMacros.h"

#include "PCGEx.generated.h"

#ifndef PCGEX_CONSTANTS
#define PCGEX_CONSTANTS

#define DBL_INTERSECTION_TOLERANCE 0.1
#define DBL_COLLOCATION_TOLERANCE 0.1
#define DBL_COMPARE_TOLERANCE 0.01

#endif

using PCGExTypeHash = uint32;
using InlineSparseAllocator = TSetAllocator<TSparseArrayAllocator<TInlineAllocator<8>, TInlineAllocator<8>>, TInlineAllocator<8>>;

UENUM()
enum class EPCGExOptionState : uint8
{
	Default  = 0 UMETA(DisplayName = "Default", Tooltip="Uses the default value selected in settings"),
	Enabled  = 1 UMETA(DisplayName = "Enabled", Tooltip="Option is enabled, if supported."),
	Disabled = 2 UMETA(DisplayName = "Disabled", Tooltip="Option is disabled, if supported.")
};

UENUM()
enum class EPCGExTransformMode : uint8
{
	Absolute = 0 UMETA(DisplayName = "Absolute", ToolTip="Absolute, ignores source transform."),
	Relative = 1 UMETA(DisplayName = "Relative", ToolTip="Relative to source transform."),
};

UENUM()
enum class EPCGExAttributeSetPackingMode : uint8
{
	PerInput = 0 UMETA(DisplayName = "Per Input", ToolTip="..."),
	Merged   = 1 UMETA(DisplayName = "Merged", ToolTip="..."),
};

UENUM()
enum class EPCGExWinding : uint8
{
	Clockwise        = 1 UMETA(DisplayName = "Clockwise", ToolTip="..."),
	CounterClockwise = 2 UMETA(DisplayName = "Counter Clockwise", ToolTip="..."),
};

UENUM()
enum class EPCGExWindingMutation : uint8
{
	Unchanged        = 0 UMETA(DisplayName = "Unchanged", ToolTip="..."),
	Clockwise        = 1 UMETA(DisplayName = "Clockwise", ToolTip="..."),
	CounterClockwise = 2 UMETA(DisplayName = "CounterClockwise", ToolTip="..."),
};

UENUM()
enum class EPCGExTransformComponent : uint8
{
	Position = 0 UMETA(DisplayName = "Position", ToolTip="Position component."),
	Rotation = 1 UMETA(DisplayName = "Rotation", ToolTip="Rotation component."),
	Scale    = 2 UMETA(DisplayName = "Scale", ToolTip="Scale component."),
};

UENUM()
enum class EPCGExMinimalAxis : uint8
{
	None = 0 UMETA(DisplayName = "None", ToolTip="None"),
	X    = 1 UMETA(DisplayName = "X", ToolTip="X Axis"),
	Y    = 2 UMETA(DisplayName = "Y", ToolTip="Y Axis"),
	Z    = 3 UMETA(DisplayName = "Z", ToolTip="Z Axis"),
};

UENUM()
enum class EPCGExSingleField : uint8
{
	X             = 0 UMETA(DisplayName = "X/Roll", ToolTip="X/Roll component if it exist, raw value otherwise."),
	Y             = 1 UMETA(DisplayName = "Y/Pitch", ToolTip="Y/Pitch component if it exist, fallback to previous value otherwise."),
	Z             = 2 UMETA(DisplayName = "Z/Yaw", ToolTip="Z/Yaw component if it exist, fallback to previous value otherwise."),
	W             = 3 UMETA(DisplayName = "W", ToolTip="W component if it exist, fallback to previous value otherwise."),
	Length        = 4 UMETA(DisplayName = "Length", ToolTip="Length if vector, raw value otherwise."),
	SquaredLength = 5 UMETA(DisplayName = "SquaredLength", ToolTip="SquaredLength if vector, raw value otherwise."),
	Volume        = 6 UMETA(DisplayName = "Volume", ToolTip="Volume if vector, raw value otherwise."),
	Sum           = 7 UMETA(DisplayName = "Sum", ToolTip="Sum of individual components, raw value otherwise."),
};

UENUM()
enum class EPCGExAxis : uint8
{
	Forward  = 0 UMETA(DisplayName = "Forward", ToolTip="Forward (X+)."),
	Backward = 1 UMETA(DisplayName = "Backward", ToolTip="Backward (X-)."),
	Right    = 2 UMETA(DisplayName = "Right", ToolTip="Right (Y+)"),
	Left     = 3 UMETA(DisplayName = "Left", ToolTip="Left (Y-)"),
	Up       = 4 UMETA(DisplayName = "Up", ToolTip="Up (Z+)"),
	Down     = 5 UMETA(DisplayName = "Down", ToolTip="Down (Z-)"),
};

UENUM()
enum class EPCGExAxisOrder : uint8
{
	XYZ = 0 UMETA(DisplayName = "X > Y > Z"),
	YZX = 1 UMETA(DisplayName = "Y > Z > X"),
	ZXY = 2 UMETA(DisplayName = "Z > X > Y"),
	YXZ = 3 UMETA(DisplayName = "Y > X > Z"),
	ZYX = 4 UMETA(DisplayName = "Z > Y > X"),
	XZY = 5 UMETA(DisplayName = "X > Z > Y")
};

UENUM()
enum class EPCGExAxisAlign : uint8
{
	Forward  = 0 UMETA(DisplayName = "Forward", ToolTip="..."),
	Backward = 1 UMETA(DisplayName = "Backward", ToolTip="..."),
	Right    = 2 UMETA(DisplayName = "Right", ToolTip="..."),
	Left     = 3 UMETA(DisplayName = "Left", ToolTip="..."),
	Up       = 4 UMETA(DisplayName = "Up", ToolTip="..."),
	Down     = 5 UMETA(DisplayName = "Down", ToolTip="..."),
};

UENUM()
enum class EPCGExDistance : uint8
{
	Center       = 0 UMETA(DisplayName = "Center", ToolTip="Center"),
	SphereBounds = 1 UMETA(DisplayName = "Sphere Bounds", ToolTip="Point sphere which radius is scaled extent"),
	BoxBounds    = 2 UMETA(DisplayName = "Box Bounds", ToolTip="Point extents"),
	None         = 3 UMETA(Hidden, DisplayName = "None", ToolTip="Used for union blending with full weight."),
};

UENUM()
enum class EPCGExIndexSafety : uint8
{
	Ignore = 0 UMETA(DisplayName = "Ignore", Tooltip="Out of bounds indices are ignored."),
	Tile   = 1 UMETA(DisplayName = "Tile", Tooltip="Out of bounds indices are tiled."),
	Clamp  = 2 UMETA(DisplayName = "Clamp", Tooltip="Out of bounds indices are clamped."),
	Yoyo   = 3 UMETA(DisplayName = "Yoyo", Tooltip="Out of bounds indices are mirrored and back."),
};

UENUM()
enum class EPCGExCollisionFilterType : uint8
{
	Channel    = 0 UMETA(DisplayName = "Channel", ToolTip="Channel"),
	ObjectType = 1 UMETA(DisplayName = "Object Type", ToolTip="Object Type"),
	Profile    = 2 UMETA(DisplayName = "Profile", ToolTip="Profile"),
};

UENUM()
enum class EPCGExRangeType : uint8
{
	FullRange      = 0 UMETA(DisplayName = "Full Range", ToolTip="Normalize in the [0..1] range using [0..Max Value] range."),
	EffectiveRange = 1 UMETA(DisplayName = "Effective Range", ToolTip="Remap the input [Min..Max] range to [0..1]."),
};

UENUM()
enum class EPCGExTruncateMode : uint8
{
	None  = 0 UMETA(DisplayName = "None", ToolTip="None"),
	Round = 1 UMETA(DisplayName = "Round", ToolTip="Round"),
	Ceil  = 2 UMETA(DisplayName = "Ceil", ToolTip="Ceil"),
	Floor = 3 UMETA(DisplayName = "Floor", ToolTip="Floor"),
};

namespace PCGEx
{
#if WITH_EDITOR
	const FString META_PCGExDocURL = TEXT("PCGExNodeLibraryDoc");
	const FString META_PCGExDocNodeLibraryBaseURL = TEXT("https://pcgex.gitbook.io/pcgex/node-library/");
#endif

	constexpr EPCGPointNativeProperties AllPointNativePropertiesButMeta =
		EPCGPointNativeProperties::All & ~EPCGPointNativeProperties::MetadataEntry;

	constexpr EPCGPointNativeProperties AllPointNativePropertiesButTransform =
		EPCGPointNativeProperties::All & ~EPCGPointNativeProperties::Transform;

	constexpr EPCGPointNativeProperties AllPointNativePropertiesButMetaAndTransform =
		EPCGPointNativeProperties::All & ~(EPCGPointNativeProperties::MetadataEntry | EPCGPointNativeProperties::Transform);

	const FName DEPRECATED_NAME = TEXT("#DEPRECATED");
	
	const FName PreviousAttributeName = TEXT("#Previous");
	const FName PreviousNameAttributeName = TEXT("#PreviousName");

	const FString PCGExPrefix = TEXT("PCGEx/");
	const FName SourcePointsLabel = TEXT("In");
	const FName SourceTargetsLabel = TEXT("Targets");
	const FName SourceSourcesLabel = TEXT("Sources");
	const FName SourceBoundsLabel = TEXT("Bounds");
	const FName OutputPointsLabel = TEXT("Out");

	const FName SourceAdditionalReq = TEXT("AdditionalRequirementsFilters");
	const FName SourcePerInputOverrides = TEXT("PerInputOverrides");

	const FName SourcePointFilters = TEXT("PointFilters");
	const FName SourceUseValueIfFilters = TEXT("UsableValueFilters");

	const FSoftObjectPath DefaultDotOverDistanceCurve = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExGraphBalance_DistanceOnly.FC_PCGExGraphBalance_DistanceOnly"));
	const FSoftObjectPath WeightDistributionLinearInv = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExWeightDistribution_Linear_Inv.FC_PCGExWeightDistribution_Linear_Inv"));
	const FSoftObjectPath WeightDistributionLinear = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExWeightDistribution_Linear.FC_PCGExWeightDistribution_Linear"));
	const FSoftObjectPath WeightDistributionExpoInv = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExWeightDistribution_Expo_Inv.FC_PCGExWeightDistribution_Expo_Inv"));
	const FSoftObjectPath WeightDistributionExpo = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExWeightDistribution_Expo.FC_PCGExWeightDistribution_Expo"));
	const FSoftObjectPath SteepnessWeightCurve = FSoftObjectPath(TEXT("/PCGExtendedToolkit/Curves/FC_PCGExSteepness_Default.FC_PCGExSteepness_Default"));

	const int32 AxisOrders[6][3] = {
		{0, 1, 2}, // X > Y > Z
		{1, 2, 0}, // Y > Z > X
		{2, 0, 1}, // Z > X > Y
		{1, 0, 2}, // Y > X > Z
		{2, 1, 0}, // Z > Y > X
		{0, 2, 1}  // X > Z > Y
	};

	FORCEINLINE void GetAxisOrder(EPCGExAxisOrder Order, int32 (&OutArray)[3])
	{
		const int32 Index = static_cast<uint8>(Order);
		OutArray[0] = AxisOrders[Index][0];
		OutArray[1] = AxisOrders[Index][1];
		OutArray[2] = AxisOrders[Index][2];
	}
	
	PCGEXTENDEDTOOLKIT_API
	bool IsPCGExAttribute(const FString& InStr);
	PCGEXTENDEDTOOLKIT_API
	bool IsPCGExAttribute(const FName InName);
	PCGEXTENDEDTOOLKIT_API
	bool IsPCGExAttribute(const FText& InText);

	static FName MakePCGExAttributeName(const FString& Str0) { return FName(FText::Format(FText::FromString(TEXT("{0}{1}")), FText::FromString(PCGExPrefix), FText::FromString(Str0)).ToString()); }

	static FName MakePCGExAttributeName(const FString& Str0, const FString& Str1) { return FName(FText::Format(FText::FromString(TEXT("{0}{1}/{2}")), FText::FromString(PCGExPrefix), FText::FromString(Str0), FText::FromString(Str1)).ToString()); }

	PCGEXTENDEDTOOLKIT_API
	bool IsWritableAttributeName(const FName Name);
	PCGEXTENDEDTOOLKIT_API
	FString StringTagFromName(const FName Name);
	PCGEXTENDEDTOOLKIT_API
	bool IsValidStringTag(const FString& Tag);

	PCGEXTENDEDTOOLKIT_API
	double TruncateDbl(const double Value, const EPCGExTruncateMode Mode);

	PCGEXTENDEDTOOLKIT_API
	void ArrayOfIndices(TArray<int32>& OutArray, const int32 InNum, const int32 Offset = 0);
	PCGEXTENDEDTOOLKIT_API
	int32 ArrayOfIndices(TArray<int32>& OutArray, const TArrayView<const int8>& Mask, const int32 Offset, const bool bInvert = false);
	PCGEXTENDEDTOOLKIT_API
	int32 ArrayOfIndices(TArray<int32>& OutArray, const TBitArray<>& Mask, const int32 Offset, const bool bInvert = false);

	PCGEXTENDEDTOOLKIT_API
	FName GetCompoundName(const FName A, const FName B);
	PCGEXTENDEDTOOLKIT_API
	FName GetCompoundName(const FName A, const FName B, const FName C);

	PCGEXTENDEDTOOLKIT_API
	void ScopeIndices(const TArray<int32>& InIndices, TArray<uint64>& OutScopes);

	struct PCGEXTENDEDTOOLKIT_API FOpStats
	{
		int32 Count = 0;
		double Weight = 0;
	};

	struct PCGEXTENDEDTOOLKIT_API FIndexedItem
	{
		int32 Index;
		FBoxSphereBounds Bounds;

		FIndexedItem(const int32 InIndex, const FBoxSphereBounds& InBounds)
			: Index(InIndex), Bounds(InBounds)
		{
		}
	};

	PCGEX_OCTREE_SEMANTICS_REF(FIndexedItem, { return Element.Bounds;}, { return A.Index == B.Index; })
}
