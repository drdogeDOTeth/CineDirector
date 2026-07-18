// Copyright Roundtree. All Rights Reserved.

#include "CineFaceBaker.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Math/RandomStream.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sound/SoundWave.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "CineDirectorFace"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorFaceBake, Log, All);

namespace
{
	struct FSlotValue
	{
		ECineFaceSlot Slot;
		float Value;
	};

	struct FEmotionDef
	{
		const TCHAR* Keywords; // comma-separated triggers
		std::initializer_list<FSlotValue> Pose;
	};

	const FEmotionDef GEmotions[] = {
		{ TEXT("scared,afraid,terrified,fear,frightened"),
			{ { ECineFaceSlot::BrowSad, 0.7f }, { ECineFaceSlot::BrowUp, 0.5f }, { ECineFaceSlot::EyeWide, 0.9f },
			  { ECineFaceSlot::MouthFrown, 0.3f }, { ECineFaceSlot::MouthPress, 0.2f } } },
		{ TEXT("angry,furious,mad,rage,pissed"),
			{ { ECineFaceSlot::BrowDown, 1.0f }, { ECineFaceSlot::EyeSquint, 0.5f }, { ECineFaceSlot::NoseSneer, 0.6f },
			  { ECineFaceSlot::MouthPress, 0.5f }, { ECineFaceSlot::MouthFrown, 0.4f } } },
		{ TEXT("happy,joyful,cheerful,smiling,glad"),
			{ { ECineFaceSlot::MouthSmile, 0.8f }, { ECineFaceSlot::EyeSquint, 0.25f }, { ECineFaceSlot::BrowUp, 0.2f } } },
		{ TEXT("sad,somber,mournful,depressed,grief"),
			{ { ECineFaceSlot::BrowSad, 0.8f }, { ECineFaceSlot::MouthFrown, 0.7f }, { ECineFaceSlot::EyeBlink, 0.1f } } },
		{ TEXT("surprised,shocked,amazed,startled"),
			{ { ECineFaceSlot::BrowUp, 1.0f }, { ECineFaceSlot::EyeWide, 1.0f }, { ECineFaceSlot::JawOpen, 0.35f } } },
		{ TEXT("disgusted,disgust,revolted,grossed"),
			{ { ECineFaceSlot::NoseSneer, 0.9f }, { ECineFaceSlot::BrowDown, 0.5f }, { ECineFaceSlot::MouthFrown, 0.5f },
			  { ECineFaceSlot::EyeSquint, 0.4f } } },
		{ TEXT("pain,hurt,agony,wincing"),
			{ { ECineFaceSlot::EyeSquint, 0.9f }, { ECineFaceSlot::BrowSad, 0.6f }, { ECineFaceSlot::NoseSneer, 0.5f },
			  { ECineFaceSlot::MouthWide, 0.4f } } },
		{ TEXT("suspicious,wary,distrustful,skeptical"),
			{ { ECineFaceSlot::BrowDown, 0.4f }, { ECineFaceSlot::EyeSquint, 0.6f }, { ECineFaceSlot::MouthPress, 0.3f } } },
		{ TEXT("calm,neutral,relaxed,blank"), {} },
	};

