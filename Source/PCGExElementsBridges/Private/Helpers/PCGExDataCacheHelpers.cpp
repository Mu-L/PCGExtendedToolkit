// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExDataCacheHelpers.h"

#include "PCGContext.h"
#include "PCGGraphExecutionStateInterface.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGWorldActor.h"
#include "Helpers/PCGHelpers.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

#include "Core/PCGExContext.h"
#include "Helpers/PCGExBulkAttributeHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExDataCacheHelpers"

namespace PCGExDataCache
{
	void ResolveTargetActors(FPCGExContext* InContext, const EPCGExDataCacheTarget InTarget, const FName InActorReferenceAttribute, const bool bCreateWorldActor, TArray<AActor*>& OutActors)
	{
		check(IsInGameThread());
		check(InContext);

		// Pin data wins over the enum, like the engine's Add Component target pin.
		const TArray<FPCGTaggedData> TargetInputs = InContext->InputData.GetInputsByPin(TargetActorPinLabel);
		if (!TargetInputs.IsEmpty())
		{
			TSet<AActor*> Unique;
			TArray<FSoftObjectPath> Paths;

			for (const FPCGTaggedData& TaggedData : TargetInputs)
			{
				if (!TaggedData.Data) { continue; }

				PCGExData::Helpers::BulkReadSoftPaths(TaggedData.Data, InActorReferenceAttribute, Paths);
				if (Paths.IsEmpty())
				{
					PCGE_LOG_C(Verbose, LogOnly, InContext, FText::Format(LOCTEXT("NoActorReferences", "Target actor data has no readable '{0}' attribute or no rows."), FText::FromName(InActorReferenceAttribute)));
					continue;
				}

				for (const FSoftObjectPath& Path : Paths)
				{
					UObject* Object = Path.ResolveObject();
					AActor* Actor = Cast<AActor>(Object);
					if (!Actor)
					{
						if (const UActorComponent* Component = Cast<UActorComponent>(Object)) { Actor = Component->GetOwner(); }
					}

					if (IsValid(Actor)) { Unique.Add(Actor); }
				}
			}

			OutActors.Append(Unique.Array());
			return;
		}

		IPCGGraphExecutionSource* Source = InContext->ExecutionSource.Get();
		if (!Source) { return; }

		const IPCGGraphExecutionState& State = Source->GetExecutionState();
		AActor* Actor = nullptr;

		switch (InTarget)
		{
		case EPCGExDataCacheTarget::ExecutingActor:
			Actor = InContext->GetTargetActor(nullptr);
			break;
		case EPCGExDataCacheTarget::OriginalActor:
			if (const IPCGGraphExecutionSource* Original = State.GetOriginalSource())
			{
				Actor = Original->GetExecutionState().GetTypedTarget<AActor>();
			}
			// A source with no original (non-component execution) is its own original.
			if (!Actor) { Actor = InContext->GetTargetActor(nullptr); }
			break;
		case EPCGExDataCacheTarget::WorldActor:
			Actor = bCreateWorldActor ? PCGHelpers::GetPCGWorldActor(State.GetWorld()) : PCGHelpers::FindPCGWorldActor(State.GetWorld());
			break;
		default:
			checkNoEntry();
			break;
		}

		if (IsValid(Actor)) { OutActors.Add(Actor); }
	}

	TArray<FPCGPinProperties> SanitizePins(const TArray<FPCGPinProperties>& InPins, const TArrayView<const FName> InReservedLabels)
	{
		TArray<FPCGPinProperties> Pins;
		Pins.Reserve(InPins.Num());

		TSet<FName> Seen;
		Seen.Append(InReservedLabels);

		for (const FPCGPinProperties& Pin : InPins)
		{
			if (Pin.Label.IsNone() || Seen.Contains(Pin.Label)) { continue; }
			Seen.Add(Pin.Label);
			Pins.Add(Pin);
		}

		return Pins;
	}

	bool IsTargetPinConnected(const UPCGSettings* InSettings)
	{
		const UPCGNode* Node = InSettings ? Cast<UPCGNode>(InSettings->GetOuter()) : nullptr;
		return Node && Node->IsInputPinConnected(TargetActorPinLabel);
	}
}

#undef LOCTEXT_NAMESPACE
