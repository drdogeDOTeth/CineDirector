// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineFaceTypes.h"
#include "CoreMinimal.h"

class AActor;
class UAnimSequence;
class USkeletalMesh;
class USoundWave;

/** Everything needed to bake one face performance. */
struct FCineFaceBakeRequest
{
	USkeletalMesh* Mesh = nullptr;
	FCineFaceProfile Profile;

	/** Mouth shapes from audio or synthesis; may be empty (emotion-only). */
	TArray<FCineVisemeFrame> Visemes;

	/**
	 * Plain-language emotions, "scared" or "calm then angry".
	 * Empty = neutral. When left blank in the UI and dialogue audio is present,
	 * the panel fills this via FCineLipsync::EstimateEmotionFromAudio.
	 */
	FString EmotionText;

	bool bAutoBlink = true;
	float DurationSeconds = 6.0f;
	int32 Fps = 30;

	/**
	 * Multipliers from the panel sliders (1.0 = baked defaults).
	 * MouthStrength scales lipsync jaw/shape travel.
	 * EmotionStrength scales brows / full-face Expr* poses.
	 */
	float MouthStrength = 1.3f;
	/** 1.0 = full pose table values; 0 = off; 2 = maxed. */
	float EmotionStrength = 1.0f;
};

/**
 * Bakes slot timelines (emotion base + lipsync + blinks) into a curves-only
 * additive UAnimSequence, and layers it into the open Level Sequence alongside
 * whatever body animation the character already has.
 */
class FCineFaceBaker
{
public:
	/** Creates the /Game/CineDirector/FaceAnims asset. Null + OutError on failure. */
	static UAnimSequence* BakeAnimAsset(const FCineFaceBakeRequest& Request, FString& OutError);

	/** Imports a .wav into /Game/CineDirector/Audio for Sequencer playback. */
	static USoundWave* ImportAudioAsset(const FString& WavPath, FString& OutError);

	/**
	 * Adds the face animation (and optional audio) to the current Level
	 * Sequence at the playback start, bound to the actor's skeletal mesh
	 * component. One undo transaction.
	 */
	static bool AddToSequencer(AActor* Actor, UAnimSequence* FaceAnim, USoundWave* Audio, FString& OutError);

	/** The emotion vocabulary, for help text ("scared, angry, happy, ..."). */
	static FString GetEmotionVocabulary();
};
