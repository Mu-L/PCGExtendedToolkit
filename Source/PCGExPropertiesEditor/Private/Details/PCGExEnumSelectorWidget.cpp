// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Details/PCGExEnumSelectorWidget.h"

#include "DetailLayoutBuilder.h"
#include "PropertyHandle.h"
#include "PCGExEnumSelector.h"
#include "ScopedTransaction.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "PCGExEnumSelectorWidget"

namespace PCGExEnumSelectorWidget
{
	/**
	 * Font for items inside dropdown menus. The default detail font (9pt) is too cramped
	 * for a long-scrolling list; this is the same Regular face at a slightly larger size,
	 * which matches the readability of native UE class/asset pickers.
	 */
	static FSlateFontInfo MenuItemFont() { return FCoreStyle::GetDefaultFontStyle("Regular", 10); }

	// ---------- Raw data access on the FPCGExEnumSelector property handle -------------------

	/** Returns the FPCGExEnumSelector* for each edited object. Empty array if handle invalid. */
	static TArray<FPCGExEnumSelector*> AccessSelectors(const TSharedRef<IPropertyHandle>& ValueHandle)
	{
		TArray<FPCGExEnumSelector*> Out;
		if (!ValueHandle->IsValidHandle()) { return Out; }

		TArray<void*> RawData;
		ValueHandle->AccessRawData(RawData);
		Out.Reserve(RawData.Num());
		for (void* Raw : RawData)
		{
			if (Raw) { Out.Add(static_cast<FPCGExEnumSelector*>(Raw)); }
		}
		return Out;
	}

	/** Returns the unanimous Class across all edited selectors, or nullptr on multi-edit conflict / no data. */
	static UEnum* ReadUnanimousClass(const TSharedRef<IPropertyHandle>& ValueHandle, bool& bOutMultipleValues)
	{
		bOutMultipleValues = false;
		const TArray<FPCGExEnumSelector*> Selectors = AccessSelectors(ValueHandle);
		if (Selectors.IsEmpty()) { return nullptr; }

		UEnum* First = Selectors[0]->Class;
		for (int32 i = 1; i < Selectors.Num(); ++i)
		{
			if (Selectors[i]->Class != First)
			{
				bOutMultipleValues = true;
				return nullptr;
			}
		}
		return First;
	}

	/** Returns the unanimous Value across all edited selectors. Sets bOutMultipleValues if any disagree. */
	static int64 ReadUnanimousValue(const TSharedRef<IPropertyHandle>& ValueHandle, bool& bOutMultipleValues)
	{
		bOutMultipleValues = false;
		const TArray<FPCGExEnumSelector*> Selectors = AccessSelectors(ValueHandle);
		if (Selectors.IsEmpty()) { return 0; }

		const int64 First = Selectors[0]->Value;
		for (int32 i = 1; i < Selectors.Num(); ++i)
		{
			if (Selectors[i]->Value != First)
			{
				bOutMultipleValues = true;
				return 0;
			}
		}
		return First;
	}

	/**
	 * Apply a write across all edited selectors with proper PreChange/PostChange notifications.
	 * Mutator is invoked once per edited selector with its raw pointer.
	 */
	static void ApplyWrite(
		const TSharedRef<IPropertyHandle>& ValueHandle,
		const FText& TransactionDescription,
		TFunctionRef<void(FPCGExEnumSelector&)> Mutator)
	{
		if (!ValueHandle->IsValidHandle()) { return; }

		const TArray<FPCGExEnumSelector*> Selectors = AccessSelectors(ValueHandle);
		if (Selectors.IsEmpty()) { return; }

		FScopedTransaction Transaction(TransactionDescription);
		ValueHandle->NotifyPreChange();

		for (FPCGExEnumSelector* Selector : Selectors) { Mutator(*Selector); }

		ValueHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
		ValueHandle->NotifyFinishedChangingProperties();
	}

	// ---------- Enum metadata helpers ----------------------------------------------------------

	static bool IsBitflagsEnum(const UEnum* Enum) { return Enum && Enum->HasMetaData(TEXT("Bitflags")); }

