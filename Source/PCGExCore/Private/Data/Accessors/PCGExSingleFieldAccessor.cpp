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

	//
	// Stage 3 typed fn pointers
	//

	namespace
	{
		// Extract direction: ParentPtr at T, ChildOut at double.
		template <typename T>
		void FieldGetStep(const void* Parent, void* ChildOut, const FAccessorParseResult& Parsed)
		{
			*static_cast<double*>(ChildOut) = PCGExTypeOps::FTypeOps<T>::ExtractField(Parent, Parsed.Field);
		}

		// Inject direction: ParentInOut at T, NewChild is the double to inject.
		template <typename T>
		void FieldSetStep(void* ParentInOut, const void* NewChild, const FAccessorParseResult& Parsed)
		{
			const double Value = *static_cast<const double*>(NewChild);
			PCGExTypeOps::FTypeOps<T>::InjectField(ParentInOut, Value, Parsed.Field);
		}
	}

	FStepGetFn FSingleFieldAccessor::GetStepGetFn(EPCGMetadataTypes InType) const
	{
		switch (InType)
		{
#define PCGEX_CASE(_TYPE, _NAME, ...) case EPCGMetadataTypes::_NAME: return &FieldGetStep<_TYPE>;
		PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_CASE)
#undef PCGEX_CASE
		default: return nullptr;
		}
	}

	FStepSetFn FSingleFieldAccessor::GetStepSetFn(EPCGMetadataTypes InType) const
	{
		switch (InType)
		{
#define PCGEX_CASE(_TYPE, _NAME, ...) case EPCGMetadataTypes::_NAME: return &FieldSetStep<_TYPE>;
		PCGEX_FOREACH_SUPPORTEDTYPES(PCGEX_CASE)
#undef PCGEX_CASE
		default: return nullptr;
		}
	}

	ISubAccessor::ECompileAction FSingleFieldAccessor::ClassifyForInType(
		EPCGMetadataTypes InType, const FAccessorParseResult& Parsed,
		const FPCGMetadataAttributeDesc* SourceDesc) const
	{
		(void)Parsed;
		(void)SourceDesc;
		switch (InType)
		{
		// Multi-component vector + rotation types: field extraction is natively
		// meaningful (X/Y/Z/W on vectors; X->Roll/Y->Yaw/Z->Pitch on rotations).
		case EPCGMetadataTypes::Vector2:
		case EPCGMetadataTypes::Vector:
		case EPCGMetadataTypes::Vector4:
		case EPCGMetadataTypes::Quaternion:
		case EPCGMetadataTypes::Rotator:
			return ECompileAction::Keep;

		// Scalars: keep. FTypeOps<T>::ExtractField on scalars ignores the field
		// enum and returns the value cast to double, which matches legacy
		// FCachedSubSelection behavior (skip-and-convert produces the same output).
		case EPCGMetadataTypes::Float:
		case EPCGMetadataTypes::Double:
		case EPCGMetadataTypes::Integer32:
		case EPCGMetadataTypes::Integer64:
		case EPCGMetadataTypes::Boolean:
			return ECompileAction::Keep;

		// Transform: promote with Position. `.X` on a Transform attribute
		// conventionally means Position.X; the legacy FTypeOps<FTransform>
		// ::ExtractField honored that via a direct GetLocation() call but the
		// symmetric InjectField was UB. Auto-promoting to [Position, Field]
		// gives the same get output AND a correct set path.
		case EPCGMetadataTypes::Transform:
			return ECompileAction::PromoteWithPosition;

		// String family: field of a string is meaningless; drop the step so
		// the string passes through unchanged.
		case EPCGMetadataTypes::String:
		case EPCGMetadataTypes::Name:
		case EPCGMetadataTypes::SoftObjectPath:
		case EPCGMetadataTypes::SoftClassPath:
			return ECompileAction::Drop;

		default:
			return ECompileAction::Drop;
		}
	}
}
