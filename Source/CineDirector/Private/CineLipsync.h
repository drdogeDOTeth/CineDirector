// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineFaceTypes.h"
#include "CoreMinimal.h"

/**
 * Turns dialogue audio into per-frame mouth shapes (energy envelope drives the
 * jaw, spectral balance picks wide/round shapes, dips become consonant
 * closures), or synthesizes plausible "talking" when there is no audio yet.
 */
class FCineLipsync
{
public:
	/**
	 * Load any audio file as mono float samples. WAV is parsed directly;
	 * anything else (mp3, ogg, m4a...) is converted with ffmpeg first.
	 * @param OutWavPath The .wav actually used — for non-wav input this is the
	 *        converted copy, which is also what should be imported for playback.
	 */
	static bool LoadAudioMono(const FString& Path, TArray<float>& OutSamples, int32& OutSampleRate,
		FString& OutWavPath, FString& OutError);

	static TArray<FCineVisemeFrame> AnalyzeAudio(const TArray<float>& Mono, int32 SampleRate, int32 Fps = 30);

	/** Procedural syllables/pauses for "talking without audio", deterministic per seed. */
	static TArray<FCineVisemeFrame> SynthesizeTalking(float DurationSeconds, int32 Fps = 30, int32 Seed = 0);
};
