// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class IShotPlanProvider;
class SCheckBox;
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

	TSharedPtr<IShotPlanProvider> Provider;
	TSharedPtr<SMultiLineEditableTextBox> DescriptionBox;
	TSharedPtr<SCheckBox> ContinuousCheck;
	TSharedPtr<STextBlock> StatusBlock;
};
