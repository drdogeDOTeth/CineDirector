// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorFacePanel.h"

#include "CineFaceAnalyzer.h"
#include "CineFaceBaker.h"
#include "CineLipsync.h"
#include "Templates/Function.h"
#include "Components/SkeletalMeshComponent.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "IDesktopPlatform.h"
#include "Selection.h"
#include "Misc/ConfigCacheIni.h"
#include "Sound/SoundWave.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CineDirectorFacePanel"

namespace
{
	const FLinearColor FaceErrorColor(1.0f, 0.45f, 0.35f);

	FString CalibrationSection(const USkeletalMesh* Mesh)
	{
		return Mesh ? FString::Printf(TEXT("CineDirector.FaceCalibration.%08X"), GetTypeHash(Mesh->GetPathName())) : FString();
	}
}

void SCineDirectorFacePanel::Construct(const FArguments& InArgs)
{
	auto MakeRow = [](const FText& Label, TSharedRef<SWidget> Content) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(110.0f)
				[
					SNew(STextBlock).Text(Label)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Content
			];
	};

	// Note: do not capture float& parameters into long-lived lambdas — they dangle
	// after Construct returns. Bind explicitly through `this` members.
	auto MakeStrengthSlider = [this](
		TSharedPtr<STextBlock>& ValueLabel,
		TFunction<float()> GetValue,
		TFunction<void(float)> SetValue,
		float MinV,
		float MaxV,
		const FText& Tooltip) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SSlider)
				.ToolTipText(Tooltip)
				.Value_Lambda([GetValue]() { return GetValue(); })
				.OnValueChanged_Lambda([this, SetValue](float V)
				{
					SetValue(V);
					RefreshSliderLabels();
				})
				.MinValue(MinV)
				.MaxValue(MaxV)
				.StepSize(0.05f)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(42.0f)
				[
					SAssignNew(ValueLabel, STextBlock)
					.Justification(ETextJustify::Right)
				]
			];
	};


	auto MakeCalibrationRow = [this](float FCineFaceCalibration::* GainMember,
		float FCineFaceCalibration::* OffsetMember, const FText& Tooltip) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("CalibrationGain", "Gain"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.MinValue(0.0f).MaxValue(2.0f).Delta(0.05f)
				.Value_Lambda([this, GainMember]() { return Calibration.*GainMember; })
				.OnValueChanged_Lambda([this, GainMember](float V) { Calibration.*GainMember = V; })
				.ToolTipText(Tooltip)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("CalibrationOffset", "Neutral"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.MinValue(-0.25f).MaxValue(0.25f).Delta(0.01f)
				.Value_Lambda([this, OffsetMember]() { return Calibration.*OffsetMember; })
				.OnValueChanged_Lambda([this, OffsetMember](float V) { Calibration.*OffsetMember = V; })
				.ToolTipText(LOCTEXT("NeutralOffsetTip", "Value added after gain. Use Preview Neutral to inspect the character's idle/rest pose."))
			];
	};
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
				.ToolTipText(LOCTEXT("UseSelectedTip", "Target the actor currently selected in the viewport/outliner. It needs a skeletal mesh with facial morph targets (or a MetaHuman face)."))
				.OnClicked(this, &SCineDirectorFacePanel::OnUseSelectedActor)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(TargetLabel, STextBlock)
				.Text(LOCTEXT("NoTarget", "No character selected"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Analyze", "Analyze Face"))
				.ToolTipText(LOCTEXT("AnalyzeTip", "Scan the character's morph targets and show how they map onto CineDirector's face slots."))
				.OnClicked(this, &SCineDirectorFacePanel::OnAnalyzeFace)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("AudioLabel", "Dialogue audio"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(AudioPathBox, SEditableTextBox)
					.HintText(LOCTEXT("AudioHint", "Optional: path to a .wav/.mp3 — drives the lipsync and is added to the sequence"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Browse", "..."))
					.OnClicked(this, &SCineDirectorFacePanel::OnBrowseAudio)
				])
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("EmotionLabel", "Emotion"),
				SAssignNew(EmotionBox, SEditableTextBox)
				.HintText(LOCTEXT("EmotionHint", "Optional — leave blank to auto-detect from audio (or type: angry, happy, sad…)"))
				.ToolTipText(FText::FromString(
					FString(TEXT("Leave empty to infer emotion from the dialogue audio. "))
					+ FCineFaceBaker::GetEmotionVocabulary())))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("DurationLabel", "Duration (s)"),
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox).WidthOverride(60.0f)
					[
						SAssignNew(DurationBox, SEditableTextBox)
						.Text(FText::FromString(TEXT("6")))
						.ToolTipText(LOCTEXT("DurationTip", "Used when there is no audio file; with audio, the audio's length wins."))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(16.0f, 0.0f, 0.0f, 0.0f).VAlign(VAlign_Center)
				[
					SAssignNew(TalkingCheck, SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					.ToolTipText(LOCTEXT("TalkingTip", "With audio: lipsync to it. Without: synthesize natural-looking talking. Unchecked: emotion and blinks only."))
					[
						SNew(STextBlock).Text(LOCTEXT("Talking", "Talking"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(12.0f, 0.0f, 0.0f, 0.0f).VAlign(VAlign_Center)
				[
					SAssignNew(BlinkCheck, SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					[
						SNew(STextBlock).Text(LOCTEXT("Blinks", "Auto blinks"))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(12.0f, 0.0f, 0.0f, 0.0f).VAlign(VAlign_Center)
				[
					SAssignNew(IsolateVoiceCheck, SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					.ToolTipText(LOCTEXT("IsolateVoiceTip",
						"For songs: pull vocals for lipsync. Uses Demucs AI stem separation when installed "
						"(pip install demucs), otherwise a fast DSP fallback. Full mix still plays on the sequence. "
						"Isolation Strength blends mix → isolated vocal. Results are cached under Saved/CineDirectorFace/Stems."))
					[
						SNew(STextBlock).Text(LOCTEXT("IsolateVoice", "Isolate voice"))
					]
				])
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SAssignNew(LayeredArkitMouthCheck, SCheckBox)
			.IsChecked(ECheckBoxState::Unchecked)
			.ToolTipText(LOCTEXT("LayeredArkitMouthTip",
				"For dual void faces (A/I/U/O + ARKit): off = safe exclusive vowels (default). "
				"On = MetaHuman-style layered ARKit mouth + jaw co-articulation under EE/OO/OH. "
				"Reads more articulated; may stretch more. No effect on pure VRM or pure MetaHuman."))
			[
				SNew(STextBlock).Text(LOCTEXT("LayeredArkitMouth", "Layered ARKit mouth (MetaHuman-style)"))
			]
		]

		// --- Strength sliders ---
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("StrengthHeader", "Strength"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("MouthStrLabel", "Mouth"),
				MakeStrengthSlider(
					MouthStrengthLabel,
					[this]() { return MouthStrength; },
					[this](float V) { MouthStrength = V; },
					0.0f, 2.0f,
					LOCTEXT("MouthStrTip", "How far lipsync jaw / A-I-U-O shapes travel. 0 = no mouth motion, 1 = default, 2 = maxed.")))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("EmotionStrLabel", "Emotion"),
				MakeStrengthSlider(
					EmotionStrengthLabel,
					[this]() { return EmotionStrength; },
					[this](float V) { EmotionStrength = V; },
					0.0f, 2.0f,
					LOCTEXT("EmotionStrTip", "How strong brows / full-face Joy-Angry-Sorrow-Surprised poses are. 0 = none, 0.6 = calibrated default, 1 = full pose.")))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("ArticulationLabel", "Articulation"),
				MakeStrengthSlider(
					ArticulationLabel,
					[this]() { return Articulation; },
					[this](float V) { Articulation = V; },
					0.0f, 2.0f,
					LOCTEXT("ArticulationTip", "How crisply the mouth hits each shape. Below 1 = soft, mumbled transitions; 1 = default; above 1 = snappy, fully-enunciated MetaHuman-style pronunciation.")))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("IsoStrLabel", "Isolation"),
				MakeStrengthSlider(
					IsolateStrengthLabel,
					[this]() { return IsolateStrength; },
					[this](float V) { IsolateStrength = V; },
					0.0f, 1.0f,
					LOCTEXT("IsoStrTip", "How hard Isolate voice filters music. 0 = raw mix, 1 = full isolation. Only used when Isolate voice is checked.")))
		]


		// --- Per-mesh calibration ---
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("CalibrationHeader", "Mesh Calibration (gain / neutral offset)"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalJaw", "Jaw"), MakeCalibrationRow(
				&FCineFaceCalibration::JawGain, &FCineFaceCalibration::JawOffset,
				LOCTEXT("CalJawTip", "Character-specific jaw-open travel.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalStretch", "Stretch"), MakeCalibrationRow(
				&FCineFaceCalibration::StretchGain, &FCineFaceCalibration::StretchOffset,
				LOCTEXT("CalStretchTip", "Mouth-wide/stretch gain. Lower this first for an over-stretched ARKit idle or EE shape.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalSmile", "Smile"), MakeCalibrationRow(
				&FCineFaceCalibration::SmileGain, &FCineFaceCalibration::SmileOffset,
				LOCTEXT("CalSmileTip", "Smile and happy-mouth travel.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalPucker", "Pucker"), MakeCalibrationRow(
				&FCineFaceCalibration::PuckerGain, &FCineFaceCalibration::PuckerOffset,
				LOCTEXT("CalPuckerTip", "Pucker and funnel/rounded vowel travel.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalLowerLip", "Lower lip"), MakeCalibrationRow(
				&FCineFaceCalibration::LowerLipGain, &FCineFaceCalibration::LowerLipOffset,
				LOCTEXT("CalLowerLipTip", "Lower-lip depression and lower-teeth reveal.")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			MakeRow(LOCTEXT("CalBrows", "Brows"), MakeCalibrationRow(
				&FCineFaceCalibration::BrowGain, &FCineFaceCalibration::BrowOffset,
				LOCTEXT("CalBrowsTip", "Brow up, down, and inner/sad brow travel.")))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(110.0f, 4.0f, 0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("SaveCalibration", "Save Profile"))
				.OnClicked(this, &SCineDirectorFacePanel::OnSaveCalibration)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("ResetCalibration", "Reset"))
				.OnClicked(this, &SCineDirectorFacePanel::OnResetCalibration)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("PreviewNeutral", "Preview Neutral"))
				.OnClicked(this, &SCineDirectorFacePanel::OnPreviewNeutral)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).Text(LOCTEXT("ClearNeutral", "Clear Preview"))
				.OnClicked(this, &SCineDirectorFacePanel::OnClearNeutralPreview)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(LOCTEXT("Generate", "Generate Face Animation"))
			.ToolTipText(LOCTEXT("GenerateTip", "Bakes an additive, curves-only animation asset and layers it onto the character in the open Level Sequence — the body animation keeps playing underneath."))
			.OnClicked(this, &SCineDirectorFacePanel::OnGenerate)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SAssignNew(StatusBlock, STextBlock)
			.AutoWrapText(true)
		]
	];

	RefreshSliderLabels();
}

void SCineDirectorFacePanel::RefreshSliderLabels()
{
	if (MouthStrengthLabel.IsValid())
	{
		MouthStrengthLabel->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), MouthStrength)));
	}
	if (EmotionStrengthLabel.IsValid())
	{
		EmotionStrengthLabel->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), EmotionStrength)));
	}
	if (ArticulationLabel.IsValid())
	{
		ArticulationLabel->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), Articulation)));
	}
	if (IsolateStrengthLabel.IsValid())
	{
		IsolateStrengthLabel->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), IsolateStrength)));
	}
}


