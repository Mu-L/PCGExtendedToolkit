// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExComponentFixups.h"

#include "PCGExVersion.h"
#include "Helpers/PCGExActorPropertyDelta.h"
#include "Components/SplineComponent.h"

namespace PCGExComponentFixups
{
	namespace
	{
		/**
		 * USplineComponent invariants that the tagged-property delta can break.
		 *
		 * UE 5.7+ added a second EditAnywhere spline property (`FSpline Spline`) that
		 * aliases the legacy `FSplineCurves SplineCurves`. Authoring code keeps the two
		 * in sync, but the delta system treats them independently: if only one side was
		 * actually modified on the source, only that side is captured, and the other
		 * reads back as archetype defaults on the spawned actor. The runtime then
		 * follows whichever side `ShouldUseSplineCurves()` selects -- and if that's the
		 * unchanged side, the spline looks "reset" to BP defaults.
		 *
		 * In UE 5.6 the new `Spline` field is Transient + private and never appears in
		 * the delta; only SplineCurves is ever captured, so no sync is needed -- just
		 * the reparam-table rebuild below.
		 *
		 * Strategy (5.7+):
		 *   - Compare each property against the archetype. Whichever side diverged is
		 *     authoritative; the other gets rebuilt from it via SetSpline() (both
		 *     overloads write both sides, so one call restores consistency).
		 *   - Both diverged and match -> already consistent, nothing to do.
		 *   - Both diverged and disagree -> caller-authored inconsistency we can't
		 *     resolve without losing data; leave as-is.
		 *
		 * The reparam table is always rebuilt -- it's CPF_Transient, so the delta
		 * never includes it, and rendering/sampling depend on it being current.
		 */
		static void FixupSplineComponent(UActorComponent* Component, UObject* Archetype)
		{
			USplineComponent* Spline = Cast<USplineComponent>(Component);
			if (!Spline) { return; }

#if PCGEX_ENGINE_VERSION > 506
			if (const USplineComponent* ArchSpline = Cast<USplineComponent>(Archetype))
			{
				const bool bSplineDiverged = Spline->GetSpline() != ArchSpline->GetSpline();
				const bool bCurvesDiverged = Spline->GetSplineCurves() != ArchSpline->GetSplineCurves();

				if (bCurvesDiverged && !bSplineDiverged)
				{
					// Legacy path: SplineCurves edited, Spline at default. Rebuild Spline.
					Spline->SynchronizeSplines();
				}
				else if (bSplineDiverged && !bCurvesDiverged)
				{
					// New path: Spline edited, SplineCurves at default. Rebuild SplineCurves.
					Spline->SetSpline(Spline->GetSpline());
				}
			}
#endif

			Spline->UpdateSpline();
		}
	}

	void RegisterBuiltins()
	{
		PCGExActorDelta::RegisterPostApplyFixup(USplineComponent::StaticClass(), &FixupSplineComponent);
	}
}
