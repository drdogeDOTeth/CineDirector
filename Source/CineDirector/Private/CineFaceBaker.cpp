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

	/**
	 * Emotion poses. Full-face Expr* slots are driven hard so VRM/MMD Joy/Angry/
	 * Sorrow/Surprised morphs read clearly; micro-slots (brows, squint, etc.)
	 * still fire for ARKit / MetaHuman faces that lack full-face morphs.
	 */
	const FEmotionDef GEmotions[] = {
		{ TEXT("scared,afraid,terrified,fear,frightened"),
			{ { ECineFaceSlot::ExprSurprised, 0.9f },
			  { ECineFaceSlot::BrowSad, 1.0f }, { ECineFaceSlot::BrowUp, 0.95f }, { ECineFaceSlot::EyeWide, 1.0f },
			  { ECineFaceSlot::MouthFrown, 0.55f }, { ECineFaceSlot::MouthPress, 0.45f } } },
		{ TEXT("angry,furious,mad,rage,pissed"),
			{ { ECineFaceSlot::ExprAngry, 1.0f },
			  { ECineFaceSlot::BrowDown, 1.0f }, { ECineFaceSlot::EyeSquint, 0.8f }, { ECineFaceSlot::NoseSneer, 0.9f },
			  { ECineFaceSlot::MouthPress, 0.8f }, { ECineFaceSlot::MouthFrown, 0.7f } } },
		{ TEXT("happy,joyful,cheerful,smiling,glad"),
			// Strong default; panel Emotion Strength slider can dial down if too wide.
			{ { ECineFaceSlot::ExprHappy, 0.85f },
			  { ECineFaceSlot::MouthSmile, 0.8f }, { ECineFaceSlot::EyeSquint, 0.35f }, { ECineFaceSlot::BrowUp, 0.28f } } },
		{ TEXT("sad,somber,mournful,depressed,grief"),
			{ { ECineFaceSlot::ExprSad, 1.0f },
			  { ECineFaceSlot::BrowSad, 1.0f }, { ECineFaceSlot::MouthFrown, 0.95f }, { ECineFaceSlot::EyeBlink, 0.15f } } },
		{ TEXT("surprised,shocked,amazed,startled"),
			{ { ECineFaceSlot::ExprSurprised, 1.0f },
			  { ECineFaceSlot::BrowUp, 1.0f }, { ECineFaceSlot::EyeWide, 1.0f }, { ECineFaceSlot::JawOpen, 0.45f } } },
		{ TEXT("disgusted,disgust,revolted,grossed"),
			{ { ECineFaceSlot::ExprAngry, 0.7f },
			  { ECineFaceSlot::NoseSneer, 1.0f }, { ECineFaceSlot::BrowDown, 0.85f }, { ECineFaceSlot::MouthFrown, 0.8f },
			  { ECineFaceSlot::EyeSquint, 0.7f } } },
		{ TEXT("pain,hurt,agony,wincing"),
			{ { ECineFaceSlot::ExprSad, 0.8f },
			  { ECineFaceSlot::EyeSquint, 1.0f }, { ECineFaceSlot::BrowSad, 0.95f }, { ECineFaceSlot::NoseSneer, 0.75f },
			  { ECineFaceSlot::MouthWide, 0.55f } } },
		{ TEXT("suspicious,wary,distrustful,skeptical"),
			{ { ECineFaceSlot::ExprAngry, 0.55f },
			  { ECineFaceSlot::BrowDown, 0.85f }, { ECineFaceSlot::EyeSquint, 0.9f }, { ECineFaceSlot::MouthPress, 0.6f } } },
		{ TEXT("calm,neutral,relaxed,blank"),
			{ { ECineFaceSlot::BrowUp, 0.12f }, { ECineFaceSlot::MouthSmile, 0.1f } } },
	};

	/** Per-slot values one emotion segment settles at, or empty for neutral. */
	void ParseEmotionSegment(const FString& Segment, float* OutValues /* [Count] */, float StrengthMul = 1.0f)
	{
		const FString Lower = Segment.ToLower().TrimStartAndEnd();
		float Intensity = 1.0f;
		if (Lower.Contains(TEXT("slightly")) || Lower.Contains(TEXT("subtle")) || Lower.Contains(TEXT("a bit")))
		{
			Intensity = 0.75f;
		}
		else if (Lower.Contains(TEXT("very")) || Lower.Contains(TEXT("extremely")) || Lower.Contains(TEXT("super")))
		{
			Intensity = 1.15f;
		}
		// Panel Emotion Strength (1.0 = table values, 2.0 = maxed, 0 = off).
		Intensity *= FMath::Clamp(StrengthMul, 0.0f, 2.5f);

		int32 Matched = 0;
		for (const FEmotionDef& Def : GEmotions)
		{
			TArray<FString> Keywords;
			FString(Def.Keywords).ParseIntoArray(Keywords, TEXT(","));
			bool bHit = false;
			for (FString Keyword : Keywords)
			{
				Keyword.TrimStartAndEndInline();
				if (Keyword.IsEmpty()) continue;
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
		if (Matched == 0 && !Lower.IsEmpty())
		{
			UE_LOG(LogCineDirectorFaceBake, Warning, TEXT("No emotion recognized in \"%s\" — treating as neutral."), *Segment);
		}
		else
		{
			UE_LOG(LogCineDirectorFaceBake, Log, TEXT("Emotion segment \"%s\" matched %d def(s) strength=%.2f"),
				*Segment, Matched, Intensity);
		}
	}

	/**
	 * Natural eye motion: hold a gaze, then saccade to a new offset.
	 * Writes into LookLeft/Right/Up/Down (opposing axes are mutually exclusive).
	 */
	void BakeEyeGaze(TArray<TArray<float>>& Timeline, int32 NumFrames, int32 Fps, int32 Seed)
	{
		const int32 SlotL = (int32)ECineFaceSlot::EyeLookLeft;
		const int32 SlotR = (int32)ECineFaceSlot::EyeLookRight;
		const int32 SlotU = (int32)ECineFaceSlot::EyeLookUp;
		const int32 SlotD = (int32)ECineFaceSlot::EyeLookDown;

		FRandomStream Rand(Seed ^ 0xE4E5A11);
		float Time = Rand.FRandRange(0.15f, 0.6f);
		// Current held gaze (X = right-left, Y = up-down), range roughly -1..1.
		float GazeX = 0.0f;
		float GazeY = 0.0f;

		auto WriteGaze = [&](int32 Frame, float X, float Y)
		{
			if (Frame < 0 || Frame >= NumFrames) return;
			Timeline[SlotL][Frame] = X < 0.0f ? -X : 0.0f;
			Timeline[SlotR][Frame] = X > 0.0f ?  X : 0.0f;
			Timeline[SlotD][Frame] = Y < 0.0f ? -Y : 0.0f;
			Timeline[SlotU][Frame] = Y > 0.0f ?  Y : 0.0f;
		};

		// Fill initial hold
		for (int32 f = 0; f < NumFrames; ++f)
		{
			WriteGaze(f, GazeX, GazeY);
		}

		while (Time * Fps < NumFrames)
		{
			// New target: mostly near-center, occasional glance aside.
			const float Mag = Rand.FRand() < 0.35f
				? Rand.FRandRange(0.35f, 0.85f)   // glance
				: Rand.FRandRange(0.05f, 0.35f);  // subtle drift
			const float Angle = Rand.FRandRange(0.0f, 2.0f * PI);
			const float TargetX = FMath::Cos(Angle) * Mag;
			const float TargetY = FMath::Sin(Angle) * Mag * 0.55f; // vertical less extreme

			const int32 SacStart = FMath::RoundToInt32(Time * Fps);
			const int32 SacLen = FMath::Max(1, FMath::RoundToInt32(Rand.FRandRange(0.04f, 0.09f) * Fps)); // ~1–3 frames
			const float HoldSec = Rand.FRandRange(0.45f, 1.8f);
			const int32 HoldEnd = FMath::Min(NumFrames, SacStart + SacLen + FMath::RoundToInt32(HoldSec * Fps));

			for (int32 f = SacStart; f < HoldEnd; ++f)
			{
				float T = 1.0f;
				if (f < SacStart + SacLen)
				{
					T = (float)(f - SacStart + 1) / SacLen;
					// Ease-out so the saccade snaps then settles.
					T = 1.0f - FMath::Square(1.0f - T);
				}
				const float X = FMath::Lerp(GazeX, TargetX, T);
				const float Y = FMath::Lerp(GazeY, TargetY, T);
				WriteGaze(f, X, Y);
			}

			GazeX = TargetX;
			GazeY = TargetY;
			Time = (float)HoldEnd / Fps + Rand.FRandRange(0.0f, 0.15f);
		}
	}

	/**
	 * For VRM/MMD exclusive vowel morphs: keep only the dominant mouth shape
	 * so A/I/U/E/O never stack into a half-open mush.
	 * Slightly de-biases pure Jaw (A) so I/U can win when scores are close.
	 * O is intentionally less sticky — hysteresis + bias used to hold "oh"
	 * across following syllables so words never fully re-shape.
	 */
	void ApplyExclusiveVisemes(TArray<TArray<float>>& Timeline, int32 NumFrames)
	{
		const int32 Jaw = (int32)ECineFaceSlot::JawOpen;
		const int32 Wide = (int32)ECineFaceSlot::MouthWide;
		const int32 Pucker = (int32)ECineFaceSlot::MouthPucker;
		const int32 Funnel = (int32)ECineFaceSlot::MouthFunnel;
		const int32 Close = (int32)ECineFaceSlot::MouthClose;

		// A slightly down; I up. Rounded shapes (U/O) near unity so they can't
		// out-hold the next syllable's vowel.
		const float Bias[4] = { 0.92f, 1.12f, 1.06f, 1.00f };
		int32 PrevWinner = -1;
		// Frames we've forced-kept PrevWinner against a different raw winner.
		int32 StickyHold = 0;

		for (int32 f = 0; f < NumFrames; ++f)
		{
			// Closures win: shut everything else.
			if (Timeline[Close][f] > 0.5f)
			{
				Timeline[Jaw][f] = 0.0f;
				Timeline[Wide][f] = 0.0f;
				Timeline[Pucker][f] = 0.0f;
				Timeline[Funnel][f] = 0.0f;
				PrevWinner = -1;
				StickyHold = 0;
				continue;
			}

			const float Vals[4] = {
				Timeline[Jaw][f],
				Timeline[Wide][f],
				Timeline[Pucker][f],
				Timeline[Funnel][f]
			};
			float Weighted[4];
			int32 Winner = 0;
			float BestW = -1.0f;
			for (int32 i = 0; i < 4; ++i)
			{
				Weighted[i] = Vals[i] * Bias[i];
				if (Weighted[i] > BestW)
				{
					BestW = Weighted[i];
					Winner = i;
				}
			}

			// Soft floor: if nothing is open, leave all zero (mouth shut).
			if (BestW < 0.05f)
			{
				Timeline[Jaw][f] = 0.0f;
				Timeline[Wide][f] = 0.0f;
				Timeline[Pucker][f] = 0.0f;
				Timeline[Funnel][f] = 0.0f;
				PrevWinner = -1;
				StickyHold = 0;
				continue;
			}

			// Hysteresis: keep previous vowel unless a new one clearly wins.
			// Rounded shapes (U=2, O=3) are easy to leave and can only
			// force-hold briefly — both had the "stuck rounded mouth hangs into
			// the next syllable" failure mode.
			if (PrevWinner >= 0 && PrevWinner != Winner)
			{
				const float HoldRatio = (PrevWinner >= 2) ? 0.94f : 0.88f;
				const int32 MaxSticky = (PrevWinner >= 2) ? 2 : 5;
				const bool bMayHold = StickyHold < MaxSticky
					&& Weighted[PrevWinner] > BestW * HoldRatio;
				if (bMayHold)
				{
					Winner = PrevWinner;
					BestW = Weighted[PrevWinner];
					++StickyHold;
				}
				else
				{
					StickyHold = 0;
				}
			}
			else
			{
				// Raw winner agrees with previous — real support, not sticky hold.
				StickyHold = 0;
			}
			PrevWinner = Winner;

			// Punch winner so A/I/U/O travel further; Mouth Strength multiplies after.
			const float Raw = Vals[Winner];
			const float Punch = FMath::Clamp(FMath::Pow(FMath::Max(Raw, BestW * 0.85f), 0.55f) * 1.38f, 0.0f, 1.0f);
			Timeline[Jaw][f]    = (Winner == 0) ? Punch : 0.0f;
			Timeline[Wide][f]   = (Winner == 1) ? Punch : 0.0f;
			Timeline[Pucker][f] = (Winner == 2) ? Punch : 0.0f;
			Timeline[Funnel][f] = (Winner == 3) ? Punch : 0.0f;
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

	// --- Assemble per-slot timelines: emotion base, then lipsync, then blinks + gaze.
	TArray<TArray<float>> Timeline;
	Timeline.SetNum(SlotCount);
	for (TArray<float>& T : Timeline)
	{
		T.SetNumZeroed(NumFrames);
	}

	TArray<FString> Segments;
	{
		FString EmotionWork = Request.EmotionText;
		EmotionWork.ReplaceInline(TEXT(" Then "), TEXT(" then "));
		EmotionWork.ReplaceInline(TEXT(" THEN "), TEXT(" then "));
		EmotionWork.TrimStartAndEndInline();
		EmotionWork.ParseIntoArray(Segments, TEXT(" then "), true);
		if (Segments.Num() == 0 && !EmotionWork.IsEmpty())
		{
			Segments.Add(EmotionWork);
		}
	}
	const float EmotionStr = FMath::Clamp(Request.EmotionStrength, 0.0f, 2.5f);
	const int32 RampFrames = FMath::Max(1, FMath::RoundToInt32(0.35f * Fps));
	TArray<float> PrevPose, NextPose;
	PrevPose.SetNumZeroed(SlotCount);
	for (int32 SegIndex = 0; SegIndex < Segments.Num(); ++SegIndex)
	{
		NextPose.Reset();
		NextPose.SetNumZeroed(SlotCount);
		ParseEmotionSegment(Segments[SegIndex], NextPose.GetData(), EmotionStr);

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

	// ARKit morphs are authored so 1.0 = extreme travel. Emotion poses were
	// tuned for softer VRM full-face morphs — ease brows/mouth micro-slots on
	// layered faces so lips and brows don't rubber-band.
	if (Request.Profile.bLayeredBlendshapes)
	{
		const ECineFaceSlot SoftSlots[] = {
			ECineFaceSlot::BrowUp, ECineFaceSlot::BrowDown, ECineFaceSlot::BrowSad,
			ECineFaceSlot::MouthSmile, ECineFaceSlot::MouthFrown, ECineFaceSlot::MouthPress,
			ECineFaceSlot::NoseSneer, ECineFaceSlot::EyeWide, ECineFaceSlot::EyeSquint,
		};
		for (ECineFaceSlot S : SoftSlots)
		{
			for (float& V : Timeline[(int32)S])
			{
				V = FMath::Clamp(V * 0.70f, 0.0f, 1.0f);
			}
		}
		const ECineFaceSlot ExprSlotsSoft[] = {
			ECineFaceSlot::ExprHappy, ECineFaceSlot::ExprAngry,
			ECineFaceSlot::ExprSad, ECineFaceSlot::ExprSurprised,
		};
		for (ECineFaceSlot S : ExprSlotsSoft)
		{
			for (float& V : Timeline[(int32)S])
			{
				V = FMath::Clamp(V * 0.30f, 0.0f, 1.0f);
			}
		}
	}

	// Peak of expression slots for diagnostics.
	float PeakExpr = 0.0f;
	const ECineFaceSlot PeakSlots[] = {
		ECineFaceSlot::ExprHappy, ECineFaceSlot::ExprAngry, ECineFaceSlot::ExprSad, ECineFaceSlot::ExprSurprised,
		ECineFaceSlot::BrowDown, ECineFaceSlot::BrowSad, ECineFaceSlot::MouthSmile
	};
	for (ECineFaceSlot S : PeakSlots)
	{
		for (float V : Timeline[(int32)S]) { PeakExpr = FMath::Max(PeakExpr, V); }
	}
	UE_LOG(LogCineDirectorFaceBake, Log, TEXT("EmotionText=\"%s\" strength=%.2f peakExpr=%.2f segs=%d"),
		*Request.EmotionText, EmotionStr, PeakExpr, Segments.Num());

	// Snapshot expression slots so exclusive mouth processing can't wipe them.
	const ECineFaceSlot ExpressionSlots[] = {
		ECineFaceSlot::MouthSmile, ECineFaceSlot::MouthFrown, ECineFaceSlot::MouthPress,
		ECineFaceSlot::NoseSneer, ECineFaceSlot::BrowUp, ECineFaceSlot::BrowDown, ECineFaceSlot::BrowSad,
		ECineFaceSlot::EyeWide, ECineFaceSlot::EyeSquint,
		ECineFaceSlot::ExprHappy, ECineFaceSlot::ExprAngry, ECineFaceSlot::ExprSad, ECineFaceSlot::ExprSurprised,
	};
	TArray<TArray<float>> ExpressionHold;
	ExpressionHold.SetNum(UE_ARRAY_COUNT(ExpressionSlots));
	for (int32 i = 0; i < UE_ARRAY_COUNT(ExpressionSlots); ++i)
	{
		ExpressionHold[i] = Timeline[(int32)ExpressionSlots[i]];
	}

	// Lipsync on top of the emotion base (unit gain). Mouth Strength is applied
	// after exclusive visemes so the slider always has a clear, final effect.
	// Lips anticipate: speakers pre-shape wide/round mouths ~50 ms before the
	// voiced sound, so shape channels read slightly ahead while the jaw and
	// closures stay locked to the audio frame.
	const int32 LeadFrames = FMath::Clamp(FMath::RoundToInt32(0.055f * Fps), 1, 3);
	for (int32 Frame = 0; Frame < Request.Visemes.Num() && Frame < NumFrames; ++Frame)
	{
		const FCineVisemeFrame& V = Request.Visemes[Frame];
		const FCineVisemeFrame& VLead = Request.Visemes[FMath::Min(Frame + LeadFrames, Request.Visemes.Num() - 1)];
		const float Open = FMath::Clamp(V.Jaw * (1.0f - V.Close), 0.0f, 1.0f);
		Timeline[(int32)ECineFaceSlot::JawOpen][Frame] =
			FMath::Max(Timeline[(int32)ECineFaceSlot::JawOpen][Frame] * (1.0f - V.Close), Open);
		Timeline[(int32)ECineFaceSlot::MouthClose][Frame] =
			FMath::Max(Timeline[(int32)ECineFaceSlot::MouthClose][Frame], V.Close);
		Timeline[(int32)ECineFaceSlot::MouthPress][Frame] =
			FMath::Max(Timeline[(int32)ECineFaceSlot::MouthPress][Frame], V.Close * 0.7f);
		if (Open > 0.04f || V.Close > 0.3f || VLead.Wide > 0.05f || VLead.Pucker > 0.05f || VLead.Funnel > 0.05f)
		{
			const float ShapeWide = FMath::Clamp((VLead.Wide + VLead.Sibilant * 0.4f) * (1.0f - V.Close), 0.0f, 1.0f);
			const float ShapePucker = FMath::Clamp(VLead.Pucker * (1.0f - V.Close), 0.0f, 1.0f);
			// Tiny U→O bleed only — larger values kept Funnel alive and stuck exclusive O.
			const float ShapeFunnel = FMath::Clamp(
				FMath::Max(VLead.Funnel, VLead.Pucker * 0.08f) * (1.0f - V.Close), 0.0f, 1.0f);
			Timeline[(int32)ECineFaceSlot::MouthWide][Frame] =
				FMath::Max(Timeline[(int32)ECineFaceSlot::MouthWide][Frame] * 0.4f, ShapeWide);
			Timeline[(int32)ECineFaceSlot::MouthPucker][Frame] =
				FMath::Max(Timeline[(int32)ECineFaceSlot::MouthPucker][Frame] * 0.35f, ShapePucker);
			Timeline[(int32)ECineFaceSlot::MouthFunnel][Frame] =
				FMath::Max(Timeline[(int32)ECineFaceSlot::MouthFunnel][Frame] * 0.35f, ShapeFunnel);

			// Jaw co-articulation for layered rigs (ARKit/MetaHuman): EE/OO/OH
			// still drop the jaw under the lip shape — pursing or stretching over
			// a shut jaw is the classic mumble look. Exclusive A/I/U/O rigs pick
			// one morph per frame, so extra jaw there would fight the winner.
			if (!Request.Profile.bExclusiveVisemes)
			{
				const float JawSupport = FMath::Max3(
					ShapeFunnel * 0.50f, ShapePucker * 0.35f, ShapeWide * 0.28f);
				Timeline[(int32)ECineFaceSlot::JawOpen][Frame] = FMath::Max(
					Timeline[(int32)ECineFaceSlot::JawOpen][Frame], JawSupport * (1.0f - V.Close));
			}
		}
	}

	if (Request.Profile.bExclusiveVisemes)
	{
		ApplyExclusiveVisemes(Timeline, NumFrames);
	}

	// Articulation (panel slider): reshape mouth channels around a short local
	// average. Above 1 sharpens transitions — consonants snap shut, vowels peak
	// harder (stage enunciation); below 1 blends toward the average for a soft,
	// mumbly read. Close participates so closures crisp up too, but its travel
	// is never scaled.
	{
		const float Art = FMath::Clamp(Request.Articulation, 0.0f, 2.5f);
		if (!FMath::IsNearlyEqual(Art, 1.0f))
		{
			const ECineFaceSlot ArtSlots[] = {
				ECineFaceSlot::JawOpen, ECineFaceSlot::MouthWide, ECineFaceSlot::MouthPucker,
				ECineFaceSlot::MouthFunnel, ECineFaceSlot::MouthClose,
			};
			const int32 Radius = FMath::Max(1, Fps / 12); // ~80 ms half-window
			TArray<float> LocalAvg;
			LocalAvg.SetNumUninitialized(NumFrames);
			for (ECineFaceSlot Slot : ArtSlots)
			{
				TArray<float>& Track = Timeline[(int32)Slot];
				for (int32 f = 0; f < NumFrames; ++f)
				{
					float Sum = 0.0f;
					int32 Count = 0;
					for (int32 k = -Radius; k <= Radius; ++k)
					{
						const int32 i = f + k;
						if (i >= 0 && i < NumFrames)
						{
							Sum += Track[i];
							++Count;
						}
					}
					LocalAvg[f] = Sum / FMath::Max(1, Count);
				}
				if (Art < 1.0f)
				{
					for (int32 f = 0; f < NumFrames; ++f)
					{
						Track[f] = FMath::Lerp(LocalAvg[f], Track[f], Art);
					}
				}
				else
				{
					const float K = (Art - 1.0f) * 1.4f;
					for (int32 f = 0; f < NumFrames; ++f)
					{
						Track[f] = FMath::Clamp(Track[f] + (Track[f] - LocalAvg[f]) * K, 0.0f, 1.0f);
					}
				}
			}
		}
	}

	// Final mouth strength scale (panel slider). 0 = no mouth motion, 1 = as analyzed, 2 = double.
	// Layered ARKit faces get a mild built-in ease so default slider values don't
	// max every morph (void FBX morphs travel hard at 1.0).
	{
		float MouthMul = FMath::Clamp(Request.MouthStrength, 0.0f, 2.5f);
		if (Request.Profile.bLayeredBlendshapes)
		{
			MouthMul *= 0.85f;
		}
		const ECineFaceSlot MouthSlots[] = {
			ECineFaceSlot::JawOpen, ECineFaceSlot::MouthWide,
			ECineFaceSlot::MouthPucker, ECineFaceSlot::MouthFunnel,
			// Close/press stay unscaled so consonants still shut cleanly at low strength.
		};
		if (!FMath::IsNearlyEqual(MouthMul, 1.0f))
		{
			for (ECineFaceSlot Slot : MouthSlots)
			{
				TArray<float>& Track = Timeline[(int32)Slot];
				for (float& V : Track)
				{
					V = FMath::Clamp(V * MouthMul, 0.0f, 1.0f);
				}
			}
		}
	}

	// Restore brows / full-face expressions after exclusive mouth pass — but let
	// mouth-region emotion yield while the character is actually talking. A
	// maxed smile or pressed frown otherwise flattens every viseme into the same
	// shape; brows and eyes keep carrying the emotion at full strength. The
	// speech envelope is smoothed with a slow release so the yield breathes with
	// phrases instead of flickering per syllable.
	{
		TArray<float> TalkEnv;
		TalkEnv.SetNumZeroed(NumFrames);
		const int32 Jaw = (int32)ECineFaceSlot::JawOpen;
		const int32 Wide = (int32)ECineFaceSlot::MouthWide;
		const int32 Pucker = (int32)ECineFaceSlot::MouthPucker;
		const int32 Funnel = (int32)ECineFaceSlot::MouthFunnel;
		const int32 Close = (int32)ECineFaceSlot::MouthClose;
		// Emotion-only bakes (no visemes) keep their pose at full strength —
		// a surprised open jaw is the emotion, not speech to yield to.
		if (Request.Visemes.Num() > 0)
		{
			float Env = 0.0f;
			for (int32 f = 0; f < NumFrames; ++f)
			{
				const float Raw = FMath::Clamp(
					Timeline[Jaw][f] + Timeline[Wide][f] + Timeline[Pucker][f] + Timeline[Funnel][f]
						+ Timeline[Close][f] * 0.6f,
					0.0f, 1.0f);
				const float Alpha = Raw > Env ? 0.45f : 0.06f;
				Env += (Raw - Env) * Alpha;
				TalkEnv[f] = Env;
			}
		}

		// How much of each slot's emotion gives way at full speech.
		auto YieldFor = [](ECineFaceSlot Slot) -> float
		{
			switch (Slot)
			{
			case ECineFaceSlot::MouthSmile:
			case ECineFaceSlot::MouthFrown:
				return 0.55f;
			case ECineFaceSlot::MouthPress:
				return 0.45f;
			case ECineFaceSlot::NoseSneer:
				return 0.25f;
			case ECineFaceSlot::ExprHappy:
			case ECineFaceSlot::ExprAngry:
			case ECineFaceSlot::ExprSad:
			case ECineFaceSlot::ExprSurprised:
				return 0.30f; // full-face morphs open the mouth themselves on VRM
			default:
				return 0.0f; // brows and eyes hold the emotion
			}
		};

		for (int32 i = 0; i < UE_ARRAY_COUNT(ExpressionSlots); ++i)
		{
			const int32 Slot = (int32)ExpressionSlots[i];
			const float Yield = YieldFor(ExpressionSlots[i]);
			for (int32 f = 0; f < NumFrames; ++f)
			{
				const float Damp = 1.0f - Yield * TalkEnv[f];
				if (ExpressionSlots[i] == ECineFaceSlot::MouthPress)
				{
					// Keep the consonant press (rebuilt from closures) at full
					// strength; only the held emotion press yields.
					Timeline[Slot][f] = FMath::Max(Timeline[Close][f] * 0.7f, ExpressionHold[i][f] * Damp);
				}
				else if (Yield > 0.0f)
				{
					// Nothing but the emotion base writes these slots, so the
					// damped hold is the final value.
					Timeline[Slot][f] = ExpressionHold[i][f] * Damp;
				}
				else
				{
					Timeline[Slot][f] = FMath::Max(Timeline[Slot][f], ExpressionHold[i][f]);
				}
			}
		}
	}

	// Hard shut floor: any frame where jaw + shapes are all tiny → force zero.
	// Fixes residual half-open mouths after smoothing on both ARKit and VRM.
	{
		const int32 Jaw = (int32)ECineFaceSlot::JawOpen;
		const int32 Wide = (int32)ECineFaceSlot::MouthWide;
		const int32 Pucker = (int32)ECineFaceSlot::MouthPucker;
		const int32 Funnel = (int32)ECineFaceSlot::MouthFunnel;
		const int32 Close = (int32)ECineFaceSlot::MouthClose;
		for (int32 f = 0; f < NumFrames; ++f)
		{
			const float OpenAmt = Timeline[Jaw][f] + Timeline[Wide][f] + Timeline[Pucker][f] + Timeline[Funnel][f];
			if (OpenAmt < 0.08f)
			{
				Timeline[Jaw][f] = 0.0f;
				Timeline[Wide][f] = 0.0f;
				Timeline[Pucker][f] = 0.0f;
				Timeline[Funnel][f] = 0.0f;
				// Prefer an explicit close if the morph exists; harmless if not.
				if (Timeline[Close][f] < 0.3f)
				{
					Timeline[Close][f] = 0.0f; // don't force-close morph during pure rest
				}
			}
		}
	}

	// Teeth reveal: real speech shows teeth almost constantly. Raise the upper
	// lip and drop the lower lip with open/wide shapes; rounded and closed
	// mouths hide them again. Derived from the final mouth channels so Mouth
	// Strength and Articulation carry through, and mapped only on rigs that
	// have the morphs (ARKit mouthUpperUp/LowerDown, MetaHuman lip raise/depress).
	// Layered void FBX morphs travel hard — slightly lower caps there.
	{
		const int32 Jaw = (int32)ECineFaceSlot::JawOpen;
		const int32 Wide = (int32)ECineFaceSlot::MouthWide;
		const int32 Pucker = (int32)ECineFaceSlot::MouthPucker;
		const int32 Funnel = (int32)ECineFaceSlot::MouthFunnel;
		const int32 Close = (int32)ECineFaceSlot::MouthClose;
		const int32 Upper = (int32)ECineFaceSlot::MouthUpperUp;
		const int32 Lower = (int32)ECineFaceSlot::MouthLowerDown;
		const float UpperCap = Request.Profile.bLayeredBlendshapes ? 0.45f : 0.65f;
		const float LowerCap = Request.Profile.bLayeredBlendshapes ? 0.50f : 0.75f;
		for (int32 f = 0; f < NumFrames; ++f)
		{
			const float Round = FMath::Max(Timeline[Pucker][f], Timeline[Funnel][f]);
			const float Show = FMath::Clamp(1.0f - Round * 0.8f - Timeline[Close][f], 0.0f, 1.0f);
			const float UpperV = FMath::Clamp((Timeline[Jaw][f] * 0.30f + Timeline[Wide][f] * 0.35f) * Show, 0.0f, UpperCap);
			const float LowerV = FMath::Clamp((Timeline[Jaw][f] * 0.40f + Timeline[Wide][f] * 0.40f) * Show, 0.0f, LowerCap);
			Timeline[Upper][f] = FMath::Max(Timeline[Upper][f], UpperV);
			Timeline[Lower][f] = FMath::Max(Timeline[Lower][f], LowerV);
		}
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

	// Eye look / saccades — only worth baking when the mesh has look morphs.
	const bool bHasGaze = Request.Profile.HasSlot(ECineFaceSlot::EyeLookLeft)
		|| Request.Profile.HasSlot(ECineFaceSlot::EyeLookRight)
		|| Request.Profile.HasSlot(ECineFaceSlot::EyeLookUp)
		|| Request.Profile.HasSlot(ECineFaceSlot::EyeLookDown);
	if (bHasGaze)
	{
		BakeEyeGaze(Timeline, NumFrames, Fps, NumFrames * 13 + 11);
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

	UE_LOG(LogCineDirectorFaceBake, Log, TEXT("Baked '%s': %d curves, %d frames (%.1fs). exclusiveVisemes=%d layered=%d gaze=%d artic=%.2f"),
		*AssetName, CurvesWritten, NumFrames, (float)NumFrames / Fps,
		Request.Profile.bExclusiveVisemes ? 1 : 0, Request.Profile.bLayeredBlendshapes ? 1 : 0,
		bHasGaze ? 1 : 0, Request.Articulation);
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