void SCineDirectorFacePanel::LoadCalibrationProfile()
{
	Calibration = FCineFaceCalibration();
	USkeletalMesh* Mesh = GetTargetMesh();
	if (!Mesh || !GConfig) { return; }
	const FString Section = CalibrationSection(Mesh);
	auto Read = [&Section](const TCHAR* Key, float& Value)
	{
		GConfig->GetFloat(*Section, Key, Value, GEditorPerProjectIni);
	};
	Read(TEXT("JawGain"), Calibration.JawGain); Read(TEXT("JawOffset"), Calibration.JawOffset);
	Read(TEXT("StretchGain"), Calibration.StretchGain); Read(TEXT("StretchOffset"), Calibration.StretchOffset);
	Read(TEXT("SmileGain"), Calibration.SmileGain); Read(TEXT("SmileOffset"), Calibration.SmileOffset);
	Read(TEXT("PuckerGain"), Calibration.PuckerGain); Read(TEXT("PuckerOffset"), Calibration.PuckerOffset);
	Read(TEXT("LowerLipGain"), Calibration.LowerLipGain); Read(TEXT("LowerLipOffset"), Calibration.LowerLipOffset);
	Read(TEXT("BrowGain"), Calibration.BrowGain); Read(TEXT("BrowOffset"), Calibration.BrowOffset);
	Calibration.JawGain = FMath::Clamp(Calibration.JawGain, 0.0f, 2.0f);
	Calibration.StretchGain = FMath::Clamp(Calibration.StretchGain, 0.0f, 2.0f);
	Calibration.SmileGain = FMath::Clamp(Calibration.SmileGain, 0.0f, 2.0f);
	Calibration.PuckerGain = FMath::Clamp(Calibration.PuckerGain, 0.0f, 2.0f);
	Calibration.LowerLipGain = FMath::Clamp(Calibration.LowerLipGain, 0.0f, 2.0f);
	Calibration.BrowGain = FMath::Clamp(Calibration.BrowGain, 0.0f, 2.0f);
}

