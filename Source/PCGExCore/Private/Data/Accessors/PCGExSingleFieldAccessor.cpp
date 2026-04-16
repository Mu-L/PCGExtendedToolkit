// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Data/Accessors/PCGExSingleFieldAccessor.h"

#include "Helpers/PCGExMetaHelpersMacros.h"
#include "Types/PCGExTypeOpsImpl.h"

namespace PCGExData
{
	namespace
	{
		// Mirrors STRMAP_SINGLE_FIELD from PCGExSubSelection.h. The third tuple
		// element ("FieldIndex" 0..3) is what SetFieldIndex would normalize to;
		// derived fields (Length/Volume/Sum/...) all carry index 0 in the legacy
		// table -- matching that exactly is required for parity.
		struct FFieldEntry
		{
			PCGExTypeOps::ESingleField Field;
			EPCGMetadataTypes Hint;
			int32 FieldIndex;
		};

		const TMap<FString, FFieldEntry>& GetFieldTable()
		{
			static const TMap<FString, FFieldEntry> Table = {
				{TEXT("X"),             {PCGExTypeOps::ESingleField::X,             EPCGMetadataTypes::Vector,     0}},
				{TEXT("R"),             {PCGExTypeOps::ESingleField::X,             EPCGMetadataTypes::Quaternion, 0}},
				{TEXT("ROLL"),          {PCGExTypeOps::ESingleField::X,             EPCGMetadataTypes::Quaternion, 0}},
				{TEXT("RX"),            {PCGExTypeOps::ESingleField::X,             EPCGMetadataTypes::Quaternion, 0}},
				{TEXT("Y"),             {PCGExTypeOps::ESingleField::Y,             EPCGMetadataTypes::Vector,     1}},
				{TEXT("G"),             {PCGExTypeOps::ESingleField::Y,             EPCGMetadataTypes::Vector4,    1}},
				{TEXT("YAW"),           {PCGExTypeOps::ESingleField::Y,             EPCGMetadataTypes::Quaternion, 1}},
				{TEXT("RY"),            {PCGExTypeOps::ESingleField::Y,             EPCGMetadataTypes::Quaternion, 1}},
				{TEXT("Z"),             {PCGExTypeOps::ESingleField::Z,             EPCGMetadataTypes::Vector,     2}},
				{TEXT("B"),             {PCGExTypeOps::ESingleField::Z,             EPCGMetadataTypes::Vector4,    2}},
				{TEXT("P"),             {PCGExTypeOps::ESingleField::Z,             EPCGMetadataTypes::Quaternion, 2}},
				{TEXT("PITCH"),         {PCGExTypeOps::ESingleField::Z,             EPCGMetadataTypes::Quaternion, 2}},
				{TEXT("RZ"),            {PCGExTypeOps::ESingleField::Z,             EPCGMetadataTypes::Quaternion, 2}},
				{TEXT("W"),             {PCGExTypeOps::ESingleField::W,             EPCGMetadataTypes::Vector4,    3}},
				{TEXT("A"),             {PCGExTypeOps::ESingleField::W,             EPCGMetadataTypes::Vector4,    3}},
				{TEXT("L"),             {PCGExTypeOps::ESingleField::Length,        EPCGMetadataTypes::Vector,     0}},
				{TEXT("LEN"),           {PCGExTypeOps::ESingleField::Length,        EPCGMetadataTypes::Vector,     0}},
				{TEXT("LENGTH"),        {PCGExTypeOps::ESingleField::Length,        EPCGMetadataTypes::Vector,     0}},
				{TEXT("SQUAREDLENGTH"), {PCGExTypeOps::ESingleField::SquaredLength, EPCGMetadataTypes::Vector,     0}},
				{TEXT("LENSQR"),        {PCGExTypeOps::ESingleField::SquaredLength, EPCGMetadataTypes::Vector,     0}},
				{TEXT("VOL"),           {PCGExTypeOps::ESingleField::Volume,        EPCGMetadataTypes::Vector,     0}},
				{TEXT("VOLUME"),        {PCGExTypeOps::ESingleField::Volume,        EPCGMetadataTypes::Vector,     0}},
				{TEXT("SUM"),           {PCGExTypeOps::ESingleField::Sum,           EPCGMetadataTypes::Vector,     0}},
			};
			return Table;
		}
	}

	bool FSingleFieldAccessor::MatchesToken(const FString& UpperToken, FAccessorParseResult& OutParsed) const
	{
		if (const FFieldEntry* Entry = GetFieldTable().Find(UpperToken))
		{
			OutParsed.Field = Entry->Field;
			OutParsed.FieldIndex = Entry->FieldIndex;
			OutParsed.SourceTypeHint = Entry->Hint;
			return true;
		}
		return false;
	}

	bool FSingleFieldAccessor::ResolveOutputType(EPCGMetadataTypes InType,
	                                             const FAccessorParseResult& Parsed,
	                                             EPCGMetadataTypes& OutType) const
	{
		(void)InType;
		(void)Parsed;
		OutType = EPCGMetadataTypes::Double;
		return true;
	}

	void FSingleFieldAccessor::ApplyGet(EPCGMetadataTypes InType,
	                                    const void* Source,
	                                    EPCGMetadataTypes OutType,
	                                    void* OutValue,
	                                    const FAccessorParseResult& Parsed) const
	{
		(void)OutType; // always Double; caller already sized OutValue
		check(Source != nullptr);
		check(OutValue != nullptr);

#define PCGEX_FIELD_EXTRACT(_TYPE, _NAME) { *static_cast<double*>(OutValue) = PCGExTypeOps::FTypeOps<_TYPE>::ExtractField(Source, Parsed.Field); }
		PCGEX_EXECUTEWITHRIGHTTYPE(InType, PCGEX_FIELD_EXTRACT)
#undef PCGEX_FIELD_EXTRACT
	}

	void FSingleFieldAccessor::ApplySet(EPCGMetadataTypes InType,
	                                    void* TargetInOut,
	                                    EPCGMetadataTypes SourceType,
	                                    const void* Source,
	                                    const FAccessorParseResult& Parsed) const
	{
		check(TargetInOut != nullptr);
		check(Source != nullptr);

		// Convert source -> double, then inject into target's field slot.
		double Scalar = 0.0;
		if (SourceType == EPCGMetadataTypes::Double)
		{
			Scalar = *static_cast<const double*>(Source);
		}
		else
		{
			PCGExTypeOps::FConversionTable::Convert(SourceType, Source, EPCGMetadataTypes::Double, &Scalar);
		}

#define PCGEX_FIELD_INJECT(_TYPE, _NAME) { PCGExTypeOps::FTypeOps<_TYPE>::InjectField(TargetInOut, Scalar, Parsed.Field); }
		PCGEX_EXECUTEWITHRIGHTTYPE(InType, PCGEX_FIELD_INJECT)
#undef PCGEX_FIELD_INJECT
	}

	FString FSingleFieldAccessor::GetDisplayName() const
	{
		return TEXT("SingleField");
	}
}
