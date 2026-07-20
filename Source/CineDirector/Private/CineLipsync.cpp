// Copyright Roundtree. All Rights Reserved.

#include "CineLipsync.h"

#include "CineRenderLauncher.h"
#include "HAL/PlatformProcess.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorLipsync, Log, All);

namespace
{
	uint32 ReadU32(const uint8* Data) { return Data[0] | (Data[1] << 8) | (Data[2] << 16) | (Data[3] << 24); }
	uint16 ReadU16(const uint8* Data) { return Data[0] | (Data[1] << 8); }

	/** Minimal RIFF/WAVE reader: PCM 8/16/24/32 and float32, any channel count. */
	bool ParseWav(const TArray<uint8>& Bytes, TArray<float>& OutMono, int32& OutSampleRate, FString& OutError)
	{
		if (Bytes.Num() < 44 || FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
		{
			OutError = TEXT("Not a RIFF/WAVE file.");
			return false;
		}

		uint16 Format = 0, Channels = 0, BitsPerSample = 0;
		uint32 SampleRate = 0;
		const uint8* DataChunk = nullptr;
		uint32 DataSize = 0;

		int32 Pos = 12;
		while (Pos + 8 <= Bytes.Num())
		{
			const uint8* Chunk = Bytes.GetData() + Pos;
			const uint32 ChunkSize = ReadU32(Chunk + 4);
			if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && ChunkSize >= 16)
			{
				Format = ReadU16(Chunk + 8);
				Channels = ReadU16(Chunk + 10);
				SampleRate = ReadU32(Chunk + 12);
				BitsPerSample = ReadU16(Chunk + 22);
				// WAVE_FORMAT_EXTENSIBLE: real format is the first two bytes of the GUID.
				if (Format == 0xFFFE && ChunkSize >= 40)
				{
					Format = ReadU16(Chunk + 8 + 24);
				}
			}
			else if (FMemory::Memcmp(Chunk, "data", 4) == 0)
			{
				DataChunk = Chunk + 8;
				DataSize = FMath::Min<uint32>(ChunkSize, Bytes.Num() - (Pos + 8));
			}
			Pos += 8 + ChunkSize + (ChunkSize & 1);
		}

		if (!DataChunk || Channels == 0 || SampleRate == 0)
		{
			OutError = TEXT("WAV is missing fmt/data chunks.");
			return false;
		}
		const bool bFloat = Format == 3;
		const bool bPcm = Format == 1;
		if (!bFloat && !bPcm)
		{
			OutError = FString::Printf(TEXT("Unsupported WAV encoding (format tag %d)."), Format);
			return false;
		}

		const int32 BytesPerSample = BitsPerSample / 8;
		if (BytesPerSample < 1 || BytesPerSample > 4)
		{
			OutError = FString::Printf(TEXT("Unsupported bit depth (%d)."), BitsPerSample);
			return false;
		}

		const int32 FrameCount = DataSize / (BytesPerSample * Channels);
		OutMono.SetNumUninitialized(FrameCount);
		OutSampleRate = (int32)SampleRate;

		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			float Sum = 0.0f;
			for (int32 Ch = 0; Ch < Channels; ++Ch)
			{
				const uint8* S = DataChunk + (Frame * Channels + Ch) * BytesPerSample;
				float Value = 0.0f;
				if (bFloat && BytesPerSample == 4)
				{
					Value = *reinterpret_cast<const float*>(S);
				}
				else if (BytesPerSample == 2)
				{
					Value = (int16)ReadU16(S) / 32768.0f;
				}
				else if (BytesPerSample == 1)
				{
					Value = (S[0] - 128) / 128.0f;
				}
				else if (BytesPerSample == 3)
				{
					int32 V = S[0] | (S[1] << 8) | (S[2] << 16);
					if (V & 0x800000) { V |= 0xFF000000; }
					Value = V / 8388608.0f;
				}
				else if (BytesPerSample == 4)
				{
					Value = (int32)ReadU32(S) / 2147483648.0f;
				}
				Sum += Value;
			}
			OutMono[Frame] = Sum / Channels;
		}
		return true;
	}

	/** Signal power near one frequency over a window (Goertzel). */
	float BandPower(const float* Samples, int32 Count, int32 SampleRate, float Freq)
	{
		const float Omega = 2.0f * PI * Freq / SampleRate;
		const float Coeff = 2.0f * FMath::Cos(Omega);
		float S0 = 0.0f, S1 = 0.0f, S2 = 0.0f;
		for (int32 i = 0; i < Count; ++i)
		{
			S0 = Samples[i] + Coeff * S1 - S2;
			S2 = S1;
			S1 = S0;
		}
		return FMath::Max(0.0f, S1 * S1 + S2 * S2 - Coeff * S1 * S2) / FMath::Max(1, Count);
	}

	/** Attack/release one-pole smoothing across a frame series. Higher alpha = faster. */
	void Smooth(TArray<float>& Values, float Attack, float Release)
	{
		float Prev = 0.0f;
		for (float& V : Values)
		{
			const float Alpha = V > Prev ? Attack : Release;
			V = Prev + (V - Prev) * Alpha;
			Prev = V;
		}
	}
}

