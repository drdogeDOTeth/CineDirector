// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineFaceTypes.h"
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SCompoundWidget.h"

class AActor;
class SCheckBox;
class SEditableTextBox;
class SSlider;
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
	void RefreshSliderLabels();

	TWeakObjectPtr<AActor> TargetActor;
	TSharedPtr<STextBlock> TargetLabel;
	TSharedPtr<SEditableTextBox> AudioPathBox;
	TSharedPtr<SEditableTextBox> EmotionBox;
	TSharedPtr<SEditableTextBox> DurationBox;
	TSharedPtr<SCheckBox> TalkingCheck;
	TSharedPtr<SCheckBox> BlinkCheck;
	TSharedPtr<SCheckBox> IsolateVoiceCheck;
	TSharedPtr<STextBlock> StatusBlock;

	// Tuning sliders (persisted for the editor session).
	// 1.0 = as analyzed; layered ARKit faces also get a mild baker ease.
	// Bump toward 1.3–1.5 only if a VRM/GLB mouth still looks timid.
	float MouthStrength = 1.0f;
	/** 1.0 = full pose table; >1 pushes harder (clamped in baker). */
	float EmotionStrength = 1.0f;
	/** How crisply visemes hit: <1 soft/mumbly, 1 = as analyzed, >1 snappy enunciation. */
	float Articulation = 1.0f;
	float IsolateStrength = 0.75f; // 0 = raw audio, 1 = full isolation blend

	TSharedPtr<STextBlock> MouthStrengthLabel;
	TSharedPtr<STextBlock> EmotionStrengthLabel;
	TSharedPtr<STextBlock> ArticulationLabel;
	TSharedPtr<STextBlock> IsolateStrengthLabel;
};
