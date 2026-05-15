// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/K2Node_SetPCGExProperty.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "KismetCompiler.h"
#include "PCGExPropertyBlueprintLibrary.h"
#include "PCGExPropertyCollectionComponent.h"
#include "ToolMenu.h"
#include "ToolMenus.h"
#include "Details/PCGExK2NodeTypeHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "K2Node_SetPCGExProperty"

// Pin names match the library function's parameter names so ExpandNode can look up
// the spawned UK2Node_CallFunction's pins via FindPinChecked. Display labels are set
// via PinFriendlyName in AllocateDefaultPins.
const FName UK2Node_SetPCGExProperty::ComponentPinName(TEXT("Component"));
const FName UK2Node_SetPCGExProperty::PropertyNamePinName(TEXT("PropertyName"));
const FName UK2Node_SetPCGExProperty::NewValuePinName(TEXT("NewValue"));
const FName UK2Node_SetPCGExProperty::ReadbackPinName(TEXT("Readback"));
const FName UK2Node_SetPCGExProperty::SuccessPinName(TEXT("Success"));

void UK2Node_SetPCGExProperty::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);

	UEdGraphPin* ComponentPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UPCGExPropertyCollectionComponent::StaticClass(), ComponentPinName);
	ComponentPin->PinFriendlyName = LOCTEXT("ComponentPinFriendlyName", "Collection");

	UEdGraphPin* NamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, PropertyNamePinName);
	NamePin->PinFriendlyName = LOCTEXT("PropertyNamePinFriendlyName", "Prop");

	UEdGraphPin* NewValuePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, NewValuePinName);
	NewValuePin->PinFriendlyName = LOCTEXT("NewValuePinFriendlyName", "New Value");

	UEdGraphPin* ReadbackPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Wildcard, ReadbackPinName);
	ReadbackPin->PinFriendlyName = LOCTEXT("ReadbackPinFriendlyName", "Readback");

	UEdGraphPin* SuccessPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Boolean, SuccessPinName);
	SuccessPin->PinFriendlyName = LOCTEXT("SuccessPinFriendlyName", "Success");

	Super::AllocateDefaultPins();
}

FText UK2Node_SetPCGExProperty::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("NodeTitle", "PCGEx | Set Property");
}

FText UK2Node_SetPCGExProperty::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip",
	               "Writes a value to a named property on a PCGEx Property Collection Component and "
	               "reads back the value the property now stores.\n"
	               "New Value and Readback share the same type at compile time; connecting either to "
	               "a typed pin retypes both.\n"
	               "Returns true when the write (and conversion) succeeded.");
}

FSlateIcon UK2Node_SetPCGExProperty::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Function_16x");
	return Icon;
}

FLinearColor UK2Node_SetPCGExProperty::GetNodeTitleColor() const
{
	return FLinearColor(0.0f, 0.5f, 0.8f);
}

FText UK2Node_SetPCGExProperty::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "PCGEx|Property");
}

void UK2Node_SetPCGExProperty::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

void UK2Node_SetPCGExProperty::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::PinConnectionListChanged(Pin);

	UEdGraphPin* NewValuePin = GetNewValuePin();
	UEdGraphPin* ReadbackPin = GetReadbackPin();
	if (!NewValuePin || !ReadbackPin)
	{
		return;
	}

	if (Pin != NewValuePin && Pin != ReadbackPin)
	{
		return;
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		AdoptTypeFromOther(Pin->LinkedTo[0]);
		GetGraph()->NotifyGraphChanged();
	}
	else if (NewValuePin->LinkedTo.Num() == 0 && ReadbackPin->LinkedTo.Num() == 0)
	{
		// Both wildcards fully disconnected — revert to wildcard so the node can adapt next time
		if (NewValuePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard ||
			ReadbackPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			ResetWildcardPinsToWildcard();
			GetGraph()->NotifyGraphChanged();
		}
	}
}

void UK2Node_SetPCGExProperty::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);

	if (Context->bIsDebugging || !Context->Pin)
	{
		return;
	}
	if (Context->Pin != GetNewValuePin() && Context->Pin != GetReadbackPin())
	{
		return;
	}

	FToolMenuSection& Section = Menu->AddSection(
		"K2NodeSetPCGExProperty",
		LOCTEXT("ChangeOutputType", "Change Value Type"));

	const UK2Node_SetPCGExProperty* ConstThis = this;

	for (uint8 i = 0; i < static_cast<uint8>(EPCGMetadataTypes::Count); ++i)
	{
		const EPCGMetadataTypes MetadataType = static_cast<EPCGMetadataTypes>(i);

		FEdGraphPinType PinType;
		if (!PCGExK2NodeTypeHelpers::MakePinTypeForMetadataType(MetadataType, PinType))
		{
			continue;
		}

		Section.AddMenuEntry(
			NAME_None,
			PCGExK2NodeTypeHelpers::GetDisplayNameForMetadataType(MetadataType),
			FText::GetEmpty(),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([ConstThis, PinType]()
			{
				if (UK2Node_SetPCGExProperty* MutableThis = const_cast<UK2Node_SetPCGExProperty*>(ConstThis))
				{
					MutableThis->SetWildcardPinsType(PinType);
				}
			})));
	}
}