bool FCineLipsync::LoadAudioMono(const FString& Path, TArray<float>& OutSamples, int32& OutSampleRate,
	FString& OutWavPath, FString& OutError)
{
	if (!FPaths::FileExists(Path))
	{
		OutError = FString::Printf(TEXT("Audio file not found: %s"), *Path);
		return false;
	}

	OutWavPath = Path;
	const bool bIsWav = FPaths::GetExtension(Path).Equals(TEXT("wav"), ESearchCase::IgnoreCase);
	if (!bIsWav)
	{
		// Convert through ffmpeg, preserving rate/channels so the same file can
		// be imported for Sequencer playback.
		const FString FFmpeg = FCineRenderLauncher::FindFFmpegExecutable();
		if (FFmpeg.IsEmpty())
		{
			OutError = TEXT("Only .wav can be read directly; converting other formats needs ffmpeg, which was not found.");
			return false;
		}
		const FString Converted = FPaths::ProjectSavedDir() / TEXT("CineDirectorFace") /
			FPaths::GetBaseFilename(Path) + TEXT("_conv.wav");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Converted), true);
		const FString Args = FString::Printf(TEXT("-y -i \"%s\" -acodec pcm_s16le \"%s\""), *Path, *Converted);
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(*FFmpeg, *Args, &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0 || !FPaths::FileExists(Converted))
		{
			OutError = FString::Printf(TEXT("ffmpeg could not convert '%s': %s"), *Path, *StdErr.Right(300));
			return false;
		}
		OutWavPath = Converted;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *OutWavPath))
	{
		OutError = FString::Printf(TEXT("Could not read %s"), *OutWavPath);
		return false;
	}
	return ParseWav(Bytes, OutSamples, OutSampleRate, OutError);
}

