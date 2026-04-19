// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExBuiltInInlineWidgets.h"

#include "PCGExInlineWidgetRegistry.h"
#include "PCGExPropertyTypes.h"
#include "Editor.h"
#include "PropertyHandle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/NumericTypeInterface.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"

namespace PCGExBuiltInInlineWidgets
{
	namespace
	{
		// UE-standard axis label background colors used in native Transform / Vector inline editors.
		static const FLinearColor AxisColorX = FLinearColor(0.594f, 0.0197f, 0.0f);
		static const FLinearColor AxisColorY = FLinearColor(0.0f, 0.266f, 0.006f);
		static const FLinearColor AxisColorZ = FLinearColor(0.0f, 0.07f, 0.321f);

		// Numeric type interface that displays values with a trailing degree symbol
		// (e.g. 90.0 -> "90.0°") and tolerates the symbol on input parsing.
		// Used for FRotator component editors to mirror the FTransform rotator UX.
		struct FDegreesNumericTypeInterface : public TDefaultNumericTypeInterface<double>
		{
			virtual FString ToString(const double& Value) const override
			{
				return TDefaultNumericTypeInterface<double>::ToString(Value) + TEXT("\u00B0");
			}

			virtual TOptional<double> FromString(const FString& InString, const double& ExistingValue) override
			{
				FString Sanitized = InString;
				Sanitized.ReplaceInline(TEXT("\u00B0"), TEXT(""));
				Sanitized.TrimStartAndEndInline();
				return TDefaultNumericTypeInterface<double>::FromString(Sanitized, ExistingValue);
			}
		};

		TSharedRef<SWidget> MakeAxisEntry(
			TSharedPtr<IPropertyHandle> AxisHandle,
			const FLinearColor& LabelColor,
			TSharedPtr<INumericTypeInterface<double>> TypeInterface = nullptr)
		{
			if (!AxisHandle.IsValid()) { return SNullWidget::NullWidget; }

			return SNew(SNumericEntryBox<double>)
				.AllowSpin(true)
				.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
				.TypeInterface(TypeInterface)
				.MinValue(TOptional<double>())
				.MaxValue(TOptional<double>())
				.MinSliderValue(TOptional<double>())
				.MaxSliderValue(TOptional<double>())
				.SupportDynamicSliderMaxValue(true)
				.SupportDynamicSliderMinValue(true)
				.LinearDeltaSensitivity(1)
				.Delta(0.1)
				.MinDesiredValueWidth(60.0f)
				.Value_Lambda([AxisHandle]() -> TOptional<double>
				{
					double Out = 0.0;
					if (AxisHandle->GetValue(Out) == FPropertyAccess::Success) { return Out; }
					return TOptional<double>();
				})
				.OnValueChanged_Lambda([AxisHandle](double NewValue)
				{
					AxisHandle->SetValue(NewValue, EPropertyValueSetFlags::InteractiveChange);
				})
				.OnValueCommitted_Lambda([AxisHandle](double NewValue, ETextCommit::Type)
				{
					AxisHandle->SetValue(NewValue, EPropertyValueSetFlags::DefaultFlags);
				})
				.OnBeginSliderMovement_Lambda([]()
				{
					GEditor->BeginTransaction(NSLOCTEXT("PCGExInlineWidgets", "SetAxisValue", "Set Axis Value"));
				})
				.OnEndSliderMovement_Lambda([](double)
				{
					GEditor->EndTransaction();
				})
				.Label()
				[
					SNumericEntryBox<double>::BuildNarrowColorLabel(LabelColor)
				];
		}

		TSharedRef<SWidget> MakeVectorWidget(const TSharedRef<IPropertyHandle>& ValueHandle)
		{
			TSharedPtr<IPropertyHandle> X = ValueHandle->GetChildHandle(TEXT("X"));
			TSharedPtr<IPropertyHandle> Y = ValueHandle->GetChildHandle(TEXT("Y"));
			TSharedPtr<IPropertyHandle> Z = ValueHandle->GetChildHandle(TEXT("Z"));

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 2, 0)[MakeAxisEntry(X, AxisColorX)]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0, 2, 0)[MakeAxisEntry(Y, AxisColorY)]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0, 0, 0)[MakeAxisEntry(Z, AxisColorZ)];
		}

		TSharedRef<SWidget> MakeVector2Widget(const TSharedRef<IPropertyHandle>& ValueHandle)
		{
			TSharedPtr<IPropertyHandle> X = ValueHandle->GetChildHandle(TEXT("X"));
			TSharedPtr<IPropertyHandle> Y = ValueHandle->GetChildHandle(TEXT("Y"));

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 2, 0)[MakeAxisEntry(X, AxisColorX)]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0, 0, 0)[MakeAxisEntry(Y, AxisColorY)];
		}

		TSharedRef<SWidget> MakeRotatorWidget(const TSharedRef<IPropertyHandle>& ValueHandle)
		{
			// UE rotator display convention: Roll (X-axis) / Pitch (Y-axis) / Yaw (Z-axis).
			TSharedPtr<IPropertyHandle> Roll = ValueHandle->GetChildHandle(TEXT("Roll"));
			TSharedPtr<IPropertyHandle> Pitch = ValueHandle->GetChildHandle(TEXT("Pitch"));
			TSharedPtr<IPropertyHandle> Yaw = ValueHandle->GetChildHandle(TEXT("Yaw"));

			const TSharedRef<INumericTypeInterface<double>> DegreeInterface = MakeShared<FDegreesNumericTypeInterface>();

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 2, 0)[MakeAxisEntry(Roll, AxisColorX, DegreeInterface)]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0, 2, 0)[MakeAxisEntry(Pitch, AxisColorY, DegreeInterface)]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(2, 0, 0, 0)[MakeAxisEntry(Yaw, AxisColorZ, DegreeInterface)];
		}
	}

	void RegisterAll()
	{
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Vector::StaticStruct()->GetFName(), &MakeVectorWidget);
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Vector2::StaticStruct()->GetFName(), &MakeVector2Widget);
		FPCGExInlineWidgetRegistry::Register(FPCGExProperty_Rotator::StaticStruct()->GetFName(), &MakeRotatorWidget);
	}
}
