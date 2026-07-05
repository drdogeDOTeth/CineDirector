// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "IShotPlanProvider.h"
#include "ShotGrammarParser.h"
#include "ShotPlanExecutor.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CineDirector"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorUI, Log, All);

namespace
{
	const FLinearColor ErrorColor(1.0f, 0.45f, 0.35f);
}

void SCineDirectorPanel::Construct(const FArguments& InArgs)
{
	Provider = InArgs._Provider;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelPrompt", "Describe the shots you want:"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(90.0f)
			[
				SAssignNew(DescriptionBox, SMultiLineEditableTextBox)
				.HintText(LOCTEXT("DescriptionHint",
					"e.g. Establishing drone shot over the Castle for 6 seconds. Then a slow close-up orbit around the Knight, 85mm, shallow focus."))
				.AutoWrapText(true)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 0.0f)
		[
			SAssignNew(ContinuousCheck, SCheckBox)
			.ToolTipText(LOCTEXT("ContinuousTooltip",
				"Chain every clause onto a single camera as one unbroken take — no cuts between moves. Writing \"one take\" or \"continuous\" in the description does the same."))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ContinuousCheck", "One continuous take (single camera, no cuts)"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 6.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("CreateShots", "Create Shots in Sequencer"))
				.ToolTipText(LOCTEXT("CreateShotsTooltip",
					"Spawns cine cameras and authors keys and camera cuts in the Level Sequence currently open in Sequencer. One undo step."))
				.OnClicked(this, &SCineDirectorPanel::OnCreateShots)
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Provider.IsValid() ? Provider->GetProviderName() : FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8.0f, 2.0f, 8.0f, 8.0f)
		[
			SNew(SScrollBox)

			+ SScrollBox::Slot()
			[
				SAssignNew(StatusBlock, STextBlock)
				.AutoWrapText(true)
			]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("BuilderTitle", "Shot builder — click to add to the prompt"))
				.InitiallyCollapsed(false)
				.BodyContent()
				[
					BuildShotBuilder()
				]
			]

			+ SScrollBox::Slot().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("VocabTitle", "What can I write?"))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(STextBlock)
					.Text(FShotGrammarParser::GetVocabularyHelpText())
					.AutoWrapText(true)
				]
			]
		]
	];
}

TSharedRef<SWidget> SCineDirectorPanel::BuildShotBuilder()
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	// ---- Preset examples: clicking one replaces the prompt -----------------------
	struct FPreset
	{
		FText Label;
		FString Prompt;
	};
	const TArray<FPreset> Presets =
	{
		{ LOCTEXT("PresetEstablish", "Establishing flyover"),
		  TEXT("Establishing drone shot over the [actor] for 6 seconds, slight film grain") },
		{ LOCTEXT("PresetOrbit", "Close-up orbit"),
		  TEXT("Slow close-up orbit around the [actor], 85mm, shallow focus") },
		{ LOCTEXT("PresetPushIn", "Handheld push-in"),
		  TEXT("Push in on the [actor] over 4 seconds, slightly handheld. Then hold a medium shot of the [actor] for 2 seconds") },
		{ LOCTEXT("PresetRack", "Rack focus"),
		  TEXT("Static medium shot of the [actor], rack focus from the [actor] to the [other actor]") },
		{ LOCTEXT("PresetRise", "Flyby & rise"),
		  TEXT("Quick flyby of the [actor], low angle. Then crane up 4 meters looking at the [actor], vignette") },
	};

	TSharedRef<SWrapBox> PresetWrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f));
	for (const FPreset& Preset : Presets)
	{
		PresetWrap->AddSlot()
		[
			SNew(SButton)
			.Text(Preset.Label)
			.ToolTipText(FText::FromString(Preset.Prompt))
			.OnClicked(this, &SCineDirectorPanel::OnUsePreset, Preset.Prompt)
		];
	}
	Box->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("PresetsLabel", "Examples (replaces the prompt — swap in your actor names):"))
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];
	Box->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[PresetWrap];

	// ---- Phrase chips: clicking one appends to the prompt --------------------------
	struct FChipGroup
	{
		FText Label;
		TArray<FString> Phrases;
	};
	const TArray<FChipGroup> Groups =
	{
		{ LOCTEXT("ChipsMove", "Move"),
			{ TEXT("orbit around"), TEXT("push in on"), TEXT("pull back from"), TEXT("truck left"), TEXT("truck right"),
			  TEXT("crane up"), TEXT("crane down"), TEXT("pan left"), TEXT("pan right"), TEXT("tilt up"), TEXT("tilt down"),
			  TEXT("zoom in on"), TEXT("zoom out"), TEXT("flyover of"), TEXT("static shot of") } },
		{ LOCTEXT("ChipsFraming", "Framing"),
			{ TEXT("extreme close-up"), TEXT("close-up"), TEXT("medium shot"), TEXT("wide shot"), TEXT("establishing shot") } },
		{ LOCTEXT("ChipsAngle", "Angle & side"),
			{ TEXT("low angle"), TEXT("high angle"), TEXT("overhead"), TEXT("from behind"), TEXT("from the left"),
			  TEXT("from the right"), TEXT("over the shoulder") } },
		{ LOCTEXT("ChipsLens", "Lens & focus"),
			{ TEXT("24mm"), TEXT("50mm"), TEXT("85mm"), TEXT("telephoto"), TEXT("f/1.8"), TEXT("shallow focus"),
			  TEXT("deep focus"), TEXT("focus on"), TEXT("rack focus from X to Y") } },
		{ LOCTEXT("ChipsEffects", "Effects"),
			{ TEXT("slightly handheld"), TEXT("handheld"), TEXT("very shaky"), TEXT("dutch angle"), TEXT("film grain"),
			  TEXT("vignette"), TEXT("chromatic aberration"), TEXT("bloom"), TEXT("lens flares") } },
		{ LOCTEXT("ChipsTiming", "Timing & amount"),
			{ TEXT("slow"), TEXT("fast"), TEXT("over 5 seconds"), TEXT("half orbit"), TEXT("full orbit"),
			  TEXT("90 degrees"), TEXT("then") } },
	};

	for (const FChipGroup& Group : Groups)
	{
		TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f));
		for (const FString& Phrase : Group.Phrases)
		{
			Wrap->AddSlot()
			[
				SNew(SButton)
				.Text(FText::FromString(Phrase))
				.OnClicked(this, &SCineDirectorPanel::OnInsertPhrase, Phrase)
			];
		}

		Box->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(Group.Label)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
		Box->AddSlot().AutoHeight()[Wrap];
	}

	return Box;
}

