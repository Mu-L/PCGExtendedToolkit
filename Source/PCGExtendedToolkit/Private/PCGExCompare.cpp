﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/


#include "PCGExCompare.h"

#include "PCGExDetails.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataTag.h"
#include "Data/PCGExProxyData.h"


namespace PCGExCompare
{
	FString ToString(const EPCGExComparison Comparison)
	{
		switch (Comparison)
		{
		case EPCGExComparison::StrictlyEqual:
			return " == ";
		case EPCGExComparison::StrictlyNotEqual:
			return " != ";
		case EPCGExComparison::EqualOrGreater:
			return " >= ";
		case EPCGExComparison::EqualOrSmaller:
			return " <= ";
		case EPCGExComparison::StrictlyGreater:
			return " > ";
		case EPCGExComparison::StrictlySmaller:
			return " < ";
		case EPCGExComparison::NearlyEqual:
			return " ~= ";
		case EPCGExComparison::NearlyNotEqual:
			return " !~= ";
		default: return " ?? ";
		}
	}

	FString ToString(const EPCGExBitflagComparison Comparison)
	{
		switch (Comparison)
		{
		case EPCGExBitflagComparison::MatchPartial:
			return " Any ";
		case EPCGExBitflagComparison::MatchFull:
			return " All ";
		case EPCGExBitflagComparison::MatchStrict:
			return " Exactly ";
		case EPCGExBitflagComparison::NoMatchPartial:
			return " Not Any ";
		case EPCGExBitflagComparison::NoMatchFull:
			return " Not All ";
		default:
			return " ?? ";
		}
	}

	FString ToString(const EPCGExStringComparison Comparison)
	{
		switch (Comparison)
		{
		case EPCGExStringComparison::StrictlyEqual:
			return " == ";
		case EPCGExStringComparison::StrictlyNotEqual:
			return " != ";
		case EPCGExStringComparison::LengthStrictlyEqual:
			return " L == L ";
		case EPCGExStringComparison::LengthStrictlyUnequal:
			return " L != L ";
		case EPCGExStringComparison::LengthEqualOrGreater:
			return " L >= L ";
		case EPCGExStringComparison::LengthEqualOrSmaller:
			return " L <= L ";
		case EPCGExStringComparison::StrictlyGreater:
			return " L > L ";
		case EPCGExStringComparison::StrictlySmaller:
			return " L < L ";
		case EPCGExStringComparison::LocaleStrictlyGreater:
			return " > ";
		case EPCGExStringComparison::LocaleStrictlySmaller:
			return " < ";
		case EPCGExStringComparison::Contains:
			return " contains ";
		case EPCGExStringComparison::StartsWith:
			return " starts with ";
		case EPCGExStringComparison::EndsWith:
			return " ends with ";
		default: return " ?? ";
		}
	}

	FString ToString(const EPCGExStringMatchMode MatchMode)
	{
		switch (MatchMode)
		{
		case EPCGExStringMatchMode::Equals:
			return " == ";
		case EPCGExStringMatchMode::Contains:
			return " contains ";
		case EPCGExStringMatchMode::StartsWith:
			return " starts w ";
		case EPCGExStringMatchMode::EndsWith:
			return " ends w ";
		default: return " ?? ";
		}
	}

	bool Compare(const EPCGExStringComparison Method, const FString& A, const FString& B)
	{
		switch (Method)
		{
		case EPCGExStringComparison::StrictlyEqual:
			return A == B;
		case EPCGExStringComparison::StrictlyNotEqual:
			return A != B;
		case EPCGExStringComparison::LengthStrictlyEqual:
			return A.Len() == B.Len();
		case EPCGExStringComparison::LengthStrictlyUnequal:
			return A.Len() != B.Len();
		case EPCGExStringComparison::LengthEqualOrGreater:
			return A.Len() >= B.Len();
		case EPCGExStringComparison::LengthEqualOrSmaller:
			return A.Len() <= B.Len();
		case EPCGExStringComparison::StrictlyGreater:
			return A.Len() > B.Len();
		case EPCGExStringComparison::StrictlySmaller:
			return A.Len() < B.Len();
		case EPCGExStringComparison::LocaleStrictlyGreater:
			return A > B;
		case EPCGExStringComparison::LocaleStrictlySmaller:
			return A < B;
		case EPCGExStringComparison::Contains:
			return A.Contains(B);
		case EPCGExStringComparison::StartsWith:
			return A.StartsWith(B);
		case EPCGExStringComparison::EndsWith:
			return A.EndsWith(B);
		default:
			return false;
		}
	}

