// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "DetailLayoutBuilder.h"
#include "Widgets/Text/STextBlock.h"

namespace PCGExCollectionEditorSlateUtils
{
	inline TSharedRef<SWidget> MakeSmallLabel(const FString& Text)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Text))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
			.MinDesiredWidth(20);
	}
}
