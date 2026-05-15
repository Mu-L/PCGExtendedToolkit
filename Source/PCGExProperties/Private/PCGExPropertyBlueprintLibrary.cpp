// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExPropertyBlueprintLibrary.h"

#include "PCGExProperty.h"
#include "PCGExPropertyCollectionComponent.h"
#include "Helpers/PCGPropertyHelpers.h"

namespace PCGExPropertyBlueprintLibrary_Private
{
	// Fallback path for Blueprint pins that PCG's helper doesn't map to an EPCGMetadataTypes
	// entry: hard object / class references. Treats the underlying property as a soft path,
	// resolves it (non-blocking when already loaded, blocking sync load otherwise) and writes
	// the typed pointer back into the pin's memory. Returns false on type mismatch.
	bool TryWriteToObjectPin(const FPCGExProperty* Prop, const FProperty* OutProp, void* OutMem)
	{
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(OutProp))
		{
			FSoftObjectPath SoftPath;
			if (!Prop->TryWriteValue(EPCGMetadataTypes::SoftObjectPath, &SoftPath))
			{
				return false;
			}
			UObject* Resolved = SoftPath.ResolveObject();
			if (!Resolved)
			{
				Resolved = SoftPath.TryLoad();
			}
			if (!Resolved || !Resolved->IsA(ObjProp->PropertyClass))
			{
				return false;
			}
			ObjProp->SetObjectPropertyValue(OutMem, Resolved);
			return true;
		}

		if (const FClassProperty* ClassProp = CastField<FClassProperty>(OutProp))
		{
			FSoftClassPath SoftPath;
			if (!Prop->TryWriteValue(EPCGMetadataTypes::SoftClassPath, &SoftPath))
			{
				return false;
			}
			UClass* Resolved = Cast<UClass>(SoftPath.ResolveObject());
			if (!Resolved)
			{
				Resolved = SoftPath.TryLoadClass<UObject>();
			}
			if (!Resolved || !Resolved->IsChildOf(ClassProp->MetaClass))
			{
				return false;
			}
			ClassProp->SetObjectPropertyValue(OutMem, Resolved);
			return true;
		}

		return false;
	}

	// Fallback path for the inverse direction: a hard object/class ref input is read as a
	// soft path and forwarded to the property's TryReadValue.
	bool TryReadFromObjectPin(FPCGExProperty* Prop, const FProperty* InProp, const void* InMem)
	{
		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(InProp))
		{
			UObject* InObj = ObjProp->GetObjectPropertyValue(const_cast<void*>(InMem));
			FSoftObjectPath SoftPath(InObj);
			return Prop->TryReadValue(EPCGMetadataTypes::SoftObjectPath, &SoftPath);
		}

		if (const FClassProperty* ClassProp = CastField<FClassProperty>(InProp))
		{
			UClass* InClass = Cast<UClass>(ClassProp->GetObjectPropertyValue(const_cast<void*>(InMem)));
			FSoftClassPath SoftPath(InClass);
			return Prop->TryReadValue(EPCGMetadataTypes::SoftClassPath, &SoftPath);
		}

		return false;
	}

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
			return TryWriteToObjectPin(Prop, OutProp, OutMem);
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
		const bool bWriteOk = (SourceType != EPCGMetadataTypes::Unknown)
			? Prop->TryReadValue(SourceType, InMem)
			: TryReadFromObjectPin(Prop, InProp, InMem);
		if (!bWriteOk)
		{
			return false;
		}

		const EPCGMetadataTypes TargetType = PCGPropertyHelpers::GetMetadataTypeFromProperty(OutProp);
		if (TargetType != EPCGMetadataTypes::Unknown)
		{
			Prop->TryWriteValue(TargetType, OutMem);
		}
		else
		{
			TryWriteToObjectPin(Prop, OutProp, OutMem);
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
