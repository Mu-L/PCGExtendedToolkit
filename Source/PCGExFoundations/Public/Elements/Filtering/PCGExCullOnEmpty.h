// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGPin.h"
#include "PCGExCoreMacros.h"
#include "Core/PCGExSettings.h"

#include "PCGExCullOnEmpty.generated.h"

UCLASS(BlueprintType, ClassGroup = (Procedural), Category = "Misc")
class UPCGExCullOnEmptySettings : public UPCGExSettings
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	PCGEX_NODE_INFOS(CullOnEmpty, "Cull On Empty", "Deactivates output pin if all inputs are empty or missing.");
	virtual FLinearColor GetNodeTitleColor() const override { return PCGEX_NODE_COLOR_OPTIN_NAME(FilterHub); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Filter; }
#endif

	virtual bool OutputPinsCanBeDeactivated() const override { return true; }

protected:
	virtual bool HasDynamicPins() const override { return true; }
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGExCullOnEmptyElement final : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
