// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sketch/PCGExSketchComponentEditTarget.h"

#include "Sketch/PCGExClusterSketchComponent.h"

FPCGExSketchComponentEditTarget::FPCGExSketchComponentEditTarget(UPCGExClusterSketchComponent* InComponent)
	: Component(InComponent)
{
}

FPCGExClusterSketchModel* FPCGExSketchComponentEditTarget::GetModel()
{
	// PURE READ. TSharedPtr::operator-> is non-const even on a const pointer, so Slate attributes reach
	// this overload every prepass -- a side effect here fires on merely displaying the panel.
	UPCGExClusterSketchComponent* Pinned = Component.Get();
	return Pinned ? Pinned->GetMutableModel() : nullptr;
}

void FPCGExSketchComponentEditTarget::BeginAuthoring()
{
	IPCGExSketchEditTarget::BeginAuthoring();
	// Inline authoring chooses "no asset", which is indistinguishable from the template default until
	// it is written down.
	if (UPCGExClusterSketchComponent* Pinned = Component.Get()) { Pinned->EDITOR_RecordSketchSource(); }
}

const FPCGExClusterSketchModel* FPCGExSketchComponentEditTarget::GetModel() const
{
	const UPCGExClusterSketchComponent* Pinned = Component.Get();
	return Pinned ? &Pinned->GetModel() : nullptr;
}

const UPCGExClusterSnapProvider* FPCGExSketchComponentEditTarget::GetSnapProvider() const
{
	const UPCGExClusterSketchComponent* Pinned = Component.Get();
	return Pinned ? Pinned->GetSnapProvider() : nullptr;
}

bool FPCGExSketchComponentEditTarget::CanEdit() const
{
	// Referencing an asset makes the component read-only; the inline payload stays inspectable.
	const UPCGExClusterSketchComponent* Pinned = Component.Get();
	return Pinned && !Pinned->IsUsingAsset();
}

UObject* FPCGExSketchComponentEditTarget::GetTransactionObject()
{
	return Component.Get();
}

FTransform FPCGExSketchComponentEditTarget::GetLocalToWorld() const
{
	const UPCGExClusterSketchComponent* Pinned = Component.Get();
	return Pinned ? Pinned->GetComponentTransform() : FTransform::Identity;
}

void FPCGExSketchComponentEditTarget::NotifyChanged()
{
	if (UPCGExClusterSketchComponent* Pinned = Component.Get())
	{
		// Keeps the passive snapshot current for the moment mode suppression lifts; the mode itself
		// draws from the controller, so this is not what repaints during editing.
		Pinned->RefreshSketchVisual();
	}
}