	bool Compare(const EPCGExComparison Method, const TSharedPtr<PCGExData::IDataValue>& A, const double B, const double Tolerance)
	{
		if (!A->IsNumeric()) { return false; }
		return Compare(Method, A->AsDouble(), B, Tolerance);
	}

	bool Compare(const EPCGExStringComparison Method, const TSharedPtr<PCGExData::IDataValue>& A, const FString B)
	{
		if (!A->IsText()) { return false; }
		return Compare(Method, A->AsString(), B);
	}

	bool Compare(const EPCGExBitflagComparison Method, const int64& Flags, const int64& Mask)
	{
		switch (Method)
		{
		case EPCGExBitflagComparison::MatchPartial:
			return ((Flags & Mask) != 0);
		case EPCGExBitflagComparison::MatchFull:
			return ((Flags & Mask) == Mask);
		case EPCGExBitflagComparison::MatchStrict:
			return (Flags == Mask);
		case EPCGExBitflagComparison::NoMatchPartial:
			return ((Flags & Mask) == 0);
		case EPCGExBitflagComparison::NoMatchFull:
			return ((Flags & Mask) != Mask);
		default: return false;
		}
	}

	bool HasMatchingTags(const TSharedPtr<PCGExData::FTags>& InTags, const FString& Query, const EPCGExStringMatchMode MatchMode, const bool bStrict)
	{
		if (bStrict)
		{
			for (const TPair<FString, TSharedPtr<PCGExData::IDataValue>>& Pair : InTags->ValueTags)
			{
				switch (MatchMode)
				{
				case EPCGExStringMatchMode::Equals:
					if (Pair.Key == Query) { return true; }
					break;
				case EPCGExStringMatchMode::Contains:
					if (Pair.Key.Contains(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::StartsWith:
					if (Pair.Key.StartsWith(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::EndsWith:
					if (Pair.Key.EndsWith(Query)) { return true; }
					break;
				}
			}

			for (const FString& Tag : InTags->RawTags)
			{
				switch (MatchMode)
				{
				case EPCGExStringMatchMode::Equals:
					if (Tag == Query) { return true; }
					break;
				case EPCGExStringMatchMode::Contains:
					if (Tag.Contains(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::StartsWith:
					if (Tag.StartsWith(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::EndsWith:
					if (Tag.EndsWith(Query)) { return true; }
					break;
				}
			}
		}
		else
		{
			TArray<FString> FlattenedTags = InTags->FlattenToArray();
			for (const FString& Tag : FlattenedTags)
			{
				switch (MatchMode)
				{
				case EPCGExStringMatchMode::Equals:
					if (Tag == Query) { return true; }
					break;
				case EPCGExStringMatchMode::Contains:
					if (Tag.Contains(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::StartsWith:
					if (Tag.StartsWith(Query)) { return true; }
					break;
				case EPCGExStringMatchMode::EndsWith:
					if (Tag.EndsWith(Query)) { return true; }
					break;
				}
			}
		}

		return false;
	}

	bool GetMatchingValueTags(const TSharedPtr<PCGExData::FTags>& InTags, const FString& Query, const EPCGExStringMatchMode MatchMode, TArray<TSharedPtr<PCGExData::IDataValue>>& OutValues)
	{
		for (const TPair<FString, TSharedPtr<PCGExData::IDataValue>>& Pair : InTags->ValueTags)
		{
			switch (MatchMode)
			{
			case EPCGExStringMatchMode::Equals:
				if (Pair.Key == Query) { OutValues.Add(Pair.Value); }
				break;
			case EPCGExStringMatchMode::Contains:
				if (Pair.Key.Contains(Query)) { OutValues.Add(Pair.Value); }
				break;
			case EPCGExStringMatchMode::StartsWith:
				if (Pair.Key.StartsWith(Query)) { OutValues.Add(Pair.Value); }
				break;
			case EPCGExStringMatchMode::EndsWith:
				if (Pair.Key.EndsWith(Query)) { OutValues.Add(Pair.Value); }
				break;
			}
		}

		return !OutValues.IsEmpty();
	}
}

bool FPCGExVectorHashComparisonDetails::Init(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InPrimaryDataFacade)
{
	Tolerance = GetValueSettingTolerance();
	if (!Tolerance->Init(InContext, InPrimaryDataFacade, false)) { return false; }

	return true;
}

FVector FPCGExVectorHashComparisonDetails::GetCWTolerance(const int32 PointIndex) const
{
	return FVector(1 / Tolerance->Read(PointIndex));
}

void FPCGExVectorHashComparisonDetails::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	FName Consumable = NAME_None;
	PCGEX_CONSUMABLE_CONDITIONAL(HashToleranceInput == EPCGExInputValueType::Attribute, HashToleranceAttribute, Consumable)
}

bool FPCGExVectorHashComparisonDetails::GetOnlyUseDataDomain() const
{
	return HashToleranceInput == EPCGExInputValueType::Constant || PCGExHelpers::IsDataDomainAttribute(HashToleranceAttribute);
}

bool FPCGExVectorHashComparisonDetails::Test(const FVector& A, const FVector& B, const int32 PointIndex) const
{
	const FVector CWTolerance = GetCWTolerance(PointIndex);
	return PCGEx::I323(A, CWTolerance) == PCGEx::I323(B, CWTolerance);
}

void FPCGExStaticDotComparisonDetails::Init()
{
	if (Domain == EPCGExAngularDomain::Degrees) { ComparisonTolerance = (1 + PCGExMath::DegreesToDot(180 - DegreesTolerance)) * 0.5; }
	else { ComparisonTolerance = (1 + DotTolerance) * 0.5; }
	if (bUnsignedComparison) { DotTolerance = FMath::Abs(DotTolerance); }
}

bool FPCGExStaticDotComparisonDetails::Test(const double A) const
{
	return PCGExCompare::Compare(Comparison, bUnsignedComparison ? FMath::Abs(A) : (1 + A) * 0.5, DotTolerance, ComparisonTolerance);
}

bool FPCGExDotComparisonDetails::Init(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InPrimaryDataCache)
{
	ThresholdGetter = GetValueSettingThreshold();
	if (!ThresholdGetter->Init(InContext, InPrimaryDataCache, false)) { return false; }

	if (Domain == EPCGExAngularDomain::Degrees) { ComparisonTolerance = (1 + PCGExMath::DegreesToDot(180 - DegreesTolerance)) * 0.5; }
	else { ComparisonTolerance = DotTolerance; }

	return true;
}

double FPCGExDotComparisonDetails::GetComparisonThreshold(const int32 PointIndex) const
{
	if (Domain == EPCGExAngularDomain::Scalar) { return ThresholdGetter->Read(PointIndex); }
	return PCGExMath::DegreesToDot(180 - ThresholdGetter->Read(PointIndex));
}

bool FPCGExDotComparisonDetails::Test(const double A, const double B) const
{
	return bUnsignedComparison ?
		       PCGExCompare::Compare(Comparison, FMath::Abs(A), FMath::Abs(B), ComparisonTolerance) :
		       PCGExCompare::Compare(Comparison, (1 + A) * 0.5, (1 + B) * 0.5, ComparisonTolerance);
}

bool FPCGExDotComparisonDetails::Test(const double A, const int32 Index) const
{
	return Test(A, GetComparisonThreshold(Index));
}

void FPCGExDotComparisonDetails::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	FName Consumable = NAME_None;
	PCGEX_CONSUMABLE_CONDITIONAL(ThresholdInput == EPCGExInputValueType::Attribute, ThresholdAttribute, Consumable)
}

bool FPCGExDotComparisonDetails::GetOnlyUseDataDomain() const
{
	return ThresholdInput == EPCGExInputValueType::Constant || PCGExHelpers::IsDataDomainAttribute(ThresholdAttribute);
}

#if WITH_EDITOR
FString FPCGExDotComparisonDetails::GetDisplayComparison() const
{
	FString AngleStr = ThresholdInput == EPCGExInputValueType::Attribute ? PCGEx::GetSelectorDisplayName(ThresholdAttribute) : TEXT("");
	if (ThresholdInput == EPCGExInputValueType::Constant)
	{
		if (Domain == EPCGExAngularDomain::Degrees) { AngleStr = FString::Printf(TEXT("%.1f°"), DegreesConstant); }
		else { AngleStr = FString::Printf(TEXT("%.1f°"), FMath::RadiansToDegrees(FMath::Acos(DotConstant))); }
	}

	FString Str = PCGExCompare::ToString(Comparison) + (bUnsignedComparison ? TEXT("±") : TEXT("")) + AngleStr;
	return Str;
}
#endif


bool FPCGExAttributeToTagComparisonDetails::Init(const FPCGContext* InContext, const TSharedRef<PCGExData::FFacade>& InSourceDataFacade)
{
	if (TagNameInput == EPCGExInputValueType::Attribute)
	{
		TagNameGetter = MakeShared<PCGEx::TAttributeBroadcaster<FString>>();
		if (!TagNameGetter->Prepare(TagNameAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag name attribute."));
			return false;
		}
	}

	if (!bDoValueMatch) { return true; }

	switch (ValueType)
	{
	case EPCGExComparisonDataType::Numeric:
		NumericValueGetter = MakeShared<PCGEx::TAttributeBroadcaster<double>>();
		if (!NumericValueGetter->Prepare(ValueAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag value attribute."));
			return false;
		}
		break;
	case EPCGExComparisonDataType::String:
		StringValueGetter = MakeShared<PCGEx::TAttributeBroadcaster<FString>>();
		if (!StringValueGetter->Prepare(ValueAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag value attribute."));
			return false;
		}
		break;
	}

	return true;
}

bool FPCGExAttributeToTagComparisonDetails::Matches(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExData::FConstPoint& SourcePoint) const
{
	const FString TestTagName = TagNameGetter ? TagNameGetter->SoftGet(SourcePoint, TEXT("")) : TagName;

	if (!bDoValueMatch)
	{
		return PCGExCompare::HasMatchingTags(InData->Tags, TagNameGetter ? TagNameGetter->SoftGet(SourcePoint, TEXT("")) : TagName, NameMatch);
	}


	TArray<TSharedPtr<PCGExData::IDataValue>> TagValues;
	if (!PCGExCompare::GetMatchingValueTags(InData->Tags, TestTagName, NameMatch, TagValues)) { return false; }

	if (ValueType == EPCGExComparisonDataType::Numeric)
	{
		const double OperandBNumeric = NumericValueGetter->SoftGet(SourcePoint, 0);
		for (const TSharedPtr<PCGExData::IDataValue>& TagValue : TagValues)
		{
			if (!PCGExCompare::Compare(NumericComparison, TagValue, OperandBNumeric, Tolerance)) { return false; }
		}
	}
	else
	{
		const FString OperandBString = StringValueGetter->SoftGet(SourcePoint, TEXT(""));
		for (const TSharedPtr<PCGExData::IDataValue>& TagValue : TagValues)
		{
			if (!PCGExCompare::Compare(StringComparison, TagValue, OperandBString)) { return false; }
		}
	}

	return true;
}

void FPCGExAttributeToTagComparisonDetails::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	InContext->AddConsumableAttributeName(TagNameAttribute);

	FName Consumable = NAME_None;
	PCGEX_CONSUMABLE_SELECTOR(ValueAttribute, Consumable)
}

bool FPCGExAttributeToTagComparisonDetails::GetOnlyUseDataDomain() const
{
	return TagNameInput == EPCGExInputValueType::Constant &&
		PCGExHelpers::IsDataDomainAttribute(ValueAttribute);
}

bool FPCGExAttributeToDataComparisonDetails::Init(const FPCGContext* InContext, const TSharedRef<PCGExData::FFacade>& InSourceDataFacade)
{
	if (DataNameInput == EPCGExInputValueType::Attribute)
	{
		DataNameGetter = MakeShared<PCGEx::TAttributeBroadcaster<FName>>();
		if (!DataNameGetter->Prepare(DataNameAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag name attribute."));
			return false;
		}
	}

	switch (Check)
	{
	case EPCGExComparisonDataType::Numeric:
		NumericValueGetter = MakeShared<PCGEx::TAttributeBroadcaster<double>>();
		if (!NumericValueGetter->Prepare(ValueNameAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag value attribute."));
			return false;
		}
		break;
	case EPCGExComparisonDataType::String:
		StringValueGetter = MakeShared<PCGEx::TAttributeBroadcaster<FString>>();
		if (!StringValueGetter->Prepare(ValueNameAttribute, InSourceDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("Invalid tag value attribute."));
			return false;
		}
		break;
	}

	return true;
}

bool FPCGExAttributeToDataComparisonDetails::Matches(const TSharedPtr<PCGExData::FPointIO>& InData, const PCGExData::FConstPoint& SourcePoint) const
{
	FPCGAttributeIdentifier Identifier = PCGEx::GetAttributeIdentifier(DataNameGetter ? DataNameGetter->SoftGet(SourcePoint, NAME_None) : DataName, InData->GetIn());
	Identifier.MetadataDomain = PCGMetadataDomainID::Data;

	const FPCGMetadataAttributeBase* Attribute = InData->FindConstAttribute(Identifier);
	if (!Attribute) { return false; }

	if (Check == EPCGExComparisonDataType::Numeric)
	{
		return PCGExCompare::Compare(
			NumericCompare, PCGExDataHelpers::ReadDataValue<double>(Attribute, 0),
			NumericValueGetter->SoftGet(SourcePoint, 0), Tolerance);
	}
	return PCGExCompare::Compare(
		StringCompare, PCGExDataHelpers::ReadDataValue<FString>(Attribute, TEXT("")),
		StringValueGetter->SoftGet(SourcePoint, TEXT("")));
}

void FPCGExAttributeToDataComparisonDetails::RegisterConsumableAttributesWithData(FPCGExContext* InContext, const UPCGData* InData) const
{
	InContext->AddConsumableAttributeName(DataNameAttribute);
	InContext->AddConsumableAttributeName(ValueNameAttribute);
}

bool FPCGExAttributeToDataComparisonDetails::GetOnlyUseDataDomain() const
{
	return DataNameInput == EPCGExInputValueType::Constant &&
		PCGExHelpers::IsDataDomainAttribute(ValueNameAttribute);
}

int64 FPCGExBitmask::Get() const
{
	int64 Mask = 0;

	if (Mode == EPCGExBitmaskMode::Direct) { return Bitmask; }

	if (Mode == EPCGExBitmaskMode::Individual)
	{
		for (const FClampedBit& Bit : Bits) { if (Bit.bValue) { Mask |= (1LL << Bit.BitIndex); } }
	}
	else
	{
		Mask |= static_cast<int64>(Range_00_08) << 0;
		Mask |= static_cast<int64>(Range_08_16) << 8;
		Mask |= static_cast<int64>(Range_16_24) << 16;
		Mask |= static_cast<int64>(Range_24_32) << 24;
		Mask |= static_cast<int64>(Range_32_40) << 32;
		Mask |= static_cast<int64>(Range_40_48) << 40;
		Mask |= static_cast<int64>(Range_48_56) << 48;
		Mask |= static_cast<int64>(Range_56_64) << 56;
	}

	return Mask;
}

void FPCGExBitmask::DoOperation(const EPCGExBitOp Op, int64& Flags) const
{
	const int64 Mask = Get();
	switch (Op)
	{
	case EPCGExBitOp::Set:
		Flags = Mask;
		break;
	case EPCGExBitOp::AND:
		Flags &= Mask;
		break;
	case EPCGExBitOp::OR:
		Flags |= Mask;
		break;
	case EPCGExBitOp::NOT:
		Flags &= ~Mask;
		break;
	case EPCGExBitOp::XOR:
		Flags ^= Mask;
		break;
	default: ;
	}
}

int64 FPCGExBitmaskWithOperation::Get() const
{
	int64 Mask = 0;

	switch (Mode)
	{
	case EPCGExBitmaskMode::Direct:
		Mask = Bitmask;
		break;
	case EPCGExBitmaskMode::Individual:
		for (const FClampedBitOp& Bit : Bits) { if (Bit.bValue) { Mask |= (1LL << Bit.BitIndex); } }
		break;
	case EPCGExBitmaskMode::Composite:
		Mask |= static_cast<int64>(Range_00_08) << 0;
		Mask |= static_cast<int64>(Range_08_16) << 8;
		Mask |= static_cast<int64>(Range_16_24) << 16;
		Mask |= static_cast<int64>(Range_24_32) << 24;
		Mask |= static_cast<int64>(Range_32_40) << 32;
		Mask |= static_cast<int64>(Range_40_48) << 40;
		Mask |= static_cast<int64>(Range_48_56) << 48;
		Mask |= static_cast<int64>(Range_56_64) << 56;
		break;
	default: ;
	}

	return Mask;
}

void FPCGExBitmaskWithOperation::DoOperation(int64& Flags) const
{
	if (Mode == EPCGExBitmaskMode::Individual)
	{
		for (const FClampedBitOp& BitOp : Bits)
		{
			const int64 Bit = BitOp.Get();
			switch (BitOp.Op)
			{
			case EPCGExBitOp::Set:
				if (BitOp.bValue) { Flags |= Bit; } // Set the bit
				else { Flags &= Bit; }              // Clear the bit
				break;
			case EPCGExBitOp::AND:
				Flags &= Bit;
				break;
			case EPCGExBitOp::OR:
				Flags |= Bit;
				break;
			case EPCGExBitOp::NOT:
				Flags &= ~Bit;
				break;
			case EPCGExBitOp::XOR:
				Flags ^= Bit;
				break;
			default: ;
			}
		}
		return;
	}

	const int64 Mask = Get();

	switch (Op)
	{
	case EPCGExBitOp::Set:
		Flags = Mask;
		break;
	case EPCGExBitOp::AND:
		Flags &= Mask;
		break;
	case EPCGExBitOp::OR:
		Flags |= Mask;
		break;
	case EPCGExBitOp::NOT:
		Flags &= ~Mask;
		break;
	case EPCGExBitOp::XOR:
		Flags ^= Mask;
		break;
	default: ;
	}
}