void FCineLipsync::IsolateVoice(TArray<float>& InOutMono, int32 SampleRate)
{
	if (InOutMono.Num() < 32 || SampleRate <= 0)
	{
		return;
	}

	const int32 N = InOutMono.Num();

	// --- 1) Cascaded one-pole highpass (~120 Hz) then lowpass (~4000 Hz).
	// Kills kick/sub and extreme high-hat air so the vocal formant band dominates.
	const float HpCutoff = 120.0f;
	const float LpCutoff = 4000.0f;
	const float HpRC = 1.0f / (2.0f * PI * HpCutoff);
	const float LpRC = 1.0f / (2.0f * PI * LpCutoff);
	const float Dt = 1.0f / (float)SampleRate;
	const float HpA = HpRC / (HpRC + Dt);
	const float LpA = Dt / (LpRC + Dt);

	TArray<float> Filtered;
	Filtered.SetNumUninitialized(N);
	{
		float PrevIn = InOutMono[0];
		float Hp = 0.0f;
		float Lp = 0.0f;
		// Two cascaded HP stages for steeper bass rejection.
		float Hp2 = 0.0f;
		float PrevHp = 0.0f;
		for (int32 i = 0; i < N; ++i)
		{
			const float In = InOutMono[i];
			Hp = HpA * (Hp + In - PrevIn);
			PrevIn = In;
			Hp2 = HpA * (Hp2 + Hp - PrevHp);
			PrevHp = Hp;
			Lp = Lp + LpA * (Hp2 - Lp);
			Filtered[i] = Lp;
		}
	}

	// --- 2) Frame-wise energy + soft noise gate (against quietest ~15%).
	const int32 Hop = FMath::Max(1, SampleRate / 100); // 10 ms
	const int32 NumFrames = FMath::Max(1, N / Hop);
	TArray<float> FrameRms;
	FrameRms.SetNumZeroed(NumFrames);
	float Peak = 1e-6f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		const int32 Start = f * Hop;
		const int32 Count = FMath::Min(Hop, N - Start);
		double SumSq = 0.0;
		for (int32 i = 0; i < Count; ++i)
		{
			const float S = Filtered[Start + i];
			SumSq += S * S;
		}
		const float Rms = FMath::Sqrt((float)(SumSq / FMath::Max(1, Count)));
		FrameRms[f] = Rms;
		Peak = FMath::Max(Peak, Rms);
	}

	TArray<float> Sorted = FrameRms;
	Sorted.Sort();
	const float Floor = Sorted[FMath::Clamp(Sorted.Num() / 7, 0, Sorted.Num() - 1)];
	const float GateOpen = FMath::Max(Peak * 0.06f, Floor * 2.2f);
	const float GateClose = GateOpen * 0.45f;

	// --- 3) Syllable-rate modulation gate: sustained pads/bass have steady energy;
	//      singing/speech modulates ~2–8 Hz. Keep frames that wiggle like a voice.
	TArray<float> Env;
	Env.SetNum(NumFrames);
	float SmoothE = 0.0f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		SmoothE = SmoothE * 0.7f + FrameRms[f] * 0.3f;
		Env[f] = SmoothE;
	}
	TArray<float> Modulation;
	Modulation.SetNum(NumFrames);
	for (int32 f = 0; f < NumFrames; ++f)
	{
		// Absolute derivative of the envelope ≈ syllable attack energy.
		const float Prev = Env[FMath::Max(0, f - 1)];
		const float Next = Env[FMath::Min(NumFrames - 1, f + 1)];
		Modulation[f] = FMath::Abs(Next - Prev);
	}
	// Smooth modulation and normalize.
	float PeakMod = 1e-6f;
	float PrevM = 0.0f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		PrevM = PrevM * 0.6f + Modulation[f] * 0.4f;
		Modulation[f] = PrevM;
		PeakMod = FMath::Max(PeakMod, PrevM);
	}
	for (float& M : Modulation)
	{
		M /= PeakMod;
	}

	// Build per-sample gain (linear interp across frames).
	TArray<float> Gain;
	Gain.SetNum(NumFrames);
	float PrevGain = 0.0f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		// Energy gate
		float G = 0.0f;
		if (FrameRms[f] >= GateOpen)
		{
			G = 1.0f;
		}
		else if (FrameRms[f] > GateClose)
		{
			G = (FrameRms[f] - GateClose) / FMath::Max(1e-6f, GateOpen - GateClose);
		}
		// Modulation boost: steady instruments stay quieter; voice opens up.
		const float ModBoost = 0.25f + 0.75f * FMath::Clamp(Modulation[f] * 1.6f, 0.0f, 1.0f);
		G *= ModBoost;
		// Mild attack/release on the gate itself
		const float Alpha = G > PrevGain ? 0.55f : 0.18f;
		PrevGain = PrevGain + (G - PrevGain) * Alpha;
		Gain[f] = PrevGain;
	}

	// Apply + normalize to original peak-ish level so lipsync thresholds still work.
	float OutPeak = 1e-6f;
	for (int32 i = 0; i < N; ++i)
	{
		const float T = (float)i / (float)Hop;
		const int32 F0 = FMath::Clamp((int32)T, 0, NumFrames - 1);
		const int32 F1 = FMath::Min(F0 + 1, NumFrames - 1);
		const float Frac = FMath::Clamp(T - F0, 0.0f, 1.0f);
		const float G = FMath::Lerp(Gain[F0], Gain[F1], Frac);
		const float S = Filtered[i] * G;
		InOutMono[i] = S;
		OutPeak = FMath::Max(OutPeak, FMath::Abs(S));
	}

	// Match roughly to 0.7 peak so quiet vocals after gating aren't buried.
	const float TargetPeak = 0.7f;
	if (OutPeak > 1e-5f)
	{
		const float Scale = TargetPeak / OutPeak;
		for (float& S : InOutMono)
		{
			S = FMath::Clamp(S * Scale, -1.0f, 1.0f);
		}
	}

	UE_LOG(LogCineDirectorLipsync, Log,
		TEXT("IsolateVoice: %d samples @ %d Hz, floor=%.5f gate=%.5f peakMod=%.5f"),
		N, SampleRate, Floor, GateOpen, PeakMod);
}

