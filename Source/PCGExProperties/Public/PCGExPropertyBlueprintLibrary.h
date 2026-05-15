// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "PCGExPropertyBlueprintLibrary.generated.h"

class UPCGExPropertyCollectionComponent;

/**
 * Blueprint-callable backing for the PCGEx property-read K2 node.
 *
 * The single CustomThunk function exposes FPCGExProperty::TryWriteValue to Blueprint
 * with a wildcard output: the OutValue pin's actual type at compile time drives the
 * EPCGMetadataTypes conversion that the property's virtual TryWriteValue uses.
 *
 * The function is marked BlueprintInternalUseOnly so it doesn't surface in the
 * standard right-click palette -- UK2Node_GetPCGExProperty is the user-facing entry,
 * and it expands to a call to this function at compile time.
 */
UCLASS()
class PCGEXPROPERTIES_API UPCGExPropertyBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, CustomThunk, Category = "PCGEx|Property",
		meta = (CustomStructureParam = "OutValue", BlueprintInternalUseOnly = "true",
			DisplayName = "Try Get PCGEx Property Value"))
	static bool TryGetPCGExPropertyValue(
		const UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		int32& OutValue);

	DECLARE_FUNCTION(execTryGetPCGExPropertyValue);

	/**
	 * Try to write a value to a named property on a Property Collection Component, and
	 * read back the value the property now stores. NewValue and Readback are wildcards
	 * with the same concrete type at compile time.
	 *
	 * Useful for surfacing how the property's authored type coerced the input (e.g.,
	 * writing an int into a Vector property → Readback shows the resulting Vector).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "PCGEx|Property",
		meta = (CustomStructureParam = "NewValue,Readback", BlueprintInternalUseOnly = "true",
			DisplayName = "Try Set PCGEx Property Value"))
	static bool TrySetPCGExPropertyValue(
		UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		const int32& NewValue,
		int32& Readback);

	DECLARE_FUNCTION(execTrySetPCGExPropertyValue);
};