void SCineDirectorFacePanel::SaveCalibrationProfile() const
{
	USkeletalMesh* Mesh = GetTargetMesh();
	if (!Mesh || !GConfig) { return; }
	const FString Section = CalibrationSection(Mesh);
	auto Write = [&Section](const TCHAR* Key, float Value)
	{
		GConfig->SetFloat(*Section, Key, Value, GEditorPerProjectIni);
	};
	Write(TEXT("JawGain"), Calibration.JawGain); Write(TEXT("JawOffset"), Calibration.JawOffset);
	Write(TEXT("StretchGain"), Calibration.StretchGain); Write(TEXT("StretchOffset"), Calibration.StretchOffset);
	Write(TEXT("SmileGain"), Calibration.SmileGain); Write(TEXT("SmileOffset"), Calibration.SmileOffset);
	Write(TEXT("PuckerGain"), Calibration.PuckerGain); Write(TEXT("PuckerOffset"), Calibration.PuckerOffset);
	Write(TEXT("LowerLipGain"), Calibration.LowerLipGain); Write(TEXT("LowerLipOffset"), Calibration.LowerLipOffset);
	Write(TEXT("BrowGain"), Calibration.BrowGain); Write(TEXT("BrowOffset"), Calibration.BrowOffset);
	GConfig->Flush(false, GEditorPerProjectIni);
}

