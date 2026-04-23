// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"

namespace PCGExComponentFixups
{
	/**
	 * Register built-in post-apply fixups with PCGExActorDelta.
	 *
	 * Fixups repair engine-managed invariants that a tagged-property delta cannot
	 * express. Add new fixup functions in PCGExComponentFixups.cpp and register them
	 * here -- each lives behind an appropriate #if for the engine versions it
	 * applies to. Called once from FPCGExCollectionsModule::StartupModule.
	 */
	PCGEXCOLLECTIONS_API void RegisterBuiltins();
}
