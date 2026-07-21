// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class IShotPlanProvider;
class SCheckBox;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class STextBlock;

/**
 * The CineDirector tab: a description box, a Create button, and a result readout.
 * Talks to the level only through IShotPlanProvider + FShotPlanExecutor.
 */
class SCineDirectorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCineDirectorPanel) {}
		SLATE_ARGUMENT(TSharedPtr<IShotPlanProvider>, Provider)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnCreateShots();

	/** Appends a vocabulary chip's phrase to the description box. */
	FReply OnInsertPhrase(FString Phrase);

	/** Replaces the description box content with a preset example prompt. */
	FReply OnUsePreset(FString Prompt);

	/** Preset example buttons + clickable phrase chips, grouped by category. */
	TSharedRef<SWidget> BuildShotBuilder();

	/** Resolution / format / quality pickers + output folder + Render button. */
	TSharedRef<SWidget> BuildRenderSection();

	/** Queues the open sequence into Movie Render Queue with the picked options. */
	FReply OnStartRender();

	/** A combo box over string options that writes the chosen index back to SelectedIndex. */
	TSharedRef<SWidget> MakeOptionCombo(TArray<TSharedPtr<FString>>& Options, int32& SelectedIndex);

	TSharedPtr<IShotPlanProvider> Provider;
	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<SCheckBox> ContinuousCheck;
	TSharedPtr<STextBlock> StatusBlock;

	TArray<TSharedPtr<FString>> ResolutionOptions;
	TArray<TSharedPtr<FString>> FormatOptions;
	TArray<TSharedPtr<FString>> QualityOptions;
	int32 ResolutionIndex = 1;
	int32 FormatIndex = 0;
	int32 QualityIndex = 1;
	TSharedPtr<SEditableTextBox> OutputDirBox;

	TSharedPtr<SCheckBox> TrailerCheck;
	TSharedPtr<SEditableTextBox> TrailerStyleBox;
	TSharedPtr<SCheckBox> TrailerKeepAudioCheck;
	TSharedPtr<SCheckBox> TrailerScoreCheck;
	TSharedPtr<SEditableTextBox> TrailerTitleBox;
	TSharedPtr<SMultiLineEditableTextBox> TrailerCardsBox;
	TSharedPtr<SEditableTextBox> TrailerCamBox;
};
