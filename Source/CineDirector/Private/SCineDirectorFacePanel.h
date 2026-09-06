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
	FReply OnPurgeUnusedFaceAnims();
	FReply OnClearFaceCache();

	FReply OnSaveCalibration();
	FReply OnResetCalibration();
	FReply OnPreviewNeutral();
	FReply OnClearNeutralPreview();
	USkeletalMesh* GetTargetMesh() const;
	void SetStatus(const FString& Message, bool bIsError = false);
	void RefreshSliderLabels();

	void LoadCalibrationProfile();
	void SaveCalibrationProfile() const;
	void ApplyNeutralPreview(bool bClear);
	TWeakObjectPtr<AActor> TargetActor;
	TSharedPtr<STextBlock> TargetLabel;
	TSharedPtr<SEditableTextBox> AudioPathBox;
	TSharedPtr<SEditableTextBox> EmotionBox;
	TSharedPtr<SEditableTextBox> DurationBox;
	TSharedPtr<SCheckBox> TalkingCheck;
	TSharedPtr<SCheckBox> BlinkCheck;
	TSharedPtr<SCheckBox> IsolateVoiceCheck;
	/** Opt-in MetaHuman-style ARKit mouth layering + jaw co-articulation on dual voids. */
	TSharedPtr<SCheckBox> LayeredArkitMouthCheck;
	/**
	 * When checked, each generate creates a unique timestamped asset.
	 * Default off: reuses Mesh_Face so storage does not accumulate.
	 */
	TSharedPtr<SCheckBox> KeepNewTakeCheck;
	TSharedPtr<STextBlock> StatusBlock;

	// Tuning sliders (persisted for the editor session).
	// 1.0 = as analyzed; layered ARKit faces also get a mild baker ease.
	// Bump toward 1.3–1.5 only if a VRM/GLB mouth still looks timid.
	float MouthStrength = 1.0f;
	/** 1.0 = full pose table; >1 pushes harder (clamped in baker). */
	// NVIDIA A2E also uses 0.6 as its production emotion-strength default;
	// this leaves headroom for character-specific full-face + micro shapes.
	float EmotionStrength = 0.6f;
	/** How crisply visemes hit: <1 soft/mumbly, 1 = as analyzed, >1 snappy enunciation. */
	float Articulation = 1.0f;
	float IsolateStrength = 1.0f; // 0 = raw audio, 1 = full Demucs vocal stem

	/** Per-mesh gain and neutral-offset profile. */
	FCineFaceCalibration Calibration;
	TSharedPtr<STextBlock> MouthStrengthLabel;
	TSharedPtr<STextBlock> EmotionStrengthLabel;
	TSharedPtr<STextBlock> ArticulationLabel;
	TSharedPtr<STextBlock> IsolateStrengthLabel;
};