	/**
	 * Mirrors the engine's own SEnumComboBox visibility filter so behavior matches what users
	 * see in stock detail panels for enum-typed UPROPERTYs.
	 *
	 * Filters: Hidden / Spacer / BlueprintInternalUseOnly / HiddenByDefault metadata, plus the
	 * synthetic <EnumName>_MAX sentinel UHT auto-appends to most native UENUMs. The MAX filter
	 * is by name suffix because (a) it's not always present, (b) real values never end in _MAX,
	 * and (c) it can sit anywhere in the array — blindly stripping the last index is unsafe.
	 *
	 * Callers iterate the full Enum->NumEnums() range and skip indices where this returns true.
	 */
	static bool ShouldHideEnumValue(const UEnum* Enum, int32 Index)
	{
		if (Enum->HasMetaData(TEXT("Hidden"), Index)
			|| Enum->HasMetaData(TEXT("Spacer"), Index)
			|| Enum->HasMetaData(TEXT("BlueprintInternalUseOnly"), Index)
			|| Enum->HasMetaData(TEXT("HiddenByDefault"), Index))
		{
			return true;
		}

		const FString EntryName = Enum->GetNameStringByIndex(Index);
		if (EntryName.EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive)) { return true; }

		return false;
	}

	/**
	 * UEnums we never want to surface in the picker. These are engine internals (the macro
	 * scaffolding under /Script/CoreUObject) plus any enum explicitly tagged Hidden or
	 * BlueprintInternalUseOnly. UUserDefinedEnum (blueprint enums) are deliberately *included*.
	 */
	static bool ShouldHideEnumClass(const UEnum* Enum)
	{
		if (!Enum) { return true; }
		if (Enum->HasMetaData(TEXT("Hidden"))) { return true; }
		if (Enum->HasMetaData(TEXT("BlueprintInternalUseOnly"))) { return true; }
		return false;
	}

	// ---------- Display labels -----------------------------------------------------------------

	static FText FormatClassButtonLabel(const TSharedRef<IPropertyHandle>& ValueHandle)
	{
		bool bMulti = false;
		UEnum* Enum = ReadUnanimousClass(ValueHandle, bMulti);
		if (bMulti) { return LOCTEXT("MultipleClasses", "Multiple Values"); }
		if (!Enum) { return LOCTEXT("NoClass", "Select Enum..."); }
		return FText::FromString(Enum->GetName());
	}

	static FText FormatValueButtonLabel(const TSharedRef<IPropertyHandle>& ValueHandle)
	{
		bool bMultiClass = false;
		UEnum* Enum = ReadUnanimousClass(ValueHandle, bMultiClass);
		if (bMultiClass || !Enum) { return LOCTEXT("NoValue", "—"); }

		bool bMultiValue = false;
		const int64 Value = ReadUnanimousValue(ValueHandle, bMultiValue);
		if (bMultiValue) { return LOCTEXT("MultipleValues", "Multiple Values"); }

		if (IsBitflagsEnum(Enum))
		{
			if (Value == 0) { return LOCTEXT("BitflagsNone", "(None)"); }

			TArray<FString> Parts;
			for (int32 i = 0, Num = Enum->NumEnums(); i < Num; ++i)
			{
				if (ShouldHideEnumValue(Enum, i)) { continue; }
				const int64 Bit = Enum->GetValueByIndex(i);
				if (Bit != 0 && (Value & Bit) == Bit) { Parts.Add(Enum->GetDisplayNameTextByIndex(i).ToString()); }
			}
			return FText::FromString(FString::Join(Parts, TEXT(" | ")));
		}

		const int32 Index = Enum->GetIndexByValue(Value);
		if (Index == INDEX_NONE) { return FText::AsNumber(Value); }
		return Enum->GetDisplayNameTextByIndex(Index);
	}

	// ---------- Class picker menu --------------------------------------------------------------

	/**
	 * Searchable menu listing all eligible UEnum classes. Picks one and writes Class to the
	 * property handle (resetting Value to 0 since the previous value is unlikely to map cleanly).
	 *
	 * Held internally by an SComboButton's menu content; lifetime ends when the menu closes.
	 */
	class SEnumClassMenu : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEnumClassMenu) {}
			SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, ValueHandle)
			SLATE_ARGUMENT(TWeakPtr<SComboButton>, OwningCombo)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ValueHandle = InArgs._ValueHandle;
			OwningCombo = InArgs._OwningCombo;

			BuildAllEntries();
			FilteredEntries = AllEntries;

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(280.0f)
				.HeightOverride(400.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(4)
					[
						SNew(SSearchBox)
						.OnTextChanged(this, &SEnumClassMenu::OnSearchTextChanged)
					]
					+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4, 0, 4, 4)
					[
						SAssignNew(ListView, SListView<TWeakObjectPtr<UEnum>>)
						.ListItemsSource(&FilteredEntries)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SEnumClassMenu::OnGenerateRow)
						.OnSelectionChanged(this, &SEnumClassMenu::OnSelectionChanged)
					]
				]
			];
		}

	private:
		void BuildAllEntries()
		{
			AllEntries.Reset();
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				UEnum* Enum = *It;
				if (ShouldHideEnumClass(Enum)) { continue; }
				AllEntries.Add(Enum);
			}
			AllEntries.Sort([](const TWeakObjectPtr<UEnum>& A, const TWeakObjectPtr<UEnum>& B)
			{
				const UEnum* EA = A.Get();
				const UEnum* EB = B.Get();
				if (!EA || !EB) { return EA != nullptr; }
				return EA->GetName() < EB->GetName();
			});
		}

		void OnSearchTextChanged(const FText& NewText)
		{
			const FString Query = NewText.ToString().TrimStartAndEnd();
			if (Query.IsEmpty())
			{
				FilteredEntries = AllEntries;
			}
			else
			{
				FilteredEntries.Reset();
				for (const TWeakObjectPtr<UEnum>& Weak : AllEntries)
				{
					const UEnum* Enum = Weak.Get();
					if (!Enum) { continue; }
					if (Enum->GetName().Contains(Query, ESearchCase::IgnoreCase) ||
						Enum->GetDisplayNameText().ToString().Contains(Query, ESearchCase::IgnoreCase))
					{
						FilteredEntries.Add(Weak);
					}
				}
			}
			if (ListView.IsValid()) { ListView->RequestListRefresh(); }
		}

		TSharedRef<ITableRow> OnGenerateRow(TWeakObjectPtr<UEnum> Item, const TSharedRef<STableViewBase>& OwnerTable)
		{
			const UEnum* Enum = Item.Get();
			const FString Name = Enum ? Enum->GetName() : FString(TEXT("(missing)"));
			const FText Tooltip = Enum ? Enum->GetToolTipText() : FText::GetEmpty();

			return SNew(STableRow<TWeakObjectPtr<UEnum>>, OwnerTable)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Name))
					.ToolTipText(Tooltip)
					.Font(MenuItemFont())
				];
		}

		void OnSelectionChanged(TWeakObjectPtr<UEnum> Selected, ESelectInfo::Type SelectInfo)
		{
			// Ignore programmatic selection changes from list refresh; only act on user clicks.
			if (SelectInfo == ESelectInfo::Direct) { return; }

			UEnum* NewClass = Selected.Get();
			if (!NewClass) { return; }
			if (!ValueHandle.IsValid() || !ValueHandle->IsValidHandle()) { return; }

			ApplyWrite(ValueHandle.ToSharedRef(), LOCTEXT("SetEnumClass", "Set Enum Class"),
				[NewClass](FPCGExEnumSelector& Selector)
				{
					Selector.Class = NewClass;
					// Previous Value is unlikely to map cleanly to a different enum — reset.
					Selector.Value = 0;
				});

			if (TSharedPtr<SComboButton> Combo = OwningCombo.Pin()) { Combo->SetIsOpen(false); }
		}

		TSharedPtr<IPropertyHandle> ValueHandle;
		TWeakPtr<SComboButton> OwningCombo;
		TArray<TWeakObjectPtr<UEnum>> AllEntries;
		TArray<TWeakObjectPtr<UEnum>> FilteredEntries;
		TSharedPtr<SListView<TWeakObjectPtr<UEnum>>> ListView;
	};

	// ---------- Value picker menu (single-select) ----------------------------------------------

	/** Single-select enum value menu. One item per non-hidden enum index. No search box —
	 *  enum value lists are short enough to scan visually, and a search box just adds noise. */
	class SEnumValueMenu : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEnumValueMenu) {}
			SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, ValueHandle)
			SLATE_ARGUMENT(TWeakObjectPtr<UEnum>, EnumClass)
			SLATE_ARGUMENT(TWeakPtr<SComboButton>, OwningCombo)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ValueHandle = InArgs._ValueHandle;
			EnumClass = InArgs._EnumClass;
			OwningCombo = InArgs._OwningCombo;

			BuildEntries();

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(220.0f)
				.MaxDesiredHeight(360.0f)
				.Padding(FMargin(4))
				[
					SNew(SListView<TSharedPtr<int32>>)
					.ListItemsSource(&Entries)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SEnumValueMenu::OnGenerateRow)
					.OnSelectionChanged(this, &SEnumValueMenu::OnSelectionChanged)
				]
			];
		}

	private:
		void BuildEntries()
		{
			Entries.Reset();
			const UEnum* Enum = EnumClass.Get();
			if (!Enum) { return; }
			for (int32 i = 0, Num = Enum->NumEnums(); i < Num; ++i)
			{
				if (ShouldHideEnumValue(Enum, i)) { continue; }
				Entries.Add(MakeShared<int32>(i));
			}
		}

		TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<int32> Item, const TSharedRef<STableViewBase>& OwnerTable)
		{
			const UEnum* Enum = EnumClass.Get();
			const int32 Index = Item.IsValid() ? *Item : INDEX_NONE;
			const FText Display = (Enum && Index != INDEX_NONE) ? Enum->GetDisplayNameTextByIndex(Index) : FText::GetEmpty();
			const FText Tooltip = (Enum && Index != INDEX_NONE) ? Enum->GetToolTipTextByIndex(Index) : FText::GetEmpty();

			return SNew(STableRow<TSharedPtr<int32>>, OwnerTable)
				[
					SNew(STextBlock)
					.Text(Display)
					.ToolTipText(Tooltip)
					.Font(MenuItemFont())
				];
		}

		void OnSelectionChanged(TSharedPtr<int32> Selected, ESelectInfo::Type SelectInfo)
		{
			if (SelectInfo == ESelectInfo::Direct) { return; }
			if (!Selected.IsValid()) { return; }

			const UEnum* Enum = EnumClass.Get();
			if (!Enum) { return; }
			if (!ValueHandle.IsValid() || !ValueHandle->IsValidHandle()) { return; }

			const int64 NewValue = Enum->GetValueByIndex(*Selected);

			ApplyWrite(ValueHandle.ToSharedRef(), LOCTEXT("SetEnumValue", "Set Enum Value"),
				[NewValue](FPCGExEnumSelector& Selector) { Selector.Value = NewValue; });

			if (TSharedPtr<SComboButton> Combo = OwningCombo.Pin()) { Combo->SetIsOpen(false); }
		}

		TSharedPtr<IPropertyHandle> ValueHandle;
		TWeakObjectPtr<UEnum> EnumClass;
		TWeakPtr<SComboButton> OwningCombo;
		TArray<TSharedPtr<int32>> Entries;
	};

	// ---------- Value picker menu (bitflags) ---------------------------------------------------

	/** Multi-select bitmask menu. Each row is a checkbox toggling one bit. */
	class SEnumBitflagsMenu : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEnumBitflagsMenu) {}
			SLATE_ARGUMENT(TSharedPtr<IPropertyHandle>, ValueHandle)
			SLATE_ARGUMENT(TWeakObjectPtr<UEnum>, EnumClass)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ValueHandle = InArgs._ValueHandle;
			EnumClass = InArgs._EnumClass;

			TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

			const UEnum* Enum = EnumClass.Get();
			if (Enum)
			{
				for (int32 i = 0, Num = Enum->NumEnums(); i < Num; ++i)
				{
					if (ShouldHideEnumValue(Enum, i)) { continue; }
					const int64 Bit = Enum->GetValueByIndex(i);
					if (Bit == 0) { continue; }

					const FText Display = Enum->GetDisplayNameTextByIndex(i);
					const FText Tooltip = Enum->GetToolTipTextByIndex(i);

					Box->AddSlot().AutoHeight().Padding(2)
					[
						SNew(SCheckBox)
						.ToolTipText(Tooltip)
						.IsChecked(this, &SEnumBitflagsMenu::IsBitChecked, Bit)
						.OnCheckStateChanged(this, &SEnumBitflagsMenu::OnBitToggled, Bit)
						[
							SNew(STextBlock)
							.Text(Display)
							.Font(MenuItemFont())
						]
					];
				}
			}

			ChildSlot
			[
				SNew(SBox)
				.WidthOverride(220.0f)
				.MaxDesiredHeight(360.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot().Padding(4)
					[
						Box
					]
				]
			];
		}

	private:
		void EnsureCache() const
		{
			if (bCacheValid) { return; }
			if (!ValueHandle.IsValid() || !ValueHandle->IsValidHandle()) { return; }
			CachedValue = ReadUnanimousValue(ValueHandle.ToSharedRef(), bCachedMulti);
			bCacheValid = true;
		}

		ECheckBoxState IsBitChecked(int64 Bit) const
		{
			if (!ValueHandle.IsValid() || !ValueHandle->IsValidHandle()) { return ECheckBoxState::Unchecked; }
			EnsureCache();
			if (bCachedMulti) { return ECheckBoxState::Undetermined; }
			return (CachedValue & Bit) == Bit ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		}

		void OnBitToggled(ECheckBoxState NewState, int64 Bit)
		{
			if (!ValueHandle.IsValid() || !ValueHandle->IsValidHandle()) { return; }
			const bool bSet = (NewState == ECheckBoxState::Checked);

			ApplyWrite(ValueHandle.ToSharedRef(), LOCTEXT("ToggleEnumBit", "Toggle Enum Flag"),
				[Bit, bSet](FPCGExEnumSelector& Selector)
				{
					if (bSet) { Selector.Value |= Bit; }
					else { Selector.Value &= ~Bit; }
				});

			bCacheValid = false;
		}

		TSharedPtr<IPropertyHandle> ValueHandle;
		TWeakObjectPtr<UEnum> EnumClass;

		// Cached unanimous value. IsBitChecked is called once per checkbox per Slate paint;
		// without this cache, a 32-flag enum re-walks AccessRawData 32× per paint.
		mutable int64 CachedValue = 0;
		mutable bool bCachedMulti = false;
		mutable bool bCacheValid = false;
	};

	// ---------- Top-level factory --------------------------------------------------------------

	TSharedRef<SWidget> Make(const TSharedRef<IPropertyHandle>& ValueHandle, bool bAllowClassPicker)
	{
		if (!ValueHandle->IsValidHandle()) { return SNullWidget::NullWidget; }

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

		// Class picker (schema-edit mode only).
		if (bAllowClassPicker)
		{
			TSharedPtr<SComboButton> ClassCombo;
			Row->AddSlot().FillWidth(1.0f).Padding(0, 0, 4, 0)
			[
				SAssignNew(ClassCombo, SComboButton)
				.ContentPadding(FMargin(4, 2))
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([ValueHandle]() { return FormatClassButtonLabel(ValueHandle); })
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
			];

			TWeakPtr<SComboButton> WeakCombo = ClassCombo;
			ClassCombo->SetOnGetMenuContent(FOnGetContent::CreateLambda([ValueHandle, WeakCombo]() -> TSharedRef<SWidget>
			{
				return SNew(SEnumClassMenu)
					.ValueHandle(ValueHandle)
					.OwningCombo(WeakCombo);
			}));
		}

		// Value picker — content lambda inspects the current Class each time the menu opens
		// and switches between the single-select and bitflags variants accordingly.
		// Memoize the formatted label by (Class, Value, bMulti) tuple: for bitflags enums,
		// FormatValueButtonLabel iterates the entire enum range every paint, which compounds
		// across many visible selectors in a details panel.
		struct FValueLabelCache
		{
			UEnum* Class = nullptr;
			int64 Value = 0;
			bool bMulti = false;
			FText Text;
			bool bValid = false;
		};
		TSharedRef<FValueLabelCache> LabelCache = MakeShared<FValueLabelCache>();

		TSharedPtr<SComboButton> ValueCombo;
		Row->AddSlot().FillWidth(1.0f)
		[
			SAssignNew(ValueCombo, SComboButton)
			.ContentPadding(FMargin(4, 2))
			.IsEnabled_Lambda([ValueHandle]()
			{
				bool bMulti = false;
				return ReadUnanimousClass(ValueHandle, bMulti) != nullptr;
			})
			.ButtonContent()
			[
				SNew(STextBlock)
				.Text_Lambda([ValueHandle, LabelCache]()
				{
					bool bMultiClass = false;
					UEnum* Enum = ReadUnanimousClass(ValueHandle, bMultiClass);
					bool bMultiValue = false;
					const int64 Value = ReadUnanimousValue(ValueHandle, bMultiValue);
					const bool bMulti = bMultiClass || bMultiValue;

					if (LabelCache->bValid && LabelCache->Class == Enum && LabelCache->Value == Value && LabelCache->bMulti == bMulti)
					{
						return LabelCache->Text;
					}
					LabelCache->Class = Enum;
					LabelCache->Value = Value;
					LabelCache->bMulti = bMulti;
					LabelCache->Text = FormatValueButtonLabel(ValueHandle);
					LabelCache->bValid = true;
					return LabelCache->Text;
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];

		TWeakPtr<SComboButton> WeakValueCombo = ValueCombo;
		ValueCombo->SetOnGetMenuContent(FOnGetContent::CreateLambda([ValueHandle, WeakValueCombo]() -> TSharedRef<SWidget>
		{
			bool bMulti = false;
			UEnum* Enum = ReadUnanimousClass(ValueHandle, bMulti);
			if (!Enum) { return SNullWidget::NullWidget; }

			if (IsBitflagsEnum(Enum))
			{
				return SNew(SEnumBitflagsMenu)
					.ValueHandle(ValueHandle)
					.EnumClass(Enum);
			}

			return SNew(SEnumValueMenu)
				.ValueHandle(ValueHandle)
				.EnumClass(Enum)
				.OwningCombo(WeakValueCombo);
		}));

		return Row;
	}
}

#undef LOCTEXT_NAMESPACE