	/** Per-slot values one emotion segment settles at, or empty for neutral. */
	void ParseEmotionSegment(const FString& Segment, float* OutValues /* [Count] */)
	{
		const FString Lower = Segment.ToLower();
		float Intensity = 1.0f;
		if (Lower.Contains(TEXT("slightly")) || Lower.Contains(TEXT("subtle")) || Lower.Contains(TEXT("a bit")))
		{
			Intensity = 0.5f;
		}
		else if (Lower.Contains(TEXT("very")) || Lower.Contains(TEXT("extremely")) || Lower.Contains(TEXT("super")))
		{
			Intensity = 1.3f;
		}

		int32 Matched = 0;
		for (const FEmotionDef& Def : GEmotions)
		{
			TArray<FString> Keywords;
			FString(Def.Keywords).ParseIntoArray(Keywords, TEXT(","));
			bool bHit = false;
			for (const FString& Keyword : Keywords)
			{
				bHit |= Lower.Contains(Keyword);
			}
			if (!bHit)
			{
				continue;
			}
			++Matched;
			for (const FSlotValue& SV : Def.Pose)
			{
				OutValues[(int32)SV.Slot] = FMath::Max(OutValues[(int32)SV.Slot],
					FMath::Clamp(SV.Value * Intensity, 0.0f, 1.0f));
			}
		}
		if (Matched == 0 && !Segment.TrimStartAndEnd().IsEmpty())
		{
			UE_LOG(LogCineDirectorFaceBake, Warning, TEXT("No emotion recognized in \"%s\" — treating as neutral."), *Segment);
		}
	}
}

FString FCineFaceBaker::GetEmotionVocabulary()
{
	return TEXT("scared, angry, happy, sad, surprised, disgusted, pain, suspicious, calm — ")
		TEXT("with slightly/very modifiers, and \"then\" to change over time (\"calm then very scared\").");
}

