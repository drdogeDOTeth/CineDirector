// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorPanel.h"

#include "CineRenderLauncher.h"
#include "CineTrailerProcessor.h"
#include "SCineDirectorAutoRetargetPanel.h"
#include "SCineDirectorFacePanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "IShotPlanProvider.h"
#include "ShotGrammarParser.h"
#include "ShotPlanExecutor.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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

	ResolutionOptions = {
		MakeShared<FString>(TEXT("HD 720p")),
		MakeShared<FString>(TEXT("Full HD 1080p")),
		MakeShared<FString>(TEXT("QHD 1440p")),
		MakeShared<FString>(TEXT("UHD 4K")),
	};
	FormatOptions = {
		MakeShared<FString>(TEXT("PNG sequence")),
		MakeShared<FString>(TEXT("JPEG sequence")),
		MakeShared<FString>(TEXT("EXR sequence (HDR)")),
		MakeShared<FString>(TEXT("BMP sequence")),
		MakeShared<FString>(TEXT("MP4 video (ffmpeg)")),
	};
	QualityOptions = {
		MakeShared<FString>(TEXT("Draft (fastest)")),
		MakeShared<FString>(TEXT("Standard (4x motion samples)")),
		MakeShared<FString>(TEXT("High (8x motion samples)")),
		MakeShared<FString>(TEXT("Cinematic (16x motion samples)")),
	};

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

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("AutoRetargetTitle", "Auto Retarget (IK Rig)"))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SCineDirectorAutoRetargetPanel)
				]
			]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("FaceTitle", "Face & Lipsync"))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					SNew(SCineDirectorFacePanel)
				]
			]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.AreaTitle(LOCTEXT("RenderTitle", "Render (Movie Render Queue)"))
				.InitiallyCollapsed(true)
				.BodyContent()
				[
					BuildRenderSection()
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

TSharedRef<SWidget> SCineDirectorPanel::MakeOptionCombo(TArray<TSharedPtr<FString>>& Options, int32& SelectedIndex)
{
	return SNew(SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&Options)
		.InitiallySelectedItem(Options[SelectedIndex])
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock).Text(FText::FromString(*Item));
		})
		.OnSelectionChanged_Lambda([&Options, &SelectedIndex](TSharedPtr<FString> Item, ESelectInfo::Type)
		{
			if (Item.IsValid())
			{
				SelectedIndex = Options.IndexOfByKey(Item);
			}
		})
		[
			SNew(STextBlock)
			.Text_Lambda([&Options, &SelectedIndex]()
			{
				return FText::FromString(*Options[FMath::Clamp(SelectedIndex, 0, Options.Num() - 1)]);
			})
		];
}

TSharedRef<SWidget> SCineDirectorPanel::BuildRenderSection()
{
	auto MakeLabeledCombo = [this](const FText& Label, TArray<TSharedPtr<FString>>& Options, int32& SelectedIndex) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeOptionCombo(Options, SelectedIndex)
			];
	};

	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				MakeLabeledCombo(LOCTEXT("RenderResolution", "Resolution"), ResolutionOptions, ResolutionIndex)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				MakeLabeledCombo(LOCTEXT("RenderFormat", "Format"), FormatOptions, FormatIndex)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				MakeLabeledCombo(LOCTEXT("RenderQuality", "Quality"), QualityOptions, QualityIndex)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("RenderOutputDir", "Output folder"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(OutputDirBox, SEditableTextBox)
				.Text(FText::FromString(TEXT("C:/Chiliz/UnrealRendersNew")))
				.HintText(LOCTEXT("RenderOutputDirHint", "(empty = {project}/Saved/MovieRenders)"))
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
		[
			SNew(SExpandableArea)
			.AreaTitle(LOCTEXT("TrailerModeTitle", "Trailer mode"))
			.InitiallyCollapsed(true)
			.BodyContent()
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					SAssignNew(TrailerCheck, SCheckBox)
					.ToolTipText(LOCTEXT("TrailerCheckTooltip",
						"After the render finishes, automatically cut it into a found-footage trailer: four beats with handheld shake, flicker, REC + timecode overlays, letterspaced title cards, and a synthesized drone + whistle score. Forces the MP4 format."))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TrailerCheck", "Cut a found-footage trailer after rendering (forces MP4)"))
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("TrailerStyleLabel", "Style (describe the edit)")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(TrailerStyleBox, SEditableTextBox)
					.Text(FText::FromString(TEXT("found footage, security cam, natural colors, eerie music")))
					.ToolTipText(LOCTEXT("TrailerStyleTooltip",
						"Plain language. Look: found footage / cinematic letterbox / black and white / green / warm / cold / natural. "
						"Texture: handheld, flicker, grainy, no grain. Pacing: fast cuts or slow burn. "
						"Music: eerie / tense heartbeat / somber / no music / drone only. Titles: bold title."))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("TrailerTitleLabel", "Movie title")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(TrailerTitleBox, SEditableTextBox)
					.Text(FText::FromString(TEXT("THE VISITORS")))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("TrailerCardsLabel", "Card lines (one per line, up to 3)")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).MinDesiredHeight(64.0f)
					[
						SAssignNew(TrailerCardsBox, SMultiLineEditableTextBox)
						.Text(FText::FromString(TEXT("They told us we would be safe here\nBut something came in with us\nIt was already inside")))
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("TrailerCamLabel", "Camera tag (top-right overlay)")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(TrailerCamBox, SEditableTextBox)
					.Text(FText::FromString(TEXT("SAFEHOUSE CAM 03")))
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("RenderButton", "Render Sequence"))
				.ToolTipText(LOCTEXT("RenderButtonTooltip",
					"Queues the Level Sequence currently open in Sequencer into Movie Render Queue and renders it in-editor. Higher quality tiers add sub-frame motion blur and take longer."))
				.OnClicked(this, &SCineDirectorPanel::OnStartRender)
			]
		];
}

