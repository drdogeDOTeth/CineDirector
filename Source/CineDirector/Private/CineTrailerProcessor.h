// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Parsed trailer style: what the treatment actually does, derived from a description. */
struct FCineTrailerStyle
{
	/** Color treatment filter chain; empty = the footage's natural look. */
	FString GradeFilter;

	bool bLetterbox = false;
	bool bShake = false;
	bool bFlicker = false;
	bool bChromaShift = false;
	/** Blinking REC + camera tag + running timecode. */
	bool bCamOverlays = false;

	/** noise filter strength: 0 = none, ~8 = light film grain, ~14 = heavy. */
	int32 GrainLevel = 8;

	/** Impact-style bold title instead of letterspaced serif. */
	bool bBoldTitleFont = false;

	/** Slice sizes of the source, in order; sums to 1. More/shrinking = faster cuts. */
	TArray<double> BeatFractions;

	enum class EMood : uint8
	{
		Eerie,
		Tense,
		Somber,
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

	/** Plain-language description of the edit ("found footage, eerie", "clean cinematic, b&w, tense"). */
	FString StyleDescription;

	/** Big title card text ("THE VISITORS"). */
	FString MovieTitle = TEXT("THE VISITORS");

	/** Up to three teaser card lines shown between beats. */
	TArray<FString> CardLines;

	/** Camera tag drawn top-right when camera overlays are on. */
	FString CameraTag = TEXT("SAFEHOUSE CAM 03");
};

/**
 * Cuts a rendered movie into a styled trailer with ffmpeg: beats with the
 * described treatment, letterspaced title cards between them, and a
 * synthesized score. Runs on a background thread.
 */
class FCineTrailerProcessor
{
public:
	/** Interpret a plain-language style description into concrete treatment knobs. */
	static FCineTrailerStyle ParseStyle(const FString& Description);

	/** OnDone(bSuccess, FinalPathOrError) is called on the game thread. */
	static void ProcessAsync(const FCineTrailerOptions& Options, TFunction<void(bool, FString)> OnDone);

	/** Newest .mp4 in a directory modified at/after MinTimeUtc; empty if none. */
	static FString FindNewestMp4(const FString& Directory, const FDateTime& MinTimeUtc);
};