void SCineDirectorFacePanel::ApplyNeutralPreview(bool bClear)
{
	AActor* Actor = TargetActor.Get();
	USkeletalMesh* Mesh = GetTargetMesh();
	if (!Actor || !Mesh)
	{
		SetStatus(TEXT("Pick a character before previewing calibration."), true);
		return;
	}
	const bool bLayered = LayeredArkitMouthCheck.IsValid() && LayeredArkitMouthCheck->IsChecked();
	const FCineFaceProfile Profile = FCineFaceAnalyzer::Analyze(Mesh, bLayered);
	if (Profile.bMetaHuman)
	{
		SetStatus(TEXT("Neutral preview currently applies raw morph targets. MetaHuman control curves will use this profile when baked."), true);
		return;
	}
	TArray<USkeletalMeshComponent*> Components;
	Actor->GetComponents<USkeletalMeshComponent>(Components);
	int32 Applied = 0;
	for (USkeletalMeshComponent* Component : Components)
	{
		if (!Component || Component->GetSkeletalMeshAsset() != Mesh) { continue; }
		for (int32 Slot = 0; Slot < (int32)ECineFaceSlot::Count; ++Slot)
		{
			const float Value = bClear ? 0.0f : FMath::Clamp(Calibration.OffsetForSlot((ECineFaceSlot)Slot), -0.25f, 0.25f);
			for (const FCineFaceCurveTarget& Target : Profile.Slots[Slot])
			{
				Component->SetMorphTarget(Target.CurveName, Value, bClear || FMath::IsNearlyZero(Value));
				++Applied;
			}
		}
		Component->MarkRenderStateDirty();
	}
	SetStatus(bClear
		? FString::Printf(TEXT("Cleared neutral preview on %s."), *Mesh->GetName())
		: FString::Printf(TEXT("Previewing saved-neutral offsets on %s (%d mapped targets)."), *Mesh->GetName(), Applied));
}

