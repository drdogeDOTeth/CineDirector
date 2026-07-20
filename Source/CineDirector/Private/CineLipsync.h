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
	 * Fast DSP voice emphasis (bandpass + gate). Fallback when AI stem separation
	 * is not available. In-place on mono samples; does not affect playback audio.
	 */
	static void IsolateVoiceDsp(TArray<float>& InOutMono, int32 SampleRate);

	/** @deprecated Use IsolateVoiceDsp or IsolateVoiceForLipsync. */
	static void IsolateVoice(TArray<float>& InOutMono, int32 SampleRate);

	/**
	 * Best-effort vocal isolation for lipsync analysis.
	 * 1) Try Demucs AI stem separation (if installed) — UVR-class quality.
	 * 2) Else fall back to IsolateVoiceDsp.
	 *
	 * @param SourceAudioPath Original or converted file path (fed to Demucs).
	 * @param InOutMono       Mix mono samples; replaced with isolated (or blend).
	 * @param InOutSampleRate Sample rate of InOutMono (may change if AI output differs).
	 * @param BlendStrength   0 = keep mix, 1 = full isolated vocal.
	 * @param OutMethod       "demucs", "dsp", or "none".
	 * @param OutNote         Human-readable status (install hint, cache hit, etc.).
	 */
	static void IsolateVoiceForLipsync(const FString& SourceAudioPath, TArray<float>& InOutMono,
		int32& InOutSampleRate, float BlendStrength, FString& OutMethod, FString& OutNote);

	/** True if a Demucs CLI entry point was found (python -m demucs or demucs.exe). */
	static bool IsDemucsAvailable();

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
