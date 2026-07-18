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

	/** Plain-language emotions, "scared" or "calm then angry"; empty = neutral. */
	FString EmotionText;

	bool bAutoBlink = true;
	float DurationSeconds = 6.0f;
	int32 Fps = 30;
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