FReply SCineDirectorFacePanel::OnSaveCalibration()
{
	if (!GetTargetMesh()) { SetStatus(TEXT("Pick a character before saving calibration."), true); return FReply::Handled(); }
	SaveCalibrationProfile();
	SetStatus(FString::Printf(TEXT("Saved calibration profile for %s."), *GetTargetMesh()->GetName()));
	return FReply::Handled();
}

FReply SCineDirectorFacePanel::OnResetCalibration()
{
	USkeletalMesh* Mesh = GetTargetMesh();
	Calibration = FCineFaceCalibration();
	if (Mesh && GConfig)
	{
		GConfig->EmptySection(*CalibrationSection(Mesh), GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}
	ApplyNeutralPreview(true);
	SetStatus(Mesh ? FString::Printf(TEXT("Reset calibration for %s to defaults."), *Mesh->GetName()) : TEXT("Calibration reset."));
	return FReply::Handled();
}

FReply SCineDirectorFacePanel::OnPreviewNeutral()
{
	ApplyNeutralPreview(false);
	return FReply::Handled();
}

FReply SCineDirectorFacePanel::OnClearNeutralPreview()
{
	ApplyNeutralPreview(true);
	return FReply::Handled();
}
FReply SCineDirectorFacePanel::OnUseSelectedActor()
{
	AActor* Selected = GEditor ? GEditor->GetSelectedActors()->GetTop<AActor>() : nullptr;
	if (!Selected)
	{
		SetStatus(TEXT("Nothing selected — click a character in the viewport or outliner first."), true);
		return FReply::Handled();
	}
	TargetActor = Selected;
	LoadCalibrationProfile();
	if (USkeletalMesh* Mesh = GetTargetMesh())
	{
		TargetLabel->SetText(FText::FromString(FString::Printf(TEXT("%s (%s)"), *Selected->GetActorLabel(), *Mesh->GetName())));
		SetStatus(TEXT(""));
	}
	else
	{
		TargetLabel->SetText(FText::FromString(Selected->GetActorLabel()));
		SetStatus(FString::Printf(TEXT("'%s' has no skeletal mesh component — facial animation needs one."), *Selected->GetActorLabel()), true);
	}
	return FReply::Handled();
}

FReply SCineDirectorFacePanel::OnBrowseAudio()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		return FReply::Handled();
	}
	TArray<FString> Files;
	const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	if (Desktop->OpenFileDialog(const_cast<void*>(ParentWindow),
		TEXT("Pick dialogue audio"), TEXT(""), TEXT(""),
		TEXT("Audio files (*.wav;*.mp3;*.ogg;*.flac;*.m4a)|*.wav;*.mp3;*.ogg;*.flac;*.m4a|All files (*.*)|*.*"),
		EFileDialogFlags::None, Files) && Files.Num() > 0)
	{
		AudioPathBox->SetText(FText::FromString(Files[0]));
	}
	return FReply::Handled();
}

