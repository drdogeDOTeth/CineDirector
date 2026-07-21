// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Parsed trailer style: concrete treatment knobs derived from a plain-language
 * description (kits, grades, texture, effects, pacing, titles).
 */
struct FCineTrailerStyle
{
	/** Color treatment filter chain; empty = the footage's natural look. */
	FString GradeFilter;

	// --- Frame / camera -------------------------------------------------------
	bool bLetterbox = false;
	bool bShake = false;
	/** Shake intensity scale (1 = default handheld, ~1.6 = chaotic). */
	float ShakeIntensity = 1.0f;
	bool bDutch = false;
	/** Subtle push-in over each beat. */
	bool bZoomPunch = false;

	// --- Texture / degradation ------------------------------------------------
	bool bFlicker = false;
	bool bChromaShift = false;
	/** Blinking REC + camera tag + running timecode. */
	bool bCamOverlays = false;
	/** Horizontal CRT scanline darken every Nth row. */
	bool bScanlines = false;
	bool bVignette = false;
	bool bBloom = false;
	bool bSharpen = false;
	bool bSoft = false;
	bool bGlitch = false;
	bool bPixelate = false;
	bool bInvert = false;
	bool bMirror = false;

	/** noise filter strength: 0 = none, ~8 = light film grain, ~14 = heavy. */
	int32 GrainLevel = 8;

	// --- Transitions on each beat --------------------------------------------
	float BeatFadeIn = 0.35f;
	float BeatFadeOut = 0.40f;

	/** Impact-style bold title instead of letterspaced serif. */
	bool bBoldTitleFont = false;

	/** Slice sizes of the source, in order; sums to 1. More/shrinking = faster cuts. */
	TArray<double> BeatFractions;

	enum class EMood : uint8
	{
		Eerie,
		Tense,
		Somber,
		/** Punchy higher-energy bed for music-video edits. */
		Upbeat,
		None,
	};
	EMood Mood = EMood::Eerie;

	/** The recurring whistle melody over the bed. */
	bool bWhistleMotif = true;

	/** Human-readable interpretation, for the log/status. */
	FString Summary;
};

/** Settings for the trailer cut. */
struct FCineTrailerOptions
{
	/** The rendered movie to cut (mp4). */
	FString SourceVideo;

	/** Where the finished trailer is written. */
	FString OutputDirectory;

	/** Plain-language description of the edit ("music video, scanlines, film grain"). */
	FString StyleDescription;

	/** Big title card text ("THE VISITORS"). */
	FString MovieTitle = TEXT("THE VISITORS");

	/** Up to three teaser card lines shown between beats. */
	TArray<FString> CardLines;

	/** Camera tag drawn top-right when camera overlays are on. */
	FString CameraTag = TEXT("SAFEHOUSE CAM 03");

	/**
	 * Keep audio from the rendered source MP4, cut to match trailer beats.
	 * Default true so music-video / dialogue renders are not stripped.
	 */
	bool bKeepSourceAudio = true;

	/**
	 * Layer the synthetic trailer score (drone / whistle / upbeat bed).
	 * Default false — turn on for classic trailer music; combine with
	 * Keep Source for a bed under the original track.
	 */
	bool bAddSyntheticScore = false;
};

/**
 * Cuts a rendered movie into a styled trailer with ffmpeg: beats with the
 * described treatment, letterspaced title cards between them, and optional
 * synthesized score. Runs on a background thread.
 */
class FCineTrailerProcessor
{
public:
	/** Interpret a plain-language style description into concrete treatment knobs. */
	static FCineTrailerStyle ParseStyle(const FString& Description);

	/**
	 * Human-readable vocabulary of kits / grades / effects / pacing the parser
	 * understands — for tooltips and the panel help block.
	 */
	static FString GetStyleVocabulary();

	/** OnDone(bSuccess, FinalPathOrError) is called on the game thread. */
	static void ProcessAsync(const FCineTrailerOptions& Options, TFunction<void(bool, FString)> OnDone);

	/** Newest .mp4 in a directory modified at/after MinTimeUtc; empty if none. */
	static FString FindNewestMp4(const FString& Directory, const FDateTime& MinTimeUtc);
};
