// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExGraphsEditor.h"

void FPCGExGraphsEditorModule::StartupModule()
{
	IPCGExEditorModuleInterface::StartupModule();
	// Factories and asset definitions self-register through the AssetDefinition subsystem.
}

void FPCGExGraphsEditorModule::ShutdownModule()
{
	IPCGExEditorModuleInterface::ShutdownModule();
}

PCGEX_IMPLEMENT_MODULE(FPCGExGraphsEditorModule, PCGExGraphsEditor)
