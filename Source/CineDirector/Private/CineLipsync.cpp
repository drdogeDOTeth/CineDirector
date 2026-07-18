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

	/** Attack/release one-pole smoothing across a frame series. */
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

	for (int32 i = 0; i < NumFrames; ++i)
	{
		const float E = FMath::Clamp(Energy[i] / PeakRms, 0.0f, 1.0f);
		FCineVisemeFrame& F = Frames[i];
		if (E < 0.05f)
		{
			continue; // silence: mouth relaxes shut via smoothing below
		}

		F.Jaw = FMath::Pow(E, 0.65f);
		F.Wide = F.Jaw * FMath::Clamp(HighRatio[i] * 1.8f - 0.35f, 0.0f, 1.0f);
		F.Pucker = F.Jaw * FMath::Clamp(LowRatio[i] * 2.0f - 0.5f, 0.0f, 1.0f);

		if (Sibilance[i] > 1.2f && E > 0.08f)
		{
			F.Sibilant = FMath::Clamp((Sibilance[i] - 1.2f) * 0.8f, 0.0f, 1.0f);
			F.Jaw *= 0.4f; // S/SH is said through the teeth
		}

		// A sharp dip surrounded by speech reads as an M/B/P closure.
		if (i > 1 && i + 2 < NumFrames)
		{
			const float Around = (Energy[i - 2] + Energy[i - 1] + Energy[i + 1] + Energy[i + 2]) / (4.0f * PeakRms);
			if (Around > 0.15f && E < Around * 0.35f)
			{
				F.Close = 1.0f;
			}
		}
	}

	// Smooth each channel with speech-like attack/release.
	TArray<float> Ch;
	Ch.SetNum(NumFrames);
	auto SmoothChannel = [&Frames, &Ch, NumFrames](float FCineVisemeFrame::* Member, float Attack, float Release)
	{
		for (int32 i = 0; i < NumFrames; ++i) { Ch[i] = Frames[i].*Member; }
		Smooth(Ch, Attack, Release);
		for (int32 i = 0; i < NumFrames; ++i) { Frames[i].*Member = Ch[i]; }
	};
	SmoothChannel(&FCineVisemeFrame::Jaw, 0.55f, 0.30f);
	SmoothChannel(&FCineVisemeFrame::Wide, 0.40f, 0.25f);
	SmoothChannel(&FCineVisemeFrame::Pucker, 0.40f, 0.25f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.85f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Sibilant, 0.50f, 0.35f);

	UE_LOG(LogCineDirectorLipsync, Log, TEXT("Analyzed %.1fs of audio into %d lipsync frames."),
		(float)Mono.Num() / SampleRate, NumFrames);
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
		const float Peak = Rand.FRandRange(0.35f, 0.9f);
		const int32 Shape = Rand.RandRange(0, 3); // 0/1 neutral-ish, 2 wide, 3 round

		const int32 Start = FMath::RoundToInt32(Time * Fps);
		const int32 Len = FMath::Max(2, FMath::RoundToInt32(SylLen * Fps));
		for (int32 i = 0; i < Len && Start + i < NumFrames; ++i)
		{
			const float T = (float)i / Len;
			const float Env = FMath::Sin(T * PI) * Peak;
			FCineVisemeFrame& F = Frames[Start + i];
			F.Jaw = FMath::Max(F.Jaw, Env);
			if (Shape == 2) { F.Wide = FMath::Max(F.Wide, Env * 0.7f); }
			if (Shape == 3) { F.Pucker = FMath::Max(F.Pucker, Env * 0.8f); }
		}

		Time += SylLen + Rand.FRandRange(0.02f, 0.08f);
		if (--SyllablesUntilPause <= 0)
		{
			// Occasional closure at the pause makes it read as real phrasing.
			const int32 CloseFrame = FMath::RoundToInt32(Time * Fps);
			if (CloseFrame < NumFrames && Rand.FRand() < 0.5f)
			{
				Frames[CloseFrame].Close = 1.0f;
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
	SmoothChannel(&FCineVisemeFrame::Jaw, 0.6f, 0.35f);
	SmoothChannel(&FCineVisemeFrame::Wide, 0.45f, 0.3f);
	SmoothChannel(&FCineVisemeFrame::Pucker, 0.45f, 0.3f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.85f, 0.55f);
	return Frames;
}
