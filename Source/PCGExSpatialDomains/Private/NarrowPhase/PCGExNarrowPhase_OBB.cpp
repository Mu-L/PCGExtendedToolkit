// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "NarrowPhase/PCGExNarrowPhaseRegistrations.h"
#include "NarrowPhase/PCGExNarrowPhase.h"
#include "Shapes/PCGExFootprintShape.h"
#include "Math/OBB/PCGExOBB.h"
#include "Math/OBB/PCGExOBBTests.h"

namespace PCGExSpatial::NarrowPhase
{
	namespace
	{
		/**
		 * OBB-vs-OBB precise overlap test. Sphere pre-cull + SAT (the
		 * EPCGExBoxCheckMode::Box path inside PCGExMath::OBB::TestOverlap),
		 * matching the math the per-domain inner loops ran before the dispatch
		 * refactor.
		 */
		bool OBBvsOBB_Overlap(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
		{
			const auto& OBB_A = static_cast<const FPCGExFootprintShape_OBB&>(A);
			const auto& OBB_B = static_cast<const FPCGExFootprintShape_OBB&>(B);
			return PCGExMath::OBB::TestOverlap(OBB_A.Bounds, OBB_B.Bounds, EPCGExBoxCheckMode::Box);
		}

		/**
		 * OBB-vs-OBB SAT-MTV penetration depth. Returns the magnitude of the
		 * minimum translation vector required to separate the two boxes (zero
		 * when not overlapping). Forwards to the existing free function in
		 * PCGExOBBTests.h.
		 */
		float OBBvsOBB_Penetration(const FPCGExFootprintShape& A, const FPCGExFootprintShape& B)
		{
			const auto& OBB_A = static_cast<const FPCGExFootprintShape_OBB&>(A);
			const auto& OBB_B = static_cast<const FPCGExFootprintShape_OBB&>(B);
			return PCGExMath::OBB::SATPenetrationDepth(OBB_A.Bounds, OBB_B.Bounds);
		}
	}

	void RegisterOBBPairTests()
	{
		Register(
			FPCGExFootprintShape_OBB::StaticStruct(),
			FPCGExFootprintShape_OBB::StaticStruct(),
			{ &OBBvsOBB_Overlap, &OBBvsOBB_Penetration });
	}
}
