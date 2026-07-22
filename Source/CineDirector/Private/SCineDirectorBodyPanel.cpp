// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorBodyPanel.h"

#include "CineBodyAuthor.h"
#include "CineBodyGrammar.h"
#include "CineBodyRig.h"
#include "CineFaceBaker.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Selection.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CineDirectorBodyPanel"

namespace
{
	const FLinearColor BodyErrorColor(1.0f, 0.45f, 0.35f);
}

void SCineDirectorBodyPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("UseSelected", "Use Selected Actor"))
				.ToolTipText(LOCTEXT("UseSelectedTip", "Target the actor currently selected in the viewport/outliner. Needs a skeletal mesh on a supported rig (void-family VRM or Mixamo naming)."))
				.OnClicked(this, &SCineDirectorBodyPanel::OnUseSelectedActor)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(TargetLabel, STextBlock)
				.Text(LOCTEXT("NoTarget", "No character selected"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(110.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("PerformanceLabel", "Performance"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(DescriptionBox, SEditableTextBox)
				.HintText(LOCTEXT("PerformanceHint", "e.g. \"sitting on the bench smoking, nervous, looking around, for 20 seconds\""))
				.ToolTipText(FCineBodyGrammar::GetVocabularyHelpText())
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(LOCTEXT("Generate", "Generate Body Performance"))
			.ToolTipText(LOCTEXT("GenerateTip", "Authors a from-scratch keyframed body animation (poses, arm IK, fingers, breathing, mood ticks) and layers it onto the character in the open Level Sequence. Face & Lipsync output stacks on top on the same actor."))
			.OnClicked(this, &SCineDirectorBodyPanel::OnGenerate)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SAssignNew(StatusBlock, STextBlock)
			.AutoWrapText(true)
		]
	];
}

FReply SCineDirectorBodyPanel::OnUseSelectedActor()
{
	AActor* Selected = GEditor ? GEditor->GetSelectedActors()->GetTop<AActor>() : nullptr;
	if (!Selected)
	{
		SetStatus(TEXT("Nothing selected — click a character in the viewport or outliner first."), true);
		return FReply::Handled();
	}
	TargetActor = Selected;
	FString RigError;
	if (USkeletalMesh* Mesh = GetTargetMesh(RigError))
	{
		TargetLabel->SetText(FText::FromString(FString::Printf(TEXT("%s (%s)"), *Selected->GetActorLabel(), *Mesh->GetName())));
		SetStatus(TEXT(""));
	}
	else
	{
		TargetLabel->SetText(FText::FromString(Selected->GetActorLabel()));
		SetStatus(RigError.IsEmpty()
			? FString::Printf(TEXT("'%s' has no skeletal mesh component."), *Selected->GetActorLabel())
			: RigError, true);
	}
	return FReply::Handled();
}

USkeletalMesh* SCineDirectorBodyPanel::GetTargetMesh(FString& OutRigError) const
{
	const AActor* Actor = TargetActor.Get();
	if (!Actor)
	{
		return nullptr;
	}
	TArray<USkeletalMeshComponent*> Components;
	Actor->GetComponents<USkeletalMeshComponent>(Components);
	// Pick the first mesh whose skeleton resolves onto a supported body scheme.
	for (const USkeletalMeshComponent* Component : Components)
	{
		USkeletalMesh* Mesh = Component ? Component->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			continue;
		}
		FCineBodyRig Rig;
		FString Error;
		if (FCineBodyRig::Analyze(Mesh, Rig, Error))
		{
			OutRigError.Empty();
			return Mesh;
		}
		OutRigError = Error;
	}
	return nullptr;
}

FReply SCineDirectorBodyPanel::OnGenerate()
{
	AActor* Actor = TargetActor.Get();
	FString RigError;
	USkeletalMesh* Mesh = GetTargetMesh(RigError);
	if (!Actor || !Mesh)
	{
		SetStatus(RigError.IsEmpty()
			? TEXT("Pick a character with a skeletal mesh first (Use Selected Actor).")
			: RigError, true);
		return FReply::Handled();
	}

	FCineBodyRig Rig;
	FString Error;
	if (!FCineBodyRig::Analyze(Mesh, Rig, Error))
	{
		SetStatus(Error, true);
		return FReply::Handled();
	}

	FString Description = DescriptionBox->GetText().ToString().TrimStartAndEnd();
	if (Description.IsEmpty())
	{
		Description = TEXT("standing idle");
	}
	const FCineBodySpec Spec = FCineBodyGrammar::Parse(Description, GetTypeHash(Actor->GetActorLabel()));
	FCineBodyAnimDef Anim = FCineBodyAuthor::Build(Rig, Spec);
	Anim.Name = FString::Printf(TEXT("%s_%s_%s"), *Mesh->GetName(), *Anim.Name,
		*FDateTime::Now().ToString(TEXT("%m%d_%H%M%S")));

	TArray<UPackage*> Packages;
	UAnimSequence* Baked = CineBodyRigOps::Bake(Rig, Anim, TEXT("/Game/CineDirector/BodyAnims"), Packages, Error);
	if (!Baked)
	{
		SetStatus(Error, true);
		return FReply::Handled();
	}
	CineBodyRigOps::WritePreviewSheet(Rig, Anim, FPaths::ProjectSavedDir() / TEXT("CineDirectorBody"));
	UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages*/ false, /*bSaveContentPackages*/ true);

	if (!FCineFaceBaker::AddToSequencer(Actor, Baked, /*Audio*/ nullptr, Error))
	{
		SetStatus(FString::Printf(TEXT("Baked %s, but: %s"), *Baked->GetName(), *Error), true);
		return FReply::Handled();
	}

	SetStatus(FString::Printf(TEXT("Done: %s — understood as: %s. Layered onto '%s'."),
		*Baked->GetName(), *Spec.Describe(), *Actor->GetActorLabel()));
	return FReply::Handled();
}

void SCineDirectorBodyPanel::SetStatus(const FString& Message, bool bIsError)
{
	StatusBlock->SetText(FText::FromString(Message));
	StatusBlock->SetColorAndOpacity(bIsError ? FSlateColor(BodyErrorColor) : FSlateColor::UseForeground());
}

#undef LOCTEXT_NAMESPACE
