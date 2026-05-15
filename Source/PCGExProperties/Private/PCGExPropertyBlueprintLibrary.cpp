// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExPropertyBlueprintLibrary.h"

#include "PCGExProperty.h"
#include "PCGExPropertyCollectionComponent.h"
#include "Helpers/PCGPropertyHelpers.h"

namespace PCGExPropertyBlueprintLibrary_Private
{
	bool ReadInto(
		const UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		const FProperty* OutProp,
		void* OutMem)
	{
		if (!Component || !OutProp || !OutMem)
		{
			return false;
		}

		const FPCGExPropertySchema* Schema = Component->GetProperties().FindByName(PropertyName);
		if (!Schema)
		{
			return false;
		}

		const FPCGExProperty* Prop = Schema->GetProperty();
		if (!Prop)
		{
			return false;
		}

		const EPCGMetadataTypes TargetType = PCGPropertyHelpers::GetMetadataTypeFromProperty(OutProp);
		if (TargetType == EPCGMetadataTypes::Unknown)
		{
			return false;
		}

		return Prop->TryWriteValue(TargetType, OutMem);
	}

	bool WriteAndReadBack(
		UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		const FProperty* InProp,
		const void* InMem,
		const FProperty* OutProp,
		void* OutMem)
	{
		if (!Component || !InProp || !InMem || !OutProp || !OutMem)
		{
			return false;
		}

		FPCGExPropertySchema* Schema = Component->GetPropertiesMutable().FindByNameMutable(PropertyName);
		if (!Schema)
		{
			return false;
		}

		FPCGExProperty* Prop = Schema->GetPropertyMutable();
		if (!Prop)
		{
			return false;
		}

		const EPCGMetadataTypes SourceType = PCGPropertyHelpers::GetMetadataTypeFromProperty(InProp);
		if (SourceType == EPCGMetadataTypes::Unknown)
		{
			return false;
		}

		if (!Prop->TryReadValue(SourceType, InMem))
		{
			return false;
		}

		const EPCGMetadataTypes TargetType = PCGPropertyHelpers::GetMetadataTypeFromProperty(OutProp);
		if (TargetType != EPCGMetadataTypes::Unknown)
		{
			Prop->TryWriteValue(TargetType, OutMem);
		}

		return true;
	}
}

bool UPCGExPropertyBlueprintLibrary::TryGetPCGExPropertyValue(
	const UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	int32& OutValue)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UPCGExPropertyBlueprintLibrary::execTryGetPCGExPropertyValue)
{
	P_GET_OBJECT(UPCGExPropertyCollectionComponent, Component);
	P_GET_PROPERTY(FNameProperty, PropertyName);

	// Wildcard OutValue: the K2 node wires its actual pin type here at compile time.
	// Read the FProperty descriptor + destination memory off the stack manually.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* OutProp = Stack.MostRecentProperty;
	void* OutMem = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = PCGExPropertyBlueprintLibrary_Private::ReadInto(Component, PropertyName, OutProp, OutMem);
	P_NATIVE_END;
}

bool UPCGExPropertyBlueprintLibrary::TrySetPCGExPropertyValue(
	UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	const int32& NewValue,
	int32& Readback)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UPCGExPropertyBlueprintLibrary::execTrySetPCGExPropertyValue)
{
	P_GET_OBJECT(UPCGExPropertyCollectionComponent, Component);
	P_GET_PROPERTY(FNameProperty, PropertyName);

	// Wildcard NewValue (input).
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* InProp = Stack.MostRecentProperty;
	const void* InMem = Stack.MostRecentPropertyAddress;

	// Wildcard Readback (output).
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* OutProp = Stack.MostRecentProperty;
	void* OutMem = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = PCGExPropertyBlueprintLibrary_Private::WriteAndReadBack(
			Component, PropertyName, InProp, InMem, OutProp, OutMem);
	P_NATIVE_END;
}