UAnimSequence* FCineFaceBaker::BakeAnimAsset(const FCineFaceBakeRequest& Request, FString& OutError)
{
	if (!Request.Mesh || !Request.Mesh->GetSkeleton())
	{
		OutError = TEXT("The actor has no skeletal mesh with a skeleton.");
		return nullptr;
	}
	if (Request.Profile.NumMappedSlots() == 0)
	{
		OutError = TEXT("No face slots mapped on this mesh — nothing to animate.");
		return nullptr;
	}

	const int32 Fps = FMath::Max(1, Request.Fps);
	const int32 NumFrames = FMath::Max(Request.Visemes.Num(),
		FMath::Max(2, FMath::RoundToInt32(Request.DurationSeconds * Fps)));
	constexpr int32 SlotCount = (int32)ECineFaceSlot::Count;

	// --- Assemble per-slot timelines: emotion base, then lipsync, then blinks.
	TArray<TArray<float>> Timeline;
	Timeline.SetNum(SlotCount);
	for (TArray<float>& T : Timeline)
	{
		T.SetNumZeroed(NumFrames);
	}

	TArray<FString> Segments;
	Request.EmotionText.Replace(TEXT(" Then "), TEXT(" then ")).ParseIntoArray(Segments, TEXT(" then "));
	if (Segments.Num() == 0)
	{
		Segments.Add(Request.EmotionText);
	}
	const int32 RampFrames = FMath::Max(1, FMath::RoundToInt32(0.4f * Fps));
	TArray<float> PrevPose, NextPose;
	PrevPose.SetNumZeroed(SlotCount);
	for (int32 SegIndex = 0; SegIndex < Segments.Num(); ++SegIndex)
	{
		NextPose.Reset();
		NextPose.SetNumZeroed(SlotCount);
		ParseEmotionSegment(Segments[SegIndex], NextPose.GetData());

		const int32 SegStart = NumFrames * SegIndex / Segments.Num();
		const int32 SegEnd = NumFrames * (SegIndex + 1) / Segments.Num();
		for (int32 Frame = SegStart; Frame < SegEnd; ++Frame)
		{
			const float Ramp = FMath::SmoothStep(0.0f, 1.0f, (float)(Frame - SegStart) / RampFrames);
			for (int32 Slot = 0; Slot < SlotCount; ++Slot)
			{
				Timeline[Slot][Frame] = FMath::Lerp(PrevPose[Slot], NextPose[Slot], Ramp);
			}
		}
		PrevPose = NextPose;
	}

	for (int32 Frame = 0; Frame < Request.Visemes.Num() && Frame < NumFrames; ++Frame)
	{
		const FCineVisemeFrame& V = Request.Visemes[Frame];
		Timeline[(int32)ECineFaceSlot::JawOpen][Frame] += V.Jaw * (1.0f - V.Close);
		Timeline[(int32)ECineFaceSlot::MouthClose][Frame] += V.Close;
		Timeline[(int32)ECineFaceSlot::MouthPress][Frame] += V.Close * 0.6f;
		Timeline[(int32)ECineFaceSlot::MouthWide][Frame] += V.Wide + V.Sibilant * 0.4f;
		Timeline[(int32)ECineFaceSlot::MouthPucker][Frame] += V.Pucker;
		Timeline[(int32)ECineFaceSlot::MouthFunnel][Frame] += V.Pucker * 0.5f;
	}

	if (Request.bAutoBlink)
	{
		// Fearful faces blink more; everything else settles around every ~3.5s.
		const bool bNervous = Request.EmotionText.ToLower().Contains(TEXT("scared"))
			|| Request.EmotionText.ToLower().Contains(TEXT("afraid"))
			|| Request.EmotionText.ToLower().Contains(TEXT("terrified"));
		FRandomStream Rand(NumFrames * 7 + 3);
		float NextBlink = Rand.FRandRange(0.5f, 1.5f);
		TArray<float>& Blink = Timeline[(int32)ECineFaceSlot::EyeBlink];
		while (NextBlink * Fps < NumFrames)
		{
			const int32 Start = FMath::RoundToInt32(NextBlink * Fps);
			const int32 Down = FMath::Max(1, Fps / 10);
			const int32 Up = FMath::Max(1, Fps / 8);
			for (int32 i = 0; Start + i < NumFrames && i < Down + 1 + Up; ++i)
			{
				float Value = 1.0f;
				if (i < Down) { Value = (float)(i + 1) / Down; }
				else if (i > Down) { Value = 1.0f - (float)(i - Down) / Up; }
				Blink[Start + i] = FMath::Max(Blink[Start + i], FMath::Clamp(Value, 0.0f, 1.0f));
			}
			NextBlink += bNervous ? Rand.FRandRange(1.2f, 2.5f) : Rand.FRandRange(2.5f, 5.0f);
		}
	}

	for (TArray<float>& T : Timeline)
	{
		for (float& V : T)
		{
			V = FMath::Clamp(V, 0.0f, 1.0f);
		}
	}

	// --- Bake into a curves-only additive UAnimSequence: zero bone deltas, so
	// layering it over a body animation moves the face and nothing else.
	const FString AssetName = FString::Printf(TEXT("%s_Face_%s"),
		*Request.Mesh->GetName(), *FDateTime::Now().ToString(TEXT("%m%d_%H%M%S")));
	const FString PackagePath = FString(TEXT("/Game/CineDirector/FaceAnims")) / AssetName;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		OutError = TEXT("Could not create the animation package.");
		return nullptr;
	}

	UAnimSequence* Anim = NewObject<UAnimSequence>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	Anim->SetSkeleton(Request.Mesh->GetSkeleton());

	IAnimationDataController& Ctrl = Anim->GetController();
	Ctrl.OpenBracket(LOCTEXT("BakeFace", "Bake CineDirector face animation"));
	Ctrl.InitializeModel();
	Ctrl.SetFrameRate(FFrameRate(Fps, 1));
	Ctrl.SetNumberOfFrames(FFrameNumber(NumFrames - 1));

	int32 CurvesWritten = 0;
	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		for (const FCineFaceCurveTarget& Target : Request.Profile.Slots[Slot])
		{
			const FAnimationCurveIdentifier CurveId(Target.CurveName, ERawCurveTrackTypes::RCT_Float);
			Ctrl.AddCurve(CurveId);
			TArray<FRichCurveKey> Keys;
			Keys.Reserve(NumFrames);
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				Keys.Emplace((float)Frame / Fps, Timeline[Slot][Frame] * Target.Scale);
			}
			Ctrl.SetCurveKeys(CurveId, Keys);
			++CurvesWritten;
		}
	}

	Ctrl.NotifyPopulated();
	Ctrl.CloseBracket();

	Anim->AdditiveAnimType = AAT_LocalSpaceBase;
	Anim->RefPoseType = ABPT_RefPose;

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Anim);

	UE_LOG(LogCineDirectorFaceBake, Log, TEXT("Baked '%s': %d curves, %d frames (%.1fs)."),
		*AssetName, CurvesWritten, NumFrames, (float)NumFrames / Fps);
	return Anim;
}

