// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PropertyHandle.h"

namespace PCGExEditorCustomizationUtils
{
	// External-FStructOnScope rows write directly to struct memory and bypass the owner's
	// PostEditChangeProperty pipeline. Hook the row's value-change to (1) Modify() the owner
	// for dirty + transaction, then (2) broadcast OnObjectPropertyChanged directly -- the
	// signal PCG asset tracking listens to. Routing through PostEditChangeProperty instead
	// would trigger the heavy structural-rebuild path for what is just a value commit.
	//
	// Uses SetOnPropertyValueChangedWithData (not SetOnPropertyValueChanged) so we can read
	// ChangeType and skip Interactive ticks -- drag-in-progress events shouldn't tick-storm
	// every listener of the global OnObjectPropertyChanged delegate.
	inline void HookOwnerChangeOnHandleChanged(
		const TSharedPtr<IPropertyHandle>& Handle,
		const TWeakObjectPtr<UObject>& WeakOwner)
	{
		if (!Handle.IsValid())
		{
			return;
		}
		Handle->SetOnPropertyValueChangedWithData(TDelegate<void(const FPropertyChangedEvent&)>::CreateLambda(
			[WeakOwner](const FPropertyChangedEvent& InEvent)
			{
				if (InEvent.ChangeType == EPropertyChangeType::Interactive) { return; }
				UObject* Live = WeakOwner.Get();
				if (!Live) { return; }
				Live->Modify();
				FPropertyChangedEvent Event(InEvent.Property, EPropertyChangeType::ValueSet);
				FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Live, Event);
			}));
	}
}
