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

	/**
	 * Emphasize the vocal band and suppress steady instrumental beds so lipsync /
	 * emotion analysis can drive off a song's singing rather than the full mix.
	 * In-place on mono samples. Does not affect the file imported for playback.
	 *
	 * Pipeline: voice-range bandpass (≈120–4000 Hz) → soft noise gate against the
	 * quietest floor → syllable-rate envelope gate (kills sustained pads/bass that
	 * lack speech-like modulation). Not a full AI stem split, but enough for most
	 * pop/rock mixes where the vocal sits in the midrange.
	 */
	static void IsolateVoice(TArray<float>& InOutMono, int32 SampleRate);

	static TArray<FCineVisemeFrame> AnalyzeAudio(const TArray<float>& Mono, int32 SampleRate, int32 Fps = 30);

	/** Procedural syllables/pauses for "talking without audio", deterministic per seed. */
	static TArray<FCineVisemeFrame> SynthesizeTalking(float DurationSeconds, int32 Fps = 30, int32 Seed = 0);

	/**
	 * Infer plain-language emotion text from dialogue audio (energy, brightness,
	 * dynamics, pause rate). Returns phrases the face baker already understands
	 * ("angry", "slightly sad then very happy", …). Empty only if audio is silent.
	 * Manual Emotion box text should override this when the user types one.
	 */
	static FString EstimateEmotionFromAudio(const TArray<float>& Mono, int32 SampleRate);
};
