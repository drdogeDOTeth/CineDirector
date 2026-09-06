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
	/** Continuous audio emotion; used when the direction box is left blank. */
	TArray<FCineEmotionFrame> AudioEmotions;

	/** Saved gain/offset profile for this skeletal mesh. */
	FCineFaceCalibration Calibration;


	/**
	 * Plain-language emotions, "scared" or "calm then angry".
	 * Empty = neutral. When left blank in the UI and dialogue audio is present,
	 * the panel fills this via FCineLipsync::EstimateEmotionFromAudio.
	 */
	FString EmotionText;

	/**
	 * EmotionText came from audio analysis rather than an explicit direction.
	 * Inferred emotion is faded to neutral around leading/trailing silence so
	 * the character does not hold a speech-derived mouth pose while at rest.
	 */
	bool bEmotionFromAudio = false;

	bool bAutoBlink = true;
	float DurationSeconds = 6.0f;
	int32 Fps = 30;

	/**
	 * Multipliers from the panel sliders (1.0 = baked defaults).
	 * MouthStrength scales lipsync jaw/shape travel.
	 * EmotionStrength scales brows / full-face Expr* poses.
	 */
	float MouthStrength = 1.0f;
	/** 1.0 = full pose table values; 0 = off; 2 = maxed. */
	float EmotionStrength = 0.6f;
	/**
	 * How crisply the mouth hits each viseme. 1 = as analyzed; below 1 blends
	 * toward its local average (soft, mumbly); above 1 sharpens transitions so
	 * consonants snap and vowels peak harder (stage enunciation).
	 */
	float Articulation = 1.0f;

	/**
	 * When false (default), reuses / overwrites /Game/CineDirector/FaceAnims/<Mesh>_Face
	 * so iterative bakes do not pile up assets. When true, creates a unique
	 * timestamped take for archival.
	 */
	bool bKeepAsNewTake = false;
};

/**
 * Bakes slot timelines (emotion base + lipsync + blinks) into a curves-only
 * additive UAnimSequence, and layers it into the open Level Sequence alongside
 * whatever body animation the character already has.
 */
class FCineFaceBaker
{
public:
	/** Content folder for baked face takes. */
	static const TCHAR* FaceAnimFolder() { return TEXT("/Game/CineDirector/FaceAnims"); }

	/**
	 * Creates or reuses a /Game/CineDirector/FaceAnims asset, then saves only
	 * that package. Null + OutError on failure.
	 */
	static UAnimSequence* BakeAnimAsset(const FCineFaceBakeRequest& Request, FString& OutError);

	/** Imports a .wav into /Game/CineDirector/Audio for Sequencer playback. */
	static USoundWave* ImportAudioAsset(const FString& WavPath, FString& OutError);

	/**
	 * Adds the face animation (and optional audio) to the current Level
	 * Sequence at the playback start, bound to the actor's skeletal mesh
	 * component. Reuses an existing section that already points at the same
	 * asset (or any prior CineDirector face take on the track) instead of
	 * stacking duplicates. One undo transaction.
	 */
	static bool AddToSequencer(AActor* Actor, UAnimSequence* FaceAnim, USoundWave* Audio, FString& OutError);

	/**
	 * Deletes FaceAnims assets with no package referencers (not used by any
	 * sequence/map). Returns how many assets were removed.
	 */
	static int32 PurgeUnusedFaceAnims(FString& OutMessage);

	/** Clears Saved/CineDirectorFace stem / conversion cache. Returns files removed. */
	static int32 ClearFaceCache(FString& OutMessage);

	/** The emotion vocabulary, for help text ("scared, angry, happy, ..."). */
	static FString GetEmotionVocabulary();
};
