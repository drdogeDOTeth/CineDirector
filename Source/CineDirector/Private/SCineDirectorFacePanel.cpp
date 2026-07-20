// Copyright Roundtree. All Rights Reserved.

#include "SCineDirectorFacePanel.h"

#include "CineFaceAnalyzer.h"
#include "CineFaceBaker.h"
#include "CineLipsync.h"
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
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
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
						"For songs/mixes: emphasize the vocal band and suppress steady instrumentals before lipsync and emotion analysis. "
						"The full audio is still placed on the sequence for playback. Turn off for clean dialogue if you prefer."))
					[
						SNew(STextBlock).Text(LOCTEXT("IsolateVoice", "Isolate voice"))
					]
				])
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
	const FCineFaceProfile Profile = FCineFaceAnalyzer::Analyze(Mesh);
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
	Request.Profile = FCineFaceAnalyzer::Analyze(Mesh);
	if (Request.Profile.NumMappedSlots() == 0)
	{
		SetStatus(FCineFaceAnalyzer::DescribeProfile(Request.Profile), true);
		return FReply::Handled();
	}

	const FString ManualEmotion = EmotionBox->GetText().ToString().TrimStartAndEnd();
	Request.EmotionText = ManualEmotion;
	Request.bAutoBlink = BlinkCheck->IsChecked();
	Request.DurationSeconds = FMath::Clamp(FCString::Atof(*DurationBox->GetText().ToString()), 0.5f, 600.0f);
	if (Request.DurationSeconds < 0.51f)
	{
		Request.DurationSeconds = 6.0f;
	}

	const FString AudioPath = AudioPathBox->GetText().ToString().TrimStartAndEnd().TrimQuotes();
	USoundWave* Sound = nullptr;
	FString Error;
	bool bEmotionFromAudio = false;

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

		// Analysis copy: optionally isolate vocals so songs don't drive off drums/bass.
		// Playback still uses the original WavPath so the full mix is heard.
		TArray<float> AnalysisSamples = Samples;
		const bool bIsolated = IsolateVoiceCheck.IsValid() && IsolateVoiceCheck->IsChecked();
		if (bIsolated)
		{
			FCineLipsync::IsolateVoice(AnalysisSamples, SampleRate);
		}

		if (TalkingCheck->IsChecked())
		{
			Request.Visemes = FCineLipsync::AnalyzeAudio(AnalysisSamples, SampleRate);
		}
		// Auto-pick emotion from the dialogue when the user left the box blank.
		if (ManualEmotion.IsEmpty())
		{
			const FString Estimated = FCineLipsync::EstimateEmotionFromAudio(AnalysisSamples, SampleRate);
			if (!Estimated.IsEmpty())
			{
				Request.EmotionText = Estimated;
				bEmotionFromAudio = true;
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

	const bool bIsolated = !AudioPath.IsEmpty() && IsolateVoiceCheck.IsValid() && IsolateVoiceCheck->IsChecked();
	SetStatus(FString::Printf(
		TEXT("Done: %s — %.1fs of %s%s%s layered onto '%s' at the sequence start. %s"),
		*FaceAnim->GetName(), Request.DurationSeconds,
		Request.Visemes.Num() > 0 ? (AudioPath.IsEmpty() ? TEXT("procedural talking") : TEXT("audio-driven lipsync")) : TEXT("expression"),
		bIsolated ? TEXT(" [voice-isolated]") : TEXT(""),
		*EmotionNote,
		*Actor->GetActorLabel(),
		Sound ? TEXT("Audio placed on the sequence's audio track.") : TEXT("")));
	return FReply::Handled();
}

void SCineDirectorFacePanel::SetStatus(const FString& Message, bool bIsError)
{
	StatusBlock->SetText(FText::FromString(Message));
	StatusBlock->SetColorAndOpacity(bIsError ? FSlateColor(FaceErrorColor) : FSlateColor::UseForeground());
}

#undef LOCTEXT_NAMESPACE