TArray<FCineVisemeFrame> FCineLipsync::AnalyzeAudio(const TArray<float>& Mono, int32 SampleRate, int32 Fps)
{
	TArray<FCineVisemeFrame> Frames;
	if (Mono.Num() == 0 || SampleRate <= 0)
	{
		return Frames;
	}

	const int32 Hop = FMath::Max(1, SampleRate / Fps);
	const int32 Window = Hop * 2;
	const int32 NumFrames = FMath::Max(1, Mono.Num() / Hop);
	Frames.SetNum(NumFrames);

	TArray<float> Energy, LowRatio, HighRatio, Sibilance;
	Energy.SetNumZeroed(NumFrames);
	LowRatio.SetNumZeroed(NumFrames);
	HighRatio.SetNumZeroed(NumFrames);
	Sibilance.SetNumZeroed(NumFrames);

	float PeakRms = 1e-6f;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const int32 Start = FMath::Clamp(i * Hop - Hop / 2, 0, Mono.Num() - 1);
		const int32 Count = FMath::Min(Window, Mono.Num() - Start);
		const float* S = Mono.GetData() + Start;

		float SumSq = 0.0f;
		for (int32 j = 0; j < Count; ++j)
		{
			SumSq += S[j] * S[j];
		}
		const float Rms = FMath::Sqrt(SumSq / FMath::Max(1, Count));
		Energy[i] = Rms;
		PeakRms = FMath::Max(PeakRms, Rms);

		// Three coarse bands: voiced low, vowel-defining mid, sibilant high.
		const float Low = BandPower(S, Count, SampleRate, 250.0f) + BandPower(S, Count, SampleRate, 500.0f);
		const float Mid = BandPower(S, Count, SampleRate, 1500.0f) + BandPower(S, Count, SampleRate, 2500.0f);
		const float High = BandPower(S, Count, SampleRate, 4500.0f) + BandPower(S, Count, SampleRate, 6500.0f);
		const float Total = Low + Mid + High + 1e-9f;
		LowRatio[i] = Low / Total;
		HighRatio[i] = (Mid + High) / Total;
		Sibilance[i] = High / (Low + 1e-9f);
	}

	// Adaptive noise floor: median of the quietest ~20% of frames, so room tone
	// and breathing don't keep the jaw slightly open forever.
	TArray<float> SortedEnergy = Energy;
	SortedEnergy.Sort();
	const float NoiseFloor = SortedEnergy[FMath::Clamp(SortedEnergy.Num() / 5, 0, SortedEnergy.Num() - 1)];
	const float SpeechGate = FMath::Max(PeakRms * 0.08f, NoiseFloor * 2.5f);

	for (int32 i = 0; i < NumFrames; ++i)
	{
		const float E = FMath::Clamp(Energy[i] / PeakRms, 0.0f, 1.0f);
		const float Gate = Energy[i] / FMath::Max(SpeechGate, 1e-6f);
		FCineVisemeFrame& F = Frames[i];

		// Below the speech gate: leave at zero (mouth fully shut after smooth).
		if (Gate < 1.0f)
		{
			// Soft ramp in the near-gate band so we don't click, but hard shut below 0.5.
			if (Gate < 0.5f)
			{
				continue;
			}
			// Partial: scaled residual only — no shape variety in almost-silence.
			F.Jaw = FMath::Pow(E, 0.8f) * Gate;
			continue;
		}

		// Clear speech: open with a curve that still reaches near-zero between syllables.
		F.Jaw = FMath::Pow(E, 0.75f);
		F.Wide = F.Jaw * FMath::Clamp(HighRatio[i] * 1.8f - 0.35f, 0.0f, 1.0f);
		F.Pucker = F.Jaw * FMath::Clamp(LowRatio[i] * 2.0f - 0.5f, 0.0f, 1.0f);

		if (Sibilance[i] > 1.2f && E > 0.08f)
		{
			F.Sibilant = FMath::Clamp((Sibilance[i] - 1.2f) * 0.8f, 0.0f, 1.0f);
			F.Jaw *= 0.35f; // S/SH is said through the teeth
		}

		// A sharp dip surrounded by speech reads as an M/B/P closure.
		if (i > 1 && i + 2 < NumFrames)
		{
			const float Around = (Energy[i - 2] + Energy[i - 1] + Energy[i + 1] + Energy[i + 2]) / (4.0f * PeakRms);
			if (Around > 0.12f && E < Around * 0.45f)
			{
				F.Close = 1.0f;
				F.Jaw = 0.0f;
				F.Wide = 0.0f;
				F.Pucker = 0.0f;
			}
		}
	}

	// Smooth each channel with speech-like attack/release.
	// Jaw release is intentionally fast so the mouth actually shuts between words.
	TArray<float> Ch;
	Ch.SetNum(NumFrames);
	auto SmoothChannel = [&Frames, &Ch, NumFrames](float FCineVisemeFrame::* Member, float Attack, float Release)
	{
		for (int32 i = 0; i < NumFrames; ++i) { Ch[i] = Frames[i].*Member; }
		Smooth(Ch, Attack, Release);
		for (int32 i = 0; i < NumFrames; ++i) { Frames[i].*Member = Ch[i]; }
	};
	SmoothChannel(&FCineVisemeFrame::Jaw, 0.65f, 0.62f);      // open quick, close quick
	SmoothChannel(&FCineVisemeFrame::Wide, 0.50f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Pucker, 0.50f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.90f, 0.70f);    // closures snap on, hold briefly
	SmoothChannel(&FCineVisemeFrame::Sibilant, 0.55f, 0.45f);

	// Second pass: if energy is clearly silent, force residual mouth shapes toward zero
	// so smoothing never leaves a half-open jaw parked forever.
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const float Gate = Energy[i] / FMath::Max(SpeechGate, 1e-6f);
		if (Gate < 0.45f)
		{
			const float Kill = FMath::Clamp(Gate / 0.45f, 0.0f, 1.0f);
			// Below ~0.2 gate → fully shut; between 0.2–0.45 → soft scale-down.
			const float Scale = Gate < 0.2f ? 0.0f : Kill;
			Frames[i].Jaw *= Scale;
			Frames[i].Wide *= Scale;
			Frames[i].Pucker *= Scale;
			Frames[i].Sibilant *= Scale;
			if (Gate < 0.2f)
			{
				Frames[i].Close = FMath::Max(Frames[i].Close, 0.85f);
			}
		}
	}

	UE_LOG(LogCineDirectorLipsync, Log, TEXT("Analyzed %.1fs of audio into %d lipsync frames (gate=%.4f peak=%.4f)."),
		(float)Mono.Num() / SampleRate, NumFrames, SpeechGate, PeakRms);
	return Frames;
}

