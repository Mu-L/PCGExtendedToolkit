// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Growth/Constraints/PCGExConstraint_ContextCondition.h"

#pragma region FPCGExConstraint_ContextCondition

bool FPCGExConstraint_ContextCondition::IsValid(
	const FPCGExConstraintContext& Context,
	const FTransform& CandidateTransform) const
{
	double Value = 0.0;

	switch (Property)
	{
	case EPCGExContextProperty::Depth:            Value = static_cast<double>(Context.Depth);            break;
	case EPCGExContextProperty::CumulativeWeight: Value = static_cast<double>(Context.CumulativeWeight); break;
	case EPCGExContextProperty::ModuleIndex:      Value = static_cast<double>(Context.ChildModuleIndex); break;
	case EPCGExContextProperty::ConnectorIndex:   Value = static_cast<double>(Context.ChildConnectorIndex); break;
	case EPCGExContextProperty::PlacedCount:      Value = static_cast<double>(Context.PlacedCount);      break;
	}

	return PCGExCompare::Compare(Comparison, Value, Threshold, Tolerance);
}

#pragma endregion