FReply SCineDirectorPanel::OnInsertPhrase(FString Phrase)
{
	FString Text = DescriptionBox->GetText().ToString();
	if (!Text.IsEmpty() && !Text.EndsWith(TEXT(" ")))
	{
		Text += TEXT(" ");
	}
	Text += Phrase;
	DescriptionBox->SetText(FText::FromString(Text));
	FSlateApplication::Get().SetKeyboardFocus(DescriptionBox);
	return FReply::Handled();
}

FReply SCineDirectorPanel::OnUsePreset(FString Prompt)
{
	DescriptionBox->SetText(FText::FromString(Prompt));
	FSlateApplication::Get().SetKeyboardFocus(DescriptionBox);
	return FReply::Handled();
}

FReply SCineDirectorPanel::OnCreateShots()
{
	if (!Provider.IsValid() || !DescriptionBox.IsValid())
	{
		return FReply::Handled();
	}

	const FCineSceneContext Scene = FShotPlanExecutor::BuildSceneContext();

	FCineShotPlan Plan;
	FText Error;
	if (!Provider->BuildShotPlan(DescriptionBox->GetText().ToString(), Scene, Plan, Error))
	{
		UE_LOG(LogCineDirectorUI, Warning, TEXT("Parse failed: %s"), *Error.ToString());
		StatusBlock->SetText(Error);
		StatusBlock->SetColorAndOpacity(FSlateColor(ErrorColor));
		return FReply::Handled();
	}

	if (ContinuousCheck.IsValid() && ContinuousCheck->IsChecked())
	{
		Plan.bOneContinuousShot = true;
	}

	const FCineExecuteResult Result = FShotPlanExecutor::Execute(Plan);
	if (!Result.bSuccess)
	{
		UE_LOG(LogCineDirectorUI, Warning, TEXT("Execute failed: %s"), *Result.Error.ToString());
		StatusBlock->SetText(Result.Error);
		StatusBlock->SetColorAndOpacity(FSlateColor(ErrorColor));
		return FReply::Handled();
	}

	FString Message = FString::Printf(TEXT("Created %d shot%s, %.1f s total."),
		Result.NumShots, Result.NumShots == 1 ? TEXT("") : TEXT("s"), Result.TotalDurationSeconds);
	for (const FString& Note : Result.Notes)
	{
		Message += TEXT("\n  • ") + Note;
	}
	StatusBlock->SetText(FText::FromString(Message));
	StatusBlock->SetColorAndOpacity(FSlateColor::UseForeground());

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