USkeletalMesh* SCineDirectorFacePanel::GetTargetMesh() const
{
	const AActor* Actor = TargetActor.Get();
	if (!Actor)
	{
		return nullptr;
	}
	TArray<USkeletalMeshComponent*> Components;
	Actor->GetComponents<USkeletalMeshComponent>(Components);
	// Prefer a component that actually has morph targets (MetaHumans have
	// separate body and face meshes; marketplace characters usually have one).
	USkeletalMesh* Fallback = nullptr;
	for (const USkeletalMeshComponent* Component : Components)
	{
		USkeletalMesh* Mesh = Component ? Component->GetSkeletalMeshAsset() : nullptr;
		if (!Mesh)
		{
			continue;
		}
		if (Mesh->GetMorphTargets().Num() > 0)
		{
			return Mesh;
		}
		if (!Fallback)
		{
			Fallback = Mesh;
		}
	}
	return Fallback;
}

FReply SCineDirectorFacePanel::OnAnalyzeFace()
{
	USkeletalMesh* Mesh = GetTargetMesh();
	if (!Mesh)
	{
		SetStatus(TEXT("Pick a character with a skeletal mesh first (Use Selected Actor)."), true);
		return FReply::Handled();
	}
	const bool bLayeredMouth = LayeredArkitMouthCheck.IsValid() && LayeredArkitMouthCheck->IsChecked();
	const FCineFaceProfile Profile = FCineFaceAnalyzer::Analyze(Mesh, bLayeredMouth);
	SetStatus(FCineFaceAnalyzer::DescribeProfile(Profile), Profile.NumMappedSlots() == 0);
	return FReply::Handled();
}