USoundWave* FCineFaceBaker::ImportAudioAsset(const FString& WavPath, FString& OutError)
{
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = WavPath;
	Task->DestinationPath = TEXT("/Game/CineDirector/Audio");
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetTools.Get().ImportAssetTasks({ Task });

	for (UObject* Imported : Task->GetObjects())
	{
		if (USoundWave* Sound = Cast<USoundWave>(Imported))
		{
			return Sound;
		}
	}
	OutError = FString::Printf(TEXT("Could not import '%s' as a SoundWave."), *WavPath);
	return nullptr;
}

bool FCineFaceBaker::AddToSequencer(AActor* Actor, UAnimSequence* FaceAnim, USoundWave* Audio, FString& OutError)
{
	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		OutError = TEXT("Open a Level Sequence in Sequencer first.");
		return false;
	}
	if (!Actor || !FaceAnim)
	{
		OutError = TEXT("Missing actor or baked animation.");
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("AddFaceAnim", "CineDirector: Add face animation"));
	Sequence->Modify();
	MovieScene->Modify();

	// Same dedupe-by-name pattern the shot executor uses for sun/fog bindings.
	FGuid ActorGuid;
	const FString ActorLabel = Actor->GetActorLabel();
	for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Existing = MovieScene->GetPossessable(i);
		if (Existing.GetName() == ActorLabel && Actor->GetClass()->IsChildOf(Existing.GetPossessedObjectClass()))
		{
			ActorGuid = Existing.GetGuid();
			break;
		}
	}
	if (!ActorGuid.IsValid())
	{
		ActorGuid = MovieScene->AddPossessable(ActorLabel, Actor->GetClass());
		Sequence->BindPossessableObject(ActorGuid, *Actor, Actor->GetWorld());
	}

	const FFrameNumber Start = MovieScene->GetPlaybackRange().GetLowerBoundValue();

	UMovieSceneSkeletalAnimationTrack* AnimTrack = Cast<UMovieSceneSkeletalAnimationTrack>(
		MovieScene->FindTrack(UMovieSceneSkeletalAnimationTrack::StaticClass(), ActorGuid));
	if (!AnimTrack)
	{
		AnimTrack = Cast<UMovieSceneSkeletalAnimationTrack>(
			MovieScene->AddTrack(UMovieSceneSkeletalAnimationTrack::StaticClass(), ActorGuid));
	}
	if (!AnimTrack)
	{
		OutError = TEXT("Could not create a skeletal animation track for the actor.");
		return false;
	}
	AnimTrack->Modify();
	AnimTrack->AddNewAnimation(Start, FaceAnim);

	if (Audio)
	{
		UMovieSceneAudioTrack* AudioTrack = nullptr;
		for (UMovieSceneTrack* Track : MovieScene->GetTracks())
		{
			if (UMovieSceneAudioTrack* Existing = Cast<UMovieSceneAudioTrack>(Track))
			{
				AudioTrack = Existing;
				break;
			}
		}
		if (!AudioTrack)
		{
			AudioTrack = MovieScene->AddTrack<UMovieSceneAudioTrack>();
		}
		if (AudioTrack)
		{
			AudioTrack->Modify();
			AudioTrack->AddNewSound(Audio, Start);
		}
	}

	ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();
	UE_LOG(LogCineDirectorFaceBake, Log, TEXT("Added face animation '%s'%s to '%s' on binding '%s'."),
		*FaceAnim->GetName(), Audio ? TEXT(" + audio") : TEXT(""), *Sequence->GetName(), *ActorLabel);
	return true;
}

#undef LOCTEXT_NAMESPACE