TArray<FCineVisemeFrame> FCineLipsync::SynthesizeTalking(float DurationSeconds, int32 Fps, int32 Seed)
{
	const int32 NumFrames = FMath::Max(1, FMath::RoundToInt32(DurationSeconds * Fps));
	TArray<FCineVisemeFrame> Frames;
	Frames.SetNum(NumFrames);

	FRandomStream Rand(Seed);
	float Time = Rand.FRandRange(0.05f, 0.3f);
	int32 SyllablesUntilPause = Rand.RandRange(4, 9);

	while (Time < DurationSeconds)
	{
		const float SylLen = Rand.FRandRange(0.09f, 0.22f);
		const float Peak = Rand.FRandRange(0.45f, 0.95f);
		const int32 Shape = Rand.RandRange(0, 3); // 0/1 neutral-ish, 2 wide, 3 round

		const int32 Start = FMath::RoundToInt32(Time * Fps);
		const int32 Len = FMath::Max(2, FMath::RoundToInt32(SylLen * Fps));
		for (int32 i = 0; i < Len && Start + i < NumFrames; ++i)
		{
			const float T = (float)i / Len;
			// Sin envelope returns to zero at both ends → full close between syllables.
			const float Env = FMath::Sin(T * PI) * Peak;
			FCineVisemeFrame& F = Frames[Start + i];
			F.Jaw = FMath::Max(F.Jaw, Env);
			if (Shape == 2) { F.Wide = FMath::Max(F.Wide, Env * 0.7f); }
			if (Shape == 3) { F.Pucker = FMath::Max(F.Pucker, Env * 0.8f); }
		}

		// Brief closure at the end of every syllable (not just pauses).
		const int32 CloseAt = Start + Len;
		if (CloseAt < NumFrames)
		{
			Frames[CloseAt].Close = 1.0f;
			Frames[CloseAt].Jaw = 0.0f;
		}

		Time += SylLen + Rand.FRandRange(0.03f, 0.10f);
		if (--SyllablesUntilPause <= 0)
		{
			const int32 CloseFrame = FMath::RoundToInt32(Time * Fps);
			if (CloseFrame < NumFrames)
			{
				Frames[CloseFrame].Close = 1.0f;
				if (CloseFrame + 1 < NumFrames) { Frames[CloseFrame + 1].Close = 0.7f; }
			}
			Time += Rand.FRandRange(0.35f, 0.9f);
			SyllablesUntilPause = Rand.RandRange(4, 9);
		}
	}

	TArray<float> Ch;
	Ch.SetNum(NumFrames);
	auto SmoothChannel = [&Frames, &Ch, NumFrames](float FCineVisemeFrame::* Member, float Attack, float Release)
	{
		for (int32 i = 0; i < NumFrames; ++i) { Ch[i] = Frames[i].*Member; }
		Smooth(Ch, Attack, Release);
		for (int32 i = 0; i < NumFrames; ++i) { Frames[i].*Member = Ch[i]; }
	};
	SmoothChannel(&FCineVisemeFrame::Jaw, 0.70f, 0.65f);
	SmoothChannel(&FCineVisemeFrame::Wide, 0.50f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Pucker, 0.50f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.90f, 0.70f);
	return Frames;
}

