// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorAutoRetargetPanel.h"

#include "AssetRegistry/AssetData.h"
#include "CineDirectorIKRigGenerator.h"
#include "CineDirectorSkeletonAnalyzer.h"
#include "ContentBrowserModule.h"
#include "Engine/SkeletalMesh.h"
#include "IContentBrowserSingleton.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CineDirectorAutoRetarget"

void SCineDirectorAutoRetargetPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SScrollBox)

		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Intro", "Analyze a Skeletal Mesh, review every proposed chain, then generate a saved IK Rig."))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				SAssignNew(SelectedMeshText, STextBlock)
				.Text(LOCTEXT("NoMesh", "Selected Skeletal Mesh: none"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("UseSelected", "Use Content Browser Selection"))
					.OnClicked(this, &SCineDirectorAutoRetargetPanel::OnUseSelectedMesh)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Analyze", "Analyze Skeleton"))
					.OnClicked(this, &SCineDirectorAutoRetargetPanel::OnAnalyze)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
			[
				SAssignNew(RigSummaryText, STextBlock)
				.Text(LOCTEXT("RigUnknown", "Detected Rig Type: Not analyzed"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MappingHeader", "Chain mappings (editable before generation)"))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(MappingBox, SVerticalBox)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("OutputLabel", "Output path (blank = beside the Skeletal Mesh)"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(OutputPathBox, SEditableTextBox)
				.HintText(LOCTEXT("OutputHint", "/Game/Characters/Retarget"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ConflictBehavior", "Existing asset behavior: a unique name is always created; no existing asset is overwritten."))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Generate", "Generate IK Rig"))
					.OnClicked(this, &SCineDirectorAutoRetargetPanel::OnGenerate)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SaveProfile", "Save Profile"))
					.OnClicked(this, &SCineDirectorAutoRetargetPanel::OnSaveProfile)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadProfile", "Load Selected Profile"))
					.ToolTipText(LOCTEXT("LoadProfileTip", "Select a CineDirector Retarget Profile in the Content Browser, then load it for this skeleton."))
					.OnClicked(this, &SCineDirectorAutoRetargetPanel::OnLoadSelectedProfile)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
			[
				SAssignNew(StatusText, STextBlock)
				.AutoWrapText(true)
			]
		]
	];
}

FReply SCineDirectorAutoRetargetPanel::OnUseSelectedMesh()
{
	if (USkeletalMesh* Mesh = FindSelectedSkeletalMesh())
	{
		SelectedMesh = Mesh;
		SelectedMeshText->SetText(FText::Format(LOCTEXT("SelectedMesh", "Selected Skeletal Mesh: {0}"), FText::FromString(Mesh->GetPathName())));
		SetStatus(LOCTEXT("MeshSelected", "Mesh selected. Analyze it to inspect the proposed mappings."));
	}
	else
	{
		SetStatus(LOCTEXT("NoSkeletalMesh", "Select exactly one Skeletal Mesh in the Content Browser."), true);
	}
	return FReply::Handled();
}

FReply SCineDirectorAutoRetargetPanel::OnAnalyze()
{
	if (!SelectedMesh.IsValid())
	{
		OnUseSelectedMesh();
	}
	if (!SelectedMesh.IsValid())
	{
		return FReply::Handled();
	}

	FText Error;
	if (!FCineDirectorSkeletonAnalyzer::Analyze(SelectedMesh.Get(), Analysis, Error))
	{
		SetStatus(Error, true);
		return FReply::Handled();
	}

	RigSummaryText->SetText(FText::Format(
		LOCTEXT("RigSummary", "Detected Rig Type: {0} - overall confidence {1}% - signature {2}"),
		RigTypeText(Analysis.RigType),
		FText::AsNumber(FMath::RoundToInt(Analysis.OverallConfidence * 100.0f)),
		FText::FromString(Analysis.SkeletonSignature)));
	RefreshMappingTable();

	FString WarningText;
	for (const FString& Warning : Analysis.Warnings)
	{
		WarningText += WarningText.IsEmpty() ? Warning : TEXT("\n");
		WarningText += Warning;
	}
	SetStatus(WarningText.IsEmpty() ? LOCTEXT("AnalysisComplete", "Analysis complete. Review the mappings, especially low-confidence rows, before generating.") : FText::FromString(WarningText), !WarningText.IsEmpty());
	return FReply::Handled();
}

FReply SCineDirectorAutoRetargetPanel::OnGenerate()
{
	SyncEditsToAnalysis();
	FCineDirectorIKRigGenerationResult Result = FCineDirectorIKRigGenerator::GenerateIKRig(
		SelectedMesh.Get(),
		Analysis,
		OutputPathBox.IsValid() ? OutputPathBox->GetText().ToString() : FString());
	if (!Result.bSuccess)
	{
		SetStatus(Result.Error, true);
		return FReply::Handled();
	}
	SetStatus(FText::Format(LOCTEXT("Generated", "Generated and saved IK Rig: {0}"), FText::FromString(Result.AssetPath)));
	return FReply::Handled();
}

FReply SCineDirectorAutoRetargetPanel::OnSaveProfile()
{
	SyncEditsToAnalysis();
	FText Error;
	UCineDirectorRetargetProfile* Profile = FCineDirectorIKRigGenerator::SaveProfile(
		Analysis,
		OutputPathBox.IsValid() ? OutputPathBox->GetText().ToString() : FString(),
		Error);
	if (!Profile)
	{
		SetStatus(Error, true);
		return FReply::Handled();
	}
	SetStatus(FText::Format(LOCTEXT("ProfileSaved", "Saved reusable profile: {0}"), FText::FromString(Profile->GetPathName())));
	return FReply::Handled();
}

FReply SCineDirectorAutoRetargetPanel::OnLoadSelectedProfile()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
	if (SelectedAssets.Num() != 1)
	{
		SetStatus(LOCTEXT("SelectProfile", "Select one CineDirector Retarget Profile in the Content Browser to load it."), true);
		return FReply::Handled();
	}

	UCineDirectorRetargetProfile* Profile = Cast<UCineDirectorRetargetProfile>(SelectedAssets[0].GetAsset());
	if (!Profile)
	{
		SetStatus(LOCTEXT("WrongProfile", "The selected asset is not a CineDirector Retarget Profile."), true);
		return FReply::Handled();
	}
	if (!Analysis.SkeletonSignature.IsEmpty() && Profile->SkeletonSignature != Analysis.SkeletonSignature)
	{
		SetStatus(LOCTEXT("ProfileSignatureMismatch", "This profile belongs to a different skeleton signature. Analyze the matching mesh before loading it."), true);
		return FReply::Handled();
	}

	Analysis.RigType = Profile->RigType;
	Analysis.RetargetRoot = Profile->RetargetRoot;
	Analysis.SkeletonSignature = Profile->SkeletonSignature;
	Analysis.ChainMappings = Profile->ChainMappings;
	RefreshMappingTable();
	SetStatus(LOCTEXT("ProfileLoaded", "Profile loaded. Verify the mappings and generate an IK Rig when ready."));
	return FReply::Handled();
}