FReply SCineDirectorPanel::OnStartRender()
{
	FCineRenderOptions Options;

	static const FIntPoint Resolutions[] = { {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160} };
	Options.Resolution = Resolutions[FMath::Clamp(ResolutionIndex, 0, 3)];

	static const FCineRenderOptions::EFormat Formats[] = {
		FCineRenderOptions::EFormat::PNG, FCineRenderOptions::EFormat::JPEG,
		FCineRenderOptions::EFormat::EXR, FCineRenderOptions::EFormat::BMP,
		FCineRenderOptions::EFormat::MP4 };
	Options.Format = Formats[FMath::Clamp(FormatIndex, 0, 4)];

	static const int32 TemporalSamples[] = { 1, 4, 8, 16 };
	Options.TemporalSamples = TemporalSamples[FMath::Clamp(QualityIndex, 0, 3)];
	Options.SpatialSamples = 1;
	Options.EncodeQuality = FMath::Clamp(QualityIndex, 0, 3);

	Options.OutputDirectory = OutputDirBox.IsValid() ? OutputDirBox->GetText().ToString().TrimStartAndEnd() : FString();

	// Trailer mode consumes an MP4, so it forces that format.
	const bool bTrailerMode = TrailerCheck.IsValid() && TrailerCheck->IsChecked();
	if (bTrailerMode)
	{
		Options.Format = FCineRenderOptions::EFormat::MP4;
	}

	FCineTrailerOptions TrailerOptions;
	TrailerOptions.OutputDirectory = Options.OutputDirectory.IsEmpty()
		? FPaths::ProjectSavedDir() / TEXT("MovieRenders")
		: Options.OutputDirectory;
	TrailerOptions.StyleDescription = TrailerStyleBox.IsValid() ? TrailerStyleBox->GetText().ToString() : FString();
	TrailerOptions.MovieTitle = TrailerTitleBox.IsValid() ? TrailerTitleBox->GetText().ToString() : TEXT("THE VISITORS");
	if (TrailerCardsBox.IsValid())
	{
		TrailerCardsBox->GetText().ToString().ParseIntoArrayLines(TrailerOptions.CardLines);
	}
	TrailerOptions.CameraTag = TrailerCamBox.IsValid() ? TrailerCamBox->GetText().ToString() : TEXT("SAFEHOUSE CAM 03");
	const FDateTime RenderStartUtc = FDateTime::UtcNow();

	TWeakPtr<SCineDirectorPanel> WeakSelf = SharedThis(this);
	FText Error;
	if (!FCineRenderLauncher::StartRender(Options, Error,
		[WeakSelf, bTrailerMode, TrailerOptions, RenderStartUtc](bool bSuccess)
		{
			const TSharedPtr<SCineDirectorPanel> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			if (!bSuccess)
			{
				Self->StatusBlock->SetText(LOCTEXT("RenderFailed", "Render did not complete — check the Output Log for details."));
				Self->StatusBlock->SetColorAndOpacity(FSlateColor(ErrorColor));
				return;
			}

			if (!bTrailerMode)
			{
				Self->StatusBlock->SetText(LOCTEXT("RenderDone", "Render finished — check the output folder."));
				Self->StatusBlock->SetColorAndOpacity(FSlateColor::UseForeground());
				return;
			}

			FCineTrailerOptions RunOptions = TrailerOptions;
			RunOptions.SourceVideo = FCineTrailerProcessor::FindNewestMp4(TrailerOptions.OutputDirectory, RenderStartUtc);
			if (RunOptions.SourceVideo.IsEmpty())
			{
				Self->StatusBlock->SetText(LOCTEXT("TrailerNoSource", "Render finished, but no new MP4 was found in the output folder to cut."));
				Self->StatusBlock->SetColorAndOpacity(FSlateColor(ErrorColor));
				return;
			}

			Self->StatusBlock->SetText(LOCTEXT("TrailerCutting", "Render finished — cutting the trailer…"));
			Self->StatusBlock->SetColorAndOpacity(FSlateColor::UseForeground());

			FCineTrailerProcessor::ProcessAsync(RunOptions,
				[WeakSelf](bool bTrailerSuccess, FString ResultOrError)
				{
					if (const TSharedPtr<SCineDirectorPanel> Inner = WeakSelf.Pin())
					{
						Inner->StatusBlock->SetText(bTrailerSuccess
							? FText::Format(LOCTEXT("TrailerDoneFmt", "Trailer cut: {0}"), FText::FromString(ResultOrError))
							: FText::Format(LOCTEXT("TrailerFailFmt", "Trailer failed: {0}"), FText::FromString(ResultOrError)));
						Inner->StatusBlock->SetColorAndOpacity(bTrailerSuccess ? FSlateColor::UseForeground() : FSlateColor(ErrorColor));
					}
				});
		}))
	{
		UE_LOG(LogCineDirectorUI, Warning, TEXT("Render start failed: %s"), *Error.ToString());
		StatusBlock->SetText(Error);
		StatusBlock->SetColorAndOpacity(FSlateColor(ErrorColor));
		return FReply::Handled();
	}

	StatusBlock->SetText(bTrailerMode
		? LOCTEXT("RenderStartedTrailer", "Rendering… the trailer will be cut automatically when the render finishes.")
		: LOCTEXT("RenderStarted", "Rendering… a Movie Render Queue window will show progress. The editor stays busy until it finishes."));
	StatusBlock->SetColorAndOpacity(FSlateColor::UseForeground());
	return FReply::Handled();
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
		{ LOCTEXT("PresetSunset", "Sunset mood"),
		  TEXT("Slow half orbit around the [actor] at sunset, light fog, god rays. Then push in on the [actor] at dusk, heavy fog") },
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
		{ LOCTEXT("ChipsAngle", "Angle, side & aim"),
			{ TEXT("low angle"), TEXT("high angle"), TEXT("overhead"), TEXT("from behind"), TEXT("from the left"),
			  TEXT("from the right"), TEXT("from its left"), TEXT("from its right"), TEXT("from behind it"),
			  TEXT("over the shoulder"), TEXT("looking at"), TEXT("track") } },
		{ LOCTEXT("ChipsLens", "Lens & focus"),
			{ TEXT("24mm"), TEXT("50mm"), TEXT("85mm"), TEXT("telephoto"), TEXT("f/1.8"), TEXT("shallow focus"),
			  TEXT("deep focus"), TEXT("focus on"), TEXT("rack focus from X to Y") } },
		{ LOCTEXT("ChipsEffects", "Effects"),
			{ TEXT("slightly handheld"), TEXT("handheld"), TEXT("very shaky"), TEXT("dutch angle"), TEXT("film grain"),
			  TEXT("vignette"), TEXT("chromatic aberration"), TEXT("bloom"), TEXT("lens flares") } },
		{ LOCTEXT("ChipsLighting", "Light & sky"),
			{ TEXT("at dawn"), TEXT("morning"), TEXT("at noon"), TEXT("golden hour"), TEXT("at sunset"), TEXT("at dusk"),
			  TEXT("at night"), TEXT("midnight"), TEXT("overcast"), TEXT("light fog"), TEXT("heavy fog"), TEXT("no fog"),
			  TEXT("god rays"), TEXT("volumetric fog") } },
		{ LOCTEXT("ChipsTiming", "Timing & amount"),
			{ TEXT("slow"), TEXT("fast"), TEXT("over 5 seconds"), TEXT("half orbit"), TEXT("full orbit"),
			  TEXT("90 degrees"), TEXT("then") } },
	};

	// Each category is its own collapsible dropdown so the builder stays compact.
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

		Box->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SExpandableArea)
			.AreaTitle(Group.Label)
			.InitiallyCollapsed(true)
			.BodyContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.0f, 4.0f))
				[
					Wrap
				]
			]
		];
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
