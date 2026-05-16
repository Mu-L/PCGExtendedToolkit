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

	bool WriteFrom(
		UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		const FProperty* InProp,
		const void* InMem)
	{
		if (!Component || !InProp || !InMem)
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
		return (SourceType != EPCGMetadataTypes::Unknown)
			? Prop->TryReadValue(SourceType, InMem)
			: TryReadFromObjectPin(Prop, InProp, InMem);
	}

	// Shared lookup helpers for the well-typed Object/Class accessors. These avoid the
	// CustomThunk wildcard path entirely so the BP compiler marshalling stays correct
	// for Object/Class pins (where the wildcard CustomStructureParam mechanism would
	// otherwise mis-size the frame slot and corrupt the property's FSoftObjectPath::Value).
	bool ReadSoftPath(
		const UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		EPCGMetadataTypes PathType,
		void* OutPath)
	{
		if (!Component)
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
		return Prop->TryWriteValue(PathType, OutPath);
	}

	bool WriteSoftPath(
		UPCGExPropertyCollectionComponent* Component,
		FName PropertyName,
		EPCGMetadataTypes PathType,
		const void* InPath)
	{
		if (!Component)
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
		return Prop->TryReadValue(PathType, InPath);
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
	const int32& NewValue)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UPCGExPropertyBlueprintLibrary::execTrySetPCGExPropertyValue)
{
	P_GET_OBJECT(UPCGExPropertyCollectionComponent, Component);
	P_GET_PROPERTY(FNameProperty, PropertyName);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* InProp = Stack.MostRecentProperty;
	const void* InMem = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = PCGExPropertyBlueprintLibrary_Private::WriteFrom(
			Component, PropertyName, InProp, InMem);
	P_NATIVE_END;
}

UObject* UPCGExPropertyBlueprintLibrary::TryGetPCGExPropertyObject(
	const UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	TSubclassOf<UObject> ExpectedClass,
	bool& bSuccess)
{
	bSuccess = false;

	FSoftObjectPath SoftPath;
	if (!PCGExPropertyBlueprintLibrary_Private::ReadSoftPath(
		Component, PropertyName, EPCGMetadataTypes::SoftObjectPath, &SoftPath))
	{
		return nullptr;
	}

	UObject* Resolved = SoftPath.ResolveObject();
	if (!Resolved)
	{
		Resolved = SoftPath.TryLoad();
	}
	if (!Resolved)
	{
		return nullptr;
	}
	if (*ExpectedClass && !Resolved->IsA(ExpectedClass))
	{
		return nullptr;
	}

	bSuccess = true;
	return Resolved;
}

bool UPCGExPropertyBlueprintLibrary::TrySetPCGExPropertyObject(
	UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	UObject* NewObject)
{
	const FSoftObjectPath SoftPath(NewObject);
	return PCGExPropertyBlueprintLibrary_Private::WriteSoftPath(
		Component, PropertyName, EPCGMetadataTypes::SoftObjectPath, &SoftPath);
}

TSubclassOf<UObject> UPCGExPropertyBlueprintLibrary::TryGetPCGExPropertyClass(
	const UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	TSubclassOf<UObject> ExpectedClass,
	bool& bSuccess)
{
	bSuccess = false;

	FSoftClassPath SoftPath;
	if (!PCGExPropertyBlueprintLibrary_Private::ReadSoftPath(
		Component, PropertyName, EPCGMetadataTypes::SoftClassPath, &SoftPath))
	{
		return nullptr;
	}

	UClass* Resolved = Cast<UClass>(SoftPath.ResolveObject());
	if (!Resolved)
	{
		Resolved = SoftPath.TryLoadClass<UObject>();
	}
	if (!Resolved)
	{
		return nullptr;
	}
	if (*ExpectedClass && !Resolved->IsChildOf(ExpectedClass))
	{
		return nullptr;
	}

	bSuccess = true;
	return Resolved;
}

bool UPCGExPropertyBlueprintLibrary::TrySetPCGExPropertyClass(
	UPCGExPropertyCollectionComponent* Component,
	FName PropertyName,
	UClass* NewClass)
{
	const FSoftClassPath SoftPath(NewClass);
	return PCGExPropertyBlueprintLibrary_Private::WriteSoftPath(
		Component, PropertyName, EPCGMetadataTypes::SoftClassPath, &SoftPath);
}