void UK2Node_SetPCGExProperty::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	UEdGraphPin* NewValuePin = GetNewValuePin();
	UEdGraphPin* ReadbackPin = GetReadbackPin();
	if (!NewValuePin || !ReadbackPin ||
		NewValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
		ReadbackPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		CompilerContext.MessageLog.Error(
			*LOCTEXT("WildcardUnresolved",
			         "@@ has unresolved wildcard pins. Connect either New Value or Readback to a typed pin, or pick a type from either pin's right-click menu.")
			.ToString(),
			this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(UPCGExPropertyBlueprintLibrary, TrySetPCGExPropertyValue),
		UPCGExPropertyBlueprintLibrary::StaticClass());
	CallNode->AllocateDefaultPins();

	UEdGraphPin* CallExecPin = CallNode->GetExecPin();
	UEdGraphPin* CallThenPin = CallNode->GetThenPin();
	UEdGraphPin* CallComponentPin = CallNode->FindPinChecked(ComponentPinName);
	UEdGraphPin* CallNamePin = CallNode->FindPinChecked(PropertyNamePinName);
	UEdGraphPin* CallNewValuePin = CallNode->FindPinChecked(NewValuePinName);
	UEdGraphPin* CallReadbackPin = CallNode->FindPinChecked(ReadbackPinName);
	UEdGraphPin* CallReturnPin = CallNode->GetReturnValuePin();

	// Stamp both wildcards' resolved type onto the call pins before moving links so the
	// BP compiler emits the matching concrete VM instructions.
	CallNewValuePin->PinType = NewValuePin->PinType;
	CallReadbackPin->PinType = ReadbackPin->PinType;

	CompilerContext.MovePinLinksToIntermediate(*GetExecPin(), *CallExecPin);
	CompilerContext.MovePinLinksToIntermediate(*GetThenPin(), *CallThenPin);
	CompilerContext.MovePinLinksToIntermediate(*GetComponentPin(), *CallComponentPin);
	CompilerContext.MovePinLinksToIntermediate(*GetPropertyNamePin(), *CallNamePin);
	CompilerContext.MovePinLinksToIntermediate(*NewValuePin, *CallNewValuePin);
	CompilerContext.MovePinLinksToIntermediate(*ReadbackPin, *CallReadbackPin);
	CompilerContext.MovePinLinksToIntermediate(*GetSuccessPin(), *CallReturnPin);

	BreakAllNodeLinks();
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetExecPin() const
{
	return FindPin(UEdGraphSchema_K2::PN_Execute);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetThenPin() const
{
	return FindPin(UEdGraphSchema_K2::PN_Then);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetComponentPin() const
{
	return FindPin(ComponentPinName);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetPropertyNamePin() const
{
	return FindPin(PropertyNamePinName);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetNewValuePin() const
{
	return FindPin(NewValuePinName);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetReadbackPin() const
{
	return FindPin(ReadbackPinName);
}

UEdGraphPin* UK2Node_SetPCGExProperty::GetSuccessPin() const
{
	return FindPin(SuccessPinName);
}

void UK2Node_SetPCGExProperty::ResetWildcardPinsToWildcard()
{
	auto ResetOne = [](UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return;
		}
		Pin->PinType = FEdGraphPinType();
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		Pin->BreakAllPinLinks();
	};

	ResetOne(GetNewValuePin());
	ResetOne(GetReadbackPin());
}

void UK2Node_SetPCGExProperty::AdoptTypeFromOther(const UEdGraphPin* OtherPin)
{
	if (!OtherPin)
	{
		return;
	}
	FEdGraphPinType InType = OtherPin->PinType;
	InType.ContainerType = EPinContainerType::None;

	if (UEdGraphPin* NewValuePin = GetNewValuePin())
	{
		NewValuePin->PinType = InType;
	}
	if (UEdGraphPin* ReadbackPin = GetReadbackPin())
	{
		ReadbackPin->PinType = InType;
	}
}

void UK2Node_SetPCGExProperty::SetWildcardPinsType(const FEdGraphPinType& InType)
{
	auto Apply = [&InType](UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return;
		}
		for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
		{
			const UEdGraphPin* Linked = Pin->LinkedTo[i];
			if (!Linked || Linked->PinType != InType)
			{
				Pin->BreakLinkTo(Pin->LinkedTo[i]);
			}
		}
		Pin->PinType = InType;
	};

	Apply(GetNewValuePin());
	Apply(GetReadbackPin());

	GetGraph()->NotifyGraphChanged();

	if (UBlueprint* BP = GetBlueprint())
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
}

#undef LOCTEXT_NAMESPACE