USkeletalMesh* SCineDirectorAutoRetargetPanel::FindSelectedSkeletalMesh() const
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
	return SelectedAssets.Num() == 1 ? Cast<USkeletalMesh>(SelectedAssets[0].GetAsset()) : nullptr;
}

void SCineDirectorAutoRetargetPanel::SyncEditsToAnalysis()
{
	for (const FMappingEditors& Editors : MappingEditors)
	{
		if (Analysis.ChainMappings.IsValidIndex(Editors.MappingIndex))
		{
			FCineDirectorChainMapping& Mapping = Analysis.ChainMappings[Editors.MappingIndex];
			Mapping.StartBone = Editors.Start.IsValid() ? FName(*Editors.Start->GetText().ToString().TrimStartAndEnd()) : Mapping.StartBone;
			Mapping.EndBone = Editors.End.IsValid() ? FName(*Editors.End->GetText().ToString().TrimStartAndEnd()) : Mapping.EndBone;
		}
	}
}

void SCineDirectorAutoRetargetPanel::RefreshMappingTable()
{
	MappingEditors.Reset();
	MappingBox->ClearChildren();

	TSharedRef<SGridPanel> Header = SNew(SGridPanel);
	Header->AddSlot(0, 0).Padding(2.0f)[SNew(STextBlock).Text(LOCTEXT("ChainColumn", "Chain"))];
	Header->AddSlot(1, 0).Padding(2.0f)[SNew(STextBlock).Text(LOCTEXT("StartColumn", "Start bone"))];
	Header->AddSlot(2, 0).Padding(2.0f)[SNew(STextBlock).Text(LOCTEXT("EndColumn", "End bone"))];
	Header->AddSlot(3, 0).Padding(2.0f)[SNew(STextBlock).Text(LOCTEXT("ConfidenceColumn", "Confidence / status"))];
	MappingBox->AddSlot().AutoHeight()[Header];

	for (int32 Index = 0; Index < Analysis.ChainMappings.Num(); ++Index)
	{
		const FCineDirectorChainMapping& Mapping = Analysis.ChainMappings[Index];
		FMappingEditors Editors;
		Editors.MappingIndex = Index;
		TSharedRef<SGridPanel> Row = SNew(SGridPanel);
		Row->AddSlot(0, 0).Padding(2.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromName(Mapping.ChainName))];
		Row->AddSlot(1, 0).Padding(2.0f)[SAssignNew(Editors.Start, SEditableTextBox).Text(FText::FromName(Mapping.StartBone)).MinDesiredWidth(115.0f)];
		Row->AddSlot(2, 0).Padding(2.0f)[SAssignNew(Editors.End, SEditableTextBox).Text(FText::FromName(Mapping.EndBone)).MinDesiredWidth(115.0f)];
		const FString Status = FString::Printf(TEXT("%d%% %s"), FMath::RoundToInt(Mapping.Confidence * 100.0f), *Mapping.Warning);
		Row->AddSlot(3, 0).Padding(2.0f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FText::FromString(Status)).AutoWrapText(true)];
		MappingBox->AddSlot().AutoHeight()[Row];
		MappingEditors.Add(MoveTemp(Editors));
	}
}

void SCineDirectorAutoRetargetPanel::SetStatus(const FText& Message, bool bIsError)
{
	StatusText->SetText(Message);
	StatusText->SetColorAndOpacity(bIsError ? FSlateColor(FLinearColor(1.0f, 0.45f, 0.35f)) : FSlateColor::UseForeground());
}

FText SCineDirectorAutoRetargetPanel::RigTypeText(ECineDirectorRigType RigType)
{
	switch (RigType)
	{
	case ECineDirectorRigType::Humanoid: return LOCTEXT("Humanoid", "Humanoid");
	case ECineDirectorRigType::Quadruped: return LOCTEXT("Quadruped", "Quadruped");
	default: return LOCTEXT("Unknown", "Unknown");
	}
}

#undef LOCTEXT_NAMESPACE
