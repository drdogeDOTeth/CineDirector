// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineFaceTypes.h"
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SCompoundWidget.h"

class AActor;
class SCheckBox;
class SEditableTextBox;
class STextBlock;
class USkeletalMesh;

/**
 * Face & Lipsync section: pick a character, optionally point at dialogue
 * audio, describe the emotion, and generate a face animation layered over the
 * body animation in the open Level Sequence.
 */
class SCineDirectorFacePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCineDirectorFacePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnUseSelectedActor();
	FReply OnBrowseAudio();
	FReply OnAnalyzeFace();
	FReply OnGenerate();

	USkeletalMesh* GetTargetMesh() const;
	void SetStatus(const FString& Message, bool bIsError = false);

	TWeakObjectPtr<AActor> TargetActor;
	TSharedPtr<STextBlock> TargetLabel;
	TSharedPtr<SEditableTextBox> AudioPathBox;
	TSharedPtr<SEditableTextBox> EmotionBox;
	TSharedPtr<SEditableTextBox> DurationBox;
	TSharedPtr<SCheckBox> TalkingCheck;
	TSharedPtr<SCheckBox> BlinkCheck;
	TSharedPtr<STextBlock> StatusBlock;
};
