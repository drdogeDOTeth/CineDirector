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
#include "Sound/SoundWave.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CineDirectorFacePanel"

namespace
{
	const FLinearColor FaceErrorColor(1.0f, 0.45f, 0.35f);
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
					LOCTEXT("EmotionStrTip", "How strong brows / full-face Joy-Angry-Sorrow-Surprised poses are. 0 = no emotion, 1 = default, 2 = maxed.")))
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

FReply SCineDirectorFacePanel::OnUseSelectedActor()
{
	AActor* Selected = GEditor ? GEditor->GetSelectedActors()->GetTop<AActor>() : nullptr;
	if (!Selected)
	{
		SetStatus(TEXT("Nothing selected — click a character in the viewport or outliner first."), true);
		return FReply::Handled();
	}
	TargetActor = Selected;
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
		// Always fill something when the box is blank so faces never stay neutral.
		if (ManualEmotion.IsEmpty())
		{
			FString Estimated = FCineLipsync::EstimateEmotionFromAudio(Samples, SampleRate);
			if (Estimated.IsEmpty())
			{
				Estimated = TEXT("happy");
			}
			Request.EmotionText = Estimated;
			bEmotionFromAudio = true;
			// Show what auto picked so the user can see/edit it.
			if (EmotionBox.IsValid())
			{
				EmotionBox->SetText(FText::FromString(Estimated));
			}
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