FString FCineLipsync::EstimateEmotionFromAudio(const TArray<float>& Mono, int32 SampleRate)
{
	if (Mono.Num() < SampleRate / 4 || SampleRate <= 0)
	{
		return FString();
	}

	// Analyze at ~20fps — coarse enough for emotion, cheap enough for long lines.
	const int32 Fps = 20;
	const int32 Hop = FMath::Max(1, SampleRate / Fps);
	const int32 Window = Hop * 2;
	const int32 NumFrames = FMath::Max(1, Mono.Num() / Hop);

	TArray<float> Energy, Bright, Zcr;
	Energy.SetNumZeroed(NumFrames);
	Bright.SetNumZeroed(NumFrames);
	Zcr.SetNumZeroed(NumFrames);

	float PeakRms = 1e-6f;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const int32 Start = FMath::Clamp(i * Hop - Hop / 2, 0, Mono.Num() - 1);
		const int32 Count = FMath::Min(Window, Mono.Num() - Start);
		const float* S = Mono.GetData() + Start;

		float SumSq = 0.0f;
		int32 Crossings = 0;
		for (int32 j = 0; j < Count; ++j)
		{
			SumSq += S[j] * S[j];
			if (j > 0 && ((S[j - 1] >= 0.0f) != (S[j] >= 0.0f)))
			{
				++Crossings;
			}
		}
		const float Rms = FMath::Sqrt(SumSq / FMath::Max(1, Count));
		Energy[i] = Rms;
		PeakRms = FMath::Max(PeakRms, Rms);
		Zcr[i] = (float)Crossings / FMath::Max(1, Count);

		const float Low = BandPower(S, Count, SampleRate, 250.0f) + BandPower(S, Count, SampleRate, 500.0f);
		const float Mid = BandPower(S, Count, SampleRate, 1500.0f) + BandPower(S, Count, SampleRate, 2500.0f);
		const float High = BandPower(S, Count, SampleRate, 4500.0f) + BandPower(S, Count, SampleRate, 6500.0f);
		Bright[i] = (Mid + High) / (Low + Mid + High + 1e-9f);
	}

	TArray<float> Sorted = Energy;
	Sorted.Sort();
	const float NoiseFloor = Sorted[FMath::Clamp(Sorted.Num() / 5, 0, Sorted.Num() - 1)];
	const float SpeechGate = FMath::Max(PeakRms * 0.08f, NoiseFloor * 2.5f);

	// Chunk into ~2.5s segments so arcs like "calm then angry" fall out naturally.
	const float DurationSec = (float)Mono.Num() / SampleRate;
	const float SegLenSec = 2.5f;
	const int32 MaxSegs = 8;
	const int32 NumSegs = FMath::Clamp(FMath::CeilToInt(DurationSec / SegLenSec), 1, MaxSegs);
	const int32 FramesPerSeg = FMath::Max(1, NumFrames / NumSegs);

	struct FSegLabel
	{
		FString Name;      // base emotion word
		float Intensity;   // 0..1 strength for slightly/very
	};
	TArray<FSegLabel> Labels;
	Labels.Reserve(NumSegs);

	for (int32 Seg = 0; Seg < NumSegs; ++Seg)
	{
		const int32 A = Seg * FramesPerSeg;
		const int32 B = (Seg == NumSegs - 1) ? NumFrames : FMath::Min(NumFrames, (Seg + 1) * FramesPerSeg);
		if (A >= B)
		{
			continue;
		}

		double SumE = 0.0, SumE2 = 0.0, SumBright = 0.0, SumZcr = 0.0;
		int32 SpeechFrames = 0;
		int32 Attacks = 0;
		float PrevE = 0.0f;
		for (int32 i = A; i < B; ++i)
		{
			const float En = Energy[i] / PeakRms;
			if (Energy[i] < SpeechGate)
			{
				PrevE = En;
				continue;
			}
			++SpeechFrames;
			SumE += En;
			SumE2 += En * En;
			SumBright += Bright[i];
			SumZcr += Zcr[i];
			if (En > PrevE + 0.18f)
			{
				++Attacks;
			}
			PrevE = En;
		}

		if (SpeechFrames < 3)
		{
			Labels.Add({ TEXT("calm"), 0.3f });
			continue;
		}

		const float MeanE = (float)(SumE / SpeechFrames);
		const float VarE = FMath::Max(0.0f, (float)(SumE2 / SpeechFrames) - MeanE * MeanE);
		const float Dyn = FMath::Sqrt(VarE);
		const float MeanBright = (float)(SumBright / SpeechFrames);
		const float MeanZcr = (float)(SumZcr / SpeechFrames);
		const float PauseRatio = 1.0f - (float)SpeechFrames / (float)(B - A);
		const float AttackRate = (float)Attacks / FMath::Max(1.0f, (B - A) / (float)Fps); // per second

		// Arousal: loud + dynamic + punchy attacks.
		const float Arousal = FMath::Clamp(MeanE * 0.55f + Dyn * 1.4f + AttackRate * 0.15f, 0.0f, 1.5f);
		// Brightness / tension: brighter + higher ZCR → fear/surprise vs warm/happy.
		const float Tension = FMath::Clamp(MeanBright * 0.7f + MeanZcr * 8.0f, 0.0f, 1.5f);
		// Soft/slow: high pause + low energy → sadness.
		const float Softness = FMath::Clamp(PauseRatio * 0.9f + (1.0f - MeanE) * 0.5f, 0.0f, 1.5f);

		FString Name = TEXT("calm");
		float Intensity = 0.45f;

		if (Arousal > 0.55f && Tension > 0.75f && AttackRate > 1.2f)
		{
			// Sudden loud bright bursts
			Name = TEXT("surprised");
			Intensity = FMath::Clamp(Arousal, 0.55f, 1.0f);
		}
		else if (Arousal > 0.62f && MeanBright < 0.55f && Dyn > 0.12f)
		{
			// Loud, darker, jagged — anger
			Name = TEXT("angry");
			Intensity = FMath::Clamp(Arousal * 0.9f + Dyn, 0.55f, 1.0f);
		}
		else if (Arousal > 0.55f && Tension > 0.85f && PauseRatio < 0.35f)
		{
			// High, tense, continuous — fear
			Name = TEXT("scared");
			Intensity = FMath::Clamp(Tension * 0.7f + Arousal * 0.4f, 0.5f, 1.0f);
		}
		else if (Softness > 0.7f && Arousal < 0.4f)
		{
			// Quiet, sparse — sadness
			Name = TEXT("sad");
			Intensity = FMath::Clamp(Softness * 0.8f, 0.45f, 1.0f);
		}
		else if (Arousal > 0.4f && MeanBright > 0.52f && Dyn < 0.22f && PauseRatio < 0.45f)
		{
			// Moderate energy, bright, steady — happiness (capped; Joy morphs go wide fast)
			Name = TEXT("happy");
			Intensity = FMath::Clamp(0.35f + MeanE * 0.35f + MeanBright * 0.15f, 0.4f, 0.55f);
		}
		else if (Arousal > 0.5f && Dyn > 0.18f && MeanBright > 0.48f)
		{
			// Energetic but not clearly angry — mild happy, not a super-grin
			Name = TEXT("happy");
			Intensity = FMath::Clamp(Arousal * 0.45f, 0.4f, 0.55f);
		}
		else if (Arousal < 0.28f)
		{
			Name = TEXT("calm");
			Intensity = 0.35f;
		}
		else if (MeanBright < 0.42f && Softness > 0.45f)
		{
			Name = TEXT("sad");
			Intensity = FMath::Clamp(0.4f + Softness * 0.4f, 0.4f, 0.85f);
		}
		else if (Arousal > 0.48f && MeanBright < 0.48f)
		{
			// Mild dark energy — suspicious / low-key anger
			Name = TEXT("suspicious");
			Intensity = FMath::Clamp(Arousal * 0.7f, 0.4f, 0.8f);
		}
		else
		{
			// Default speaking: light engagement rather than a blank face.
			if (MeanE > 0.25f && MeanBright > 0.45f)
			{
				Name = TEXT("happy");
				Intensity = 0.48f; // formats as "slightly happy"
			}
			else
			{
				Name = TEXT("calm");
				Intensity = 0.4f;
			}
		}

		Labels.Add({ Name, Intensity });
	}

	if (Labels.Num() == 0)
	{
		return FString();
	}

	// Collapse consecutive identical base emotions; keep max intensity.
	TArray<FSegLabel> Collapsed;
	for (const FSegLabel& L : Labels)
	{
		// Strip any prefix we might have baked into Name for the special case
		FString Base = L.Name;
		Base.ReplaceInline(TEXT("slightly "), TEXT(""));
		Base.ReplaceInline(TEXT("very "), TEXT(""));

		if (Collapsed.Num() > 0)
		{
			FString PrevBase = Collapsed.Last().Name;
			PrevBase.ReplaceInline(TEXT("slightly "), TEXT(""));
			PrevBase.ReplaceInline(TEXT("very "), TEXT(""));
			if (PrevBase.Equals(Base, ESearchCase::IgnoreCase))
			{
				Collapsed.Last().Intensity = FMath::Max(Collapsed.Last().Intensity, L.Intensity);
				continue;
			}
		}
		Collapsed.Add({ Base, L.Intensity });
	}

	// Format with slightly/very from intensity.
	TArray<FString> Parts;
	for (const FSegLabel& L : Collapsed)
	{
		if (L.Name.Equals(TEXT("calm"), ESearchCase::IgnoreCase) && Collapsed.Num() > 1 && L.Intensity < 0.5f)
		{
			// Skip weak calm segments sandwiched in an arc — keeps "angry then sad" clean.
			continue;
		}
		FString Part;
		if (L.Intensity >= 0.85f && !L.Name.Equals(TEXT("calm"), ESearchCase::IgnoreCase))
		{
			Part = FString::Printf(TEXT("very %s"), *L.Name);
		}
		else if (L.Intensity <= 0.5f && !L.Name.Equals(TEXT("calm"), ESearchCase::IgnoreCase))
		{
			Part = FString::Printf(TEXT("slightly %s"), *L.Name);
		}
		else
		{
			Part = L.Name;
		}
		Parts.Add(Part);
	}

	if (Parts.Num() == 0)
	{
		return TEXT("calm");
	}

	const FString Result = FString::Join(Parts, TEXT(" then "));
	UE_LOG(LogCineDirectorLipsync, Log, TEXT("Audio emotion estimate: %s (%.1fs, %d segments → %d labels)"),
		*Result, DurationSec, NumSegs, Parts.Num());
	return Result;
}
