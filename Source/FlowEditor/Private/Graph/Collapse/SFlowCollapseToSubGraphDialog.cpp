// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "SFlowCollapseToSubGraphDialog.h"

#include "FlowAsset.h"
#include "FlowCollapseToSubGraphPlan.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SFlowCollapseToSubGraphDialog"

void SFlowCollapseToSubGraphDialog::Construct(const FArguments& InArgs)
{
	SourceAsset = InArgs._SourceAsset;
	AssetName = FlowCollapseToSubGraph::GetRequiredAssetNamePrefix();

	const FString PackageFolder = SourceAsset.Get()
		? FPackageName::GetLongPackagePath(SourceAsset->GetOutermost()->GetName())
		: FString();

	TSharedRef<SVerticalBox> WarningsBox = SNew(SVerticalBox);
	for (const FText& Warning : InArgs._Warnings)
	{
		WarningsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(Warning)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f)))
		];
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("TargetFolder", "The sub-graph will be created in {0}"), FText::FromString(PackageFolder)))
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f)
		[
			SAssignNew(AssetNameTextBox, SEditableTextBox)
			.Text(FText::FromString(AssetName))
			.SelectAllTextWhenFocused(false)
			.OnTextChanged(this, &SFlowCollapseToSubGraphDialog::OnAssetNameChanged)
			.OnTextCommitted(this, &SFlowCollapseToSubGraphDialog::OnAssetNameCommitted)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(this, &SFlowCollapseToSubGraphDialog::GetValidationErrorText)
			.Visibility(this, &SFlowCollapseToSubGraphDialog::GetErrorVisibility)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f)))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			WarningsBox
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Create", "Create"))
				.IsEnabled(this, &SFlowCollapseToSubGraphDialog::IsCreateEnabled)
				.OnClicked(this, &SFlowCollapseToSubGraphDialog::OnCreateClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked(this, &SFlowCollapseToSubGraphDialog::OnCancelClicked)
			]
		]
	];

	OnAssetNameChanged(FText::FromString(AssetName));
}

void SFlowCollapseToSubGraphDialog::OnAssetNameChanged(const FText& NewName)
{
	AssetName = NewName.ToString();

	ValidationError = SourceAsset.Get()
		? FlowCollapseToSubGraph::ValidateNewAssetName(*SourceAsset, AssetName)
		: LOCTEXT("NoSourceAsset", "No source asset.");

	if (AssetNameTextBox.IsValid())
	{
		AssetNameTextBox->SetError(ValidationError);
	}
}

void SFlowCollapseToSubGraphDialog::OnAssetNameCommitted(const FText& NewName, const ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter && IsCreateEnabled())
	{
		OnCreateClicked();
	}
}

FText SFlowCollapseToSubGraphDialog::GetValidationErrorText() const
{
	return ValidationError;
}

EVisibility SFlowCollapseToSubGraphDialog::GetErrorVisibility() const
{
	return ValidationError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

bool SFlowCollapseToSubGraphDialog::IsCreateEnabled() const
{
	return ValidationError.IsEmpty();
}

FReply SFlowCollapseToSubGraphDialog::OnCreateClicked()
{
	bConfirmed = true;

	if (const TSharedPtr<SWindow> OwningWindow = FSlateApplication::Get().FindWidgetWindow(AsShared()))
	{
		OwningWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

FReply SFlowCollapseToSubGraphDialog::OnCancelClicked()
{
	bConfirmed = false;

	if (const TSharedPtr<SWindow> OwningWindow = FSlateApplication::Get().FindWidgetWindow(AsShared()))
	{
		OwningWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

bool SFlowCollapseToSubGraphDialog::ShowModal(const UFlowAsset& SourceAsset, const TArray<FText>& Warnings, FString& OutAssetName)
{
	TSharedRef<SFlowCollapseToSubGraphDialog> Dialog = SNew(SFlowCollapseToSubGraphDialog)
		.SourceAsset(&SourceAsset)
		.Warnings(Warnings);

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Create Sub-Graph from Selection"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(16.0f)
			[
				SNew(SBox)
				.WidthOverride(480.0f)
				[
					Dialog
				]
			]
		];

	FSlateApplication::Get().AddModalWindow(Window, FGlobalTabmanager::Get()->GetRootWindow());

	if (!Dialog->bConfirmed)
	{
		return false;
	}

	OutAssetName = Dialog->AssetName;
	return true;
}

#undef LOCTEXT_NAMESPACE
