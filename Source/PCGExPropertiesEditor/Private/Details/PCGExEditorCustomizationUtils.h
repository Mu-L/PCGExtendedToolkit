// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPropertyCollectionComponent.h"
#include "PropertyHandle.h"

namespace PCGExEditorCustomizationUtils
{
	// FStructOnScope-based external structure rows don't propagate edits up to the owning UObject
	// -- no Modify(), no PreEditChange/PostEditChange on the component. Hook the row's handle to
	// call Modify() so the edit dirties the package the way a standard inspector edit would.
	inline void HookModifyOnHandleChanged(
		const TSharedPtr<IPropertyHandle>& Handle,
		const TWeakObjectPtr<UPCGExPropertyCollectionComponent>& WeakComponent)
	{
		if (!Handle.IsValid())
		{
			return;
		}
		Handle->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([WeakComponent]()
		{
			if (UPCGExPropertyCollectionComponent* Live = WeakComponent.Get())
			{
				Live->Modify();
			}
		}));
	}
}