FReply SCineDirectorFacePanel::OnGenerate()
{
	AActor* Actor = TargetActor.Get();
	USkeletalMesh* Mesh = GetTargetMesh();
	if (!Actor || !Mesh)
	{
		SetStatus(TEXT("Pick a character with a skeletal mesh first (Use Selected Actor)."), true);
		return FReply::Handled();
	}

	FCineFaceBakeRequest Request;
	Request.Mesh = Mesh;
	const bool bLayeredMouth = LayeredArkitMouthCheck.IsValid() && LayeredArkitMouthCheck->IsChecked();
	Request.Profile = FCineFaceAnalyzer::Analyze(Mesh, bLayeredMouth);
	if (Request.Profile.NumMappedSlots() == 0)
	{
		SetStatus(FCineFaceAnalyzer::DescribeProfile(Request.Profile), true);
		return FReply::Handled();
	}

	const FString ManualEmotion = EmotionBox->GetText().ToString().TrimStartAndEnd();
	Request.EmotionText = ManualEmotion;
	Request.bAutoBlink = BlinkCheck->IsChecked();
	Request.MouthStrength = MouthStrength;
	Request.EmotionStrength = EmotionStrength;
	Request.Articulation = Articulation;
	Request.Calibration = Calibration;
	Request.DurationSeconds = FMath::Clamp(FCString::Atof(*DurationBox->GetText().ToString()), 0.5f, 600.0f);
	if (Request.DurationSeconds < 0.51f)
	{
		Request.DurationSeconds = 6.0f;
	}

	const FString AudioPath = AudioPathBox->GetText().ToString().TrimStartAndEnd().TrimQuotes();
	USoundWave* Sound = nullptr;
	FString Error;
	bool bEmotionFromAudio = false;
	FString IsoMethod, IsoNote;

	if (!AudioPath.IsEmpty())
	{
		TArray<float> Samples;
		int32 SampleRate = 0;
		FString WavPath;
		if (!FCineLipsync::LoadAudioMono(AudioPath, Samples, SampleRate, WavPath, Error))
		{
			SetStatus(Error, true);
			return FReply::Handled();
		}
		Request.DurationSeconds = (float)Samples.Num() / FMath::Max(1, SampleRate);

		// Isolation is for lipsync only. Emotion auto-detect uses the raw mix.
		// Prefer Demucs AI stems when installed; otherwise fast DSP.
		TArray<float> LipSamples = Samples;
		int32 LipRate = SampleRate;
		const bool bIsolated = IsolateVoiceCheck.IsValid() && IsolateVoiceCheck->IsChecked();
		if (bIsolated && IsolateStrength > 0.01f)
		{
			SetStatus(TEXT("Isolating vocals… (Demucs AI if installed, else fast DSP — first AI run can take a few minutes)"), false);
			// Use converted wav when available (PCM) so Demucs always gets a clean path.
			const FString StemSource = !WavPath.IsEmpty() ? WavPath : AudioPath;
			FCineLipsync::IsolateVoiceForLipsync(StemSource, LipSamples, LipRate,
				IsolateStrength, IsoMethod, IsoNote);
		}

		if (TalkingCheck->IsChecked())
		{
			Request.Visemes = FCineLipsync::AnalyzeAudio(LipSamples, LipRate);
		}
		// Auto-pick emotion from the raw dialogue (full dynamics).
		// The rolling detector supplies short-term changes. Keep the established
		// take-level estimator as a base too: it catches valid, low-energy
		// dialogue that the per-frame confidence gate treats as uncertain.
		if (ManualEmotion.IsEmpty())
		{
			FString RollingEstimate;
			Request.AudioEmotions = FCineLipsync::AnalyzeEmotion(Samples, SampleRate, Request.Fps, &RollingEstimate);
			FString Estimated = FCineLipsync::EstimateEmotionFromAudio(Samples, SampleRate);
			if (Estimated.IsEmpty() || Estimated.Equals(TEXT("neutral"), ESearchCase::IgnoreCase))
			{
				Estimated = RollingEstimate;
			}
			if (Estimated.IsEmpty()) { Estimated = TEXT("neutral"); }
			Request.EmotionText = Estimated;
			bEmotionFromAudio = true;
			Request.bEmotionFromAudio = true;
			// Keep the box blank so the next generation remains auto; the status
			// message reports the estimate without turning it into a stale override.
		}
		Sound = FCineFaceBaker::ImportAudioAsset(WavPath, Error);
		if (!Sound)
		{
			SetStatus(Error + TEXT(" — continuing without the audio track."), true);
		}
	}
	else if (TalkingCheck->IsChecked())
	{
		Request.Visemes = FCineLipsync::SynthesizeTalking(Request.DurationSeconds, Request.Fps,
			GetTypeHash(Actor->GetActorLabel()));
	}

	UAnimSequence* FaceAnim = FCineFaceBaker::BakeAnimAsset(Request, Error);
	if (!FaceAnim)
	{
		SetStatus(Error, true);
		return FReply::Handled();
	}
	if (!FCineFaceBaker::AddToSequencer(Actor, FaceAnim, Sound, Error))
	{
		SetStatus(FString::Printf(TEXT("Baked %s, but: %s"), *FaceAnim->GetName(), *Error), true);
		return FReply::Handled();
	}

	const FString EmotionNote = Request.EmotionText.IsEmpty()
		? FString()
		: FString::Printf(TEXT(" (%s%s)"), bEmotionFromAudio ? TEXT("auto: ") : TEXT(""), *Request.EmotionText);

	FString IsoTag;
	if (!IsoMethod.IsEmpty() && IsoMethod != TEXT("none"))
	{
		IsoTag = FString::Printf(TEXT(" [iso:%s %.2f]"), *IsoMethod, IsolateStrength);
		if (!IsoNote.IsEmpty())
		{
			IsoTag += TEXT(" — ") + IsoNote;
		}
	}
	SetStatus(FString::Printf(
		TEXT("Done: %s — %.1fs of %s%s%s | mouth %.2f artic %.2f emotion %.2f layered onto '%s'. %s"),
		*FaceAnim->GetName(), Request.DurationSeconds,
		Request.Visemes.Num() > 0 ? (AudioPath.IsEmpty() ? TEXT("procedural talking") : TEXT("audio-driven lipsync")) : TEXT("expression"),
		*IsoTag,
		*EmotionNote,
		MouthStrength, Articulation, EmotionStrength,
		*Actor->GetActorLabel(),
		Sound ? TEXT("Audio on sequence track.") : TEXT("")));
	return FReply::Handled();
}

void SCineDirectorFacePanel::SetStatus(const FString& Message, bool bIsError)
{
	StatusBlock->SetText(FText::FromString(Message));
	StatusBlock->SetColorAndOpacity(bIsError ? FSlateColor(FaceErrorColor) : FSlateColor::UseForeground());
}

#undef LOCTEXT_NAMESPACE
