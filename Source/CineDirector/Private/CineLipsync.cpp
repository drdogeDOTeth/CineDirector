// Copyright Roundtree. All Rights Reserved.

#include "CineLipsync.h"

#include "CineRenderLauncher.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

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

namespace
{
	FString FindPythonExecutable()
	{
		// Prefer known install paths first (Scripts often not on PATH after winget).
		const FString LocalApp = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		const TCHAR* Known[] = {
			TEXT("Programs\\Python\\Python312\\python.exe"),
			TEXT("Programs\\Python\\Python313\\python.exe"),
			TEXT("Programs\\Python\\Python311\\python.exe"),
			TEXT("Programs\\Python\\Python310\\python.exe"),
			TEXT("anaconda3\\python.exe"),
			TEXT("miniconda3\\python.exe"),
		};
		for (const TCHAR* Rel : Known)
		{
			const FString P1 = FPaths::Combine(LocalApp, Rel);
			const FString P2 = FPaths::Combine(UserProfile, Rel);
			if (FPaths::FileExists(P1)) return P1;
			if (FPaths::FileExists(P2)) return P2;
		}

		// Prefer real interpreters; skip broken Windows Store stubs.
		const TCHAR* Candidates[] = { TEXT("python"), TEXT("python3"), TEXT("py") };
		for (const TCHAR* Name : Candidates)
		{
			int32 Code = -1;
			FString Out, Err;
			if (FCString::Strcmp(Name, TEXT("py")) == 0)
			{
				FPlatformProcess::ExecProcess(TEXT("py"), TEXT("-3 -c \"import sys; print(sys.executable)\""), &Code, &Out, &Err);
			}
			else
			{
				FPlatformProcess::ExecProcess(Name, TEXT("-c \"import sys; print(sys.executable)\""), &Code, &Out, &Err);
			}
			if (Code == 0)
			{
				FString Exe = Out.TrimStartAndEnd();
				int32 Nl = INDEX_NONE;
				if (Exe.FindChar(TEXT('\n'), Nl))
				{
					Exe.LeftInline(Nl);
					Exe.TrimStartAndEndInline();
				}
				if (!Exe.IsEmpty() && !Exe.Contains(TEXT("WindowsApps")) && FPaths::FileExists(Exe))
				{
					return Exe;
				}
			}
		}
		return FString();
	}

	bool DemucsModuleImportable(const FString& PythonExe)
	{
		int32 Code = -1;
		FString Out, Err;
		FPlatformProcess::ExecProcess(*PythonExe, TEXT("-c \"import demucs; print('ok')\""), &Code, &Out, &Err);
		return Code == 0 && Out.Contains(TEXT("ok"));
	}

	FString FindDemucsExe()
	{
		const FString LocalApp = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString ScriptsDemucs = FPaths::Combine(LocalApp, TEXT("Programs\\Python\\Python312\\Scripts\\demucs.exe"));
		if (FPaths::FileExists(ScriptsDemucs))
		{
			return ScriptsDemucs;
		}

		int32 Code = -1;
		FString Out, Err;
		FPlatformProcess::ExecProcess(TEXT("where"), TEXT("demucs"), &Code, &Out, &Err);
		if (Code == 0)
		{
			FString Line = Out.TrimStartAndEnd();
			int32 Nl = INDEX_NONE;
			if (Line.FindChar(TEXT('\n'), Nl))
			{
				Line.LeftInline(Nl);
				Line.TrimStartAndEndInline();
			}
			if (FPaths::FileExists(Line))
			{
				return Line;
			}
		}
		return FString();
	}

	FString MakeStemCacheKey(const FString& InputPath)
	{
		const int64 Size = IFileManager::Get().FileSize(*InputPath);
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*InputPath);
		const FString Raw = FString::Printf(TEXT("%s|%lld|%s"), *FPaths::ConvertRelativePathToFull(InputPath),
			Size, *Stamp.ToString());
		return FMD5::HashAnsiString(*Raw);
	}
}

bool FCineLipsync::IsDemucsAvailable()
{
	if (!FindDemucsExe().IsEmpty())
	{
		return true;
	}
	const FString Py = FindPythonExecutable();
	return !Py.IsEmpty() && DemucsModuleImportable(Py);
}

void FCineLipsync::IsolateVoiceForLipsync(const FString& SourceAudioPath, TArray<float>& InOutMono,
	int32& InOutSampleRate, float BlendStrength, FString& OutMethod, FString& OutNote)
{
	OutMethod = TEXT("none");
	OutNote.Empty();
	const float W = FMath::Clamp(BlendStrength, 0.0f, 1.0f);
	if (W <= 0.01f || InOutMono.Num() < 32)
	{
		OutNote = TEXT("isolation blend at 0 — using full mix");
		return;
	}

	TArray<float> Original = InOutMono;
	const int32 OriginalRate = InOutSampleRate;

	// --- 1) Demucs AI stem split (cached) ---
	// Windows + song titles with spaces/& break Demucs path handling, so we always
	// copy the source to a short ASCII-only job folder and run with that CWD.
	FString VocalsPath;
	FString AiError;
	bool bAiOk = false;
	const FString AbsSource = FPaths::ConvertRelativePathToFull(SourceAudioPath);
	if (FPaths::FileExists(AbsSource))
	{
		const FString CacheRoot = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("CineDirectorFace") / TEXT("Stems"));
		const FString Key = MakeStemCacheKey(AbsSource);
		// Keep path short: hash only, no song title folders.
		const FString JobDir = CacheRoot / Key;
		const FString SafeInput = JobDir / TEXT("input.wav");
		const FString SafeOutRel = TEXT("out"); // relative to JobDir
		IFileManager::Get().MakeDirectory(*JobDir, true);

		// Look for a previous vocals.wav under this job dir.
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *JobDir, TEXT("vocals.wav"), true, false);
		if (Found.Num() > 0)
		{
			VocalsPath = Found[0];
			bAiOk = true;
			OutNote = TEXT("Demucs cache hit");
		}
		else
		{
			// Copy source → input.wav (avoids spaces, commas, & in path).
			if (!FPaths::FileExists(SafeInput))
			{
				const uint32 CopyResult = IFileManager::Get().Copy(*SafeInput, *AbsSource);
				if (CopyResult != COPY_OK)
				{
					AiError = FString::Printf(TEXT("Could not stage audio for Demucs (copy code %u)"), CopyResult);
				}
			}

			if (AiError.IsEmpty() && FPaths::FileExists(SafeInput))
			{
				const FString DemucsExe = FindDemucsExe();
				const FString PythonExe = FindPythonExecutable();
				FString Cmd, Args;
				// Relative paths + working directory = no OneDrive/space/& path bugs.
				// -d cuda uses the GPU when PyTorch CUDA is installed.
				if (!DemucsExe.IsEmpty())
				{
					Cmd = DemucsExe;
					Args = FString::Printf(
						TEXT("-n htdemucs --two-stems=vocals -d cuda -o %s input.wav"), *SafeOutRel);
				}
				else if (!PythonExe.IsEmpty() && DemucsModuleImportable(PythonExe))
				{
					Cmd = PythonExe;
					Args = FString::Printf(
						TEXT("-m demucs -n htdemucs --two-stems=vocals -d cuda -o %s input.wav"), *SafeOutRel);
				}

				if (!Cmd.IsEmpty())
				{
					OutNote = TEXT("Running Demucs on GPU (first time can take a few minutes)…");
					UE_LOG(LogCineDirectorLipsync, Log, TEXT("Demucs cwd=%s | %s %s"), *JobDir, *Cmd, *Args);
					int32 Code = -1;
					FString StdOut, StdErr;
					// OptionalWorkingDirectory keeps Demucs off relative ..\..\Users\... paths.
					FPlatformProcess::ExecProcess(*Cmd, *Args, &Code, &StdOut, &StdErr, *JobDir);
					// If CUDA fails (CPU-only torch), retry without -d cuda.
					if (Code != 0)
					{
						UE_LOG(LogCineDirectorLipsync, Warning,
							TEXT("Demucs cuda run failed (code %d), retrying on default device…\n%s"),
							Code, *StdErr.Right(400));
						if (!DemucsExe.IsEmpty())
						{
							Args = FString::Printf(
								TEXT("-n htdemucs --two-stems=vocals -o %s input.wav"), *SafeOutRel);
						}
						else
						{
							Args = FString::Printf(
								TEXT("-m demucs -n htdemucs --two-stems=vocals -o %s input.wav"), *SafeOutRel);
						}
						Code = -1;
						StdOut.Empty();
						StdErr.Empty();
						FPlatformProcess::ExecProcess(*Cmd, *Args, &Code, &StdOut, &StdErr, *JobDir);
					}

					Found.Reset();
					IFileManager::Get().FindFilesRecursive(Found, *JobDir, TEXT("vocals.wav"), true, false);
					if (Code == 0 && Found.Num() > 0)
					{
						VocalsPath = Found[0];
						bAiOk = true;
						OutNote = TEXT("Demucs AI vocals");
					}
					else
					{
						AiError = FString::Printf(TEXT("Demucs failed (code %d): %s"), Code, *StdErr.Right(500));
						UE_LOG(LogCineDirectorLipsync, Warning, TEXT("%s\nstdout: %s"), *AiError, *StdOut.Right(300));
					}
				}
				else
				{
					AiError = TEXT("Demucs not found. Install with: pip install demucs  (needs a real Python, not the Store stub)");
				}
			}
		}
	}

	if (bAiOk && !VocalsPath.IsEmpty())
	{
		TArray<float> VocalMono;
		int32 VocalRate = 0;
		FString WavUsed, LoadErr;
		if (LoadAudioMono(VocalsPath, VocalMono, VocalRate, WavUsed, LoadErr) && VocalMono.Num() > 0)
		{
			// Resample-ish: if rates match, blend sample-wise; else use vocals only.
			if (VocalRate == OriginalRate && VocalMono.Num() > 0)
			{
				const int32 N = FMath::Min(Original.Num(), VocalMono.Num());
				InOutMono.SetNum(N);
				for (int32 i = 0; i < N; ++i)
				{
					InOutMono[i] = FMath::Lerp(Original[i], VocalMono[i], W);
				}
				InOutSampleRate = OriginalRate;
			}
			else
			{
				// Prefer AI vocals when rates differ (blend against silence on original side is wrong).
				InOutMono = MoveTemp(VocalMono);
				InOutSampleRate = VocalRate;
				if (W < 0.99f)
				{
					// Soften toward zero instead of the mix when rates mismatch.
					for (float& S : InOutMono) { S *= W; }
				}
			}
			OutMethod = TEXT("demucs");
			if (!OutNote.Contains(TEXT("cache")))
			{
				OutNote = TEXT("Demucs AI vocals");
			}
			return;
		}
		AiError = LoadErr.IsEmpty() ? TEXT("Could not load Demucs vocals.wav") : LoadErr;
	}

	// --- 2) DSP fallback ---
	TArray<float> Dsp = Original;
	IsolateVoiceDsp(Dsp, OriginalRate);
	const int32 N = FMath::Min(Original.Num(), Dsp.Num());
	InOutMono.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		InOutMono[i] = FMath::Lerp(Original[i], Dsp[i], W);
	}
	InOutSampleRate = OriginalRate;
	OutMethod = TEXT("dsp");
	OutNote = AiError.IsEmpty()
		? TEXT("Fast DSP isolate (install Demucs for AI stems: pip install demucs)")
		: FString::Printf(TEXT("DSP fallback — %s"), *AiError);
}

void FCineLipsync::IsolateVoice(TArray<float>& InOutMono, int32 SampleRate)
{
	IsolateVoiceDsp(InOutMono, SampleRate);
}

void FCineLipsync::IsolateVoiceDsp(TArray<float>& InOutMono, int32 SampleRate)
{
	if (InOutMono.Num() < 32 || SampleRate <= 0)
	{
		return;
	}

	const int32 N = InOutMono.Num();

	// Vocal bandpass: 2x HP @ 110 Hz, LP @ 4200 Hz — enough to drop kick/sub
	// without gutting clean acapella body.
	const float HpCutoff = 110.0f;
	const float LpCutoff = 4200.0f;
	const float HpRC = 1.0f / (2.0f * PI * HpCutoff);
	const float LpRC = 1.0f / (2.0f * PI * LpCutoff);
	const float Dt = 1.0f / (float)SampleRate;
	const float HpA = HpRC / (HpRC + Dt);
	const float LpA = Dt / (LpRC + Dt);

	TArray<float> Filtered;
	Filtered.SetNumUninitialized(N);
	{
		float PrevIn = InOutMono[0];
		float Hp = 0.0f, Hp2 = 0.0f, PrevHp = 0.0f, Lp = 0.0f;
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

	const int32 Hop = FMath::Max(1, SampleRate / 100); // 10 ms
	const int32 NumFrames = FMath::Max(1, N / Hop);
	TArray<float> FrameRms, FormantRatio;
	FrameRms.SetNumZeroed(NumFrames);
	FormantRatio.SetNumZeroed(NumFrames);
	float Peak = 1e-6f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		const int32 Start = f * Hop;
		const int32 Count = FMath::Min(Hop * 2, N - Start);
		const float* S = Filtered.GetData() + Start;
		double SumSq = 0.0;
		for (int32 i = 0; i < Count; ++i)
		{
			SumSq += S[i] * S[i];
		}
		const float Rms = FMath::Sqrt((float)(SumSq / FMath::Max(1, Count)));
		FrameRms[f] = Rms;
		Peak = FMath::Max(Peak, Rms);

		const float Low = BandPower(S, Count, SampleRate, 200.0f);
		const float Form =
			BandPower(S, Count, SampleRate, 600.0f) +
			BandPower(S, Count, SampleRate, 1200.0f) +
			BandPower(S, Count, SampleRate, 2200.0f);
		const float High = BandPower(S, Count, SampleRate, 5000.0f);
		FormantRatio[f] = Form / (Low + Form + High + 1e-9f);
	}

	TArray<float> Sorted = FrameRms;
	Sorted.Sort();
	const float Floor = Sorted[FMath::Clamp(Sorted.Num() / 7, 0, Sorted.Num() - 1)];
	// Slightly stricter than the original, still open enough for clean vocals.
	const float GateOpen = FMath::Max(Peak * 0.075f, Floor * 2.5f);
	const float GateClose = GateOpen * 0.42f;

	// Syllable-rate modulation
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
	float PeakMod = 1e-6f;
	float PrevM = 0.0f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		const float D = FMath::Abs(Env[FMath::Min(NumFrames - 1, f + 1)] - Env[FMath::Max(0, f - 1)]);
		PrevM = PrevM * 0.6f + D * 0.4f;
		Modulation[f] = PrevM;
		PeakMod = FMath::Max(PeakMod, PrevM);
	}
	for (float& M : Modulation) { M /= PeakMod; }

	// Local bed (continuous instruments under the vocal)
	TArray<float> Bed;
	Bed.SetNum(NumFrames);
	{
		const int32 BedHalf = FMath::Max(1, SampleRate / Hop / 3);
		for (int32 f = 0; f < NumFrames; ++f)
		{
			const int32 A = FMath::Max(0, f - BedHalf);
			const int32 B = FMath::Min(NumFrames - 1, f + BedHalf);
			float MinE = FrameRms[f];
			for (int32 i = A; i <= B; i += 2)
			{
				MinE = FMath::Min(MinE, FrameRms[i]);
			}
			Bed[f] = MinE;
		}
	}

	TArray<float> Gain;
	Gain.SetNum(NumFrames);
	float PrevGain = 0.0f;
	for (int32 f = 0; f < NumFrames; ++f)
	{
		float G = 0.0f;
		if (FrameRms[f] >= GateOpen)
		{
			G = 1.0f;
		}
		else if (FrameRms[f] > GateClose)
		{
			G = (FrameRms[f] - GateClose) / FMath::Max(1e-6f, GateOpen - GateClose);
		}

		// Soft bed gate — leave a healthy floor so quiet phrases still pass.
		const float AboveBed = FMath::Max(0.0f, FrameRms[f] - Bed[f] * 1.15f);
		const float BedGate = FMath::Clamp(AboveBed / FMath::Max(GateOpen * 0.4f, 1e-6f), 0.35f, 1.0f);
		G = FMath::Min(G, BedGate);

		// Steady beds quieter; floor high enough that clean speech never goes silent.
		const float ModBoost = 0.35f + 0.65f * FMath::Clamp(Modulation[f] * 1.5f, 0.0f, 1.0f);
		G *= ModBoost;

		// Mild formant preference (floor 0.55 — never crush acapella).
		const float FormBoost = FMath::Clamp((FormantRatio[f] - 0.22f) / 0.45f, 0.55f, 1.0f);
		G *= FormBoost;

		const float Alpha = G > PrevGain ? 0.55f : 0.22f;
		PrevGain = PrevGain + (G - PrevGain) * Alpha;
		Gain[f] = FMath::Clamp(PrevGain, 0.0f, 1.0f);
	}

	float OutPeak = 1e-6f;
	for (int32 i = 0; i < N; ++i)
	{
		const float T = (float)i / (float)Hop;
		const int32 F0 = FMath::Clamp((int32)T, 0, NumFrames - 1);
		const int32 F1 = FMath::Min(F0 + 1, NumFrames - 1);
		const float Frac = FMath::Clamp(T - F0, 0.0f, 1.0f);
		const float G = FMath::Lerp(Gain[F0], Gain[F1], Frac);
		const float BedG = FMath::Lerp(Bed[F0], Bed[F1], Frac);
		// Cap bed attenuation so we only dip residual music, not the vocal.
		const float BedAtten = 1.0f - FMath::Clamp(BedG / FMath::Max(Peak, 1e-6f), 0.0f, 0.30f);
		const float S = Filtered[i] * G * BedAtten;
		InOutMono[i] = S;
		OutPeak = FMath::Max(OutPeak, FMath::Abs(S));
	}

	const float TargetPeak = 0.75f;
	if (OutPeak > 1e-5f)
	{
		const float Scale = TargetPeak / OutPeak;
		for (float& S : InOutMono)
		{
			S = FMath::Clamp(S * Scale, -1.0f, 1.0f);
		}
	}

	UE_LOG(LogCineDirectorLipsync, Log,
		TEXT("IsolateVoiceDsp: %d samples @ %d Hz, floor=%.5f gate=%.5f peakMod=%.5f"),
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

	TArray<float> Energy, LowRatio, MidRatio, HighRatio, Sibilance;
	Energy.SetNumZeroed(NumFrames);
	LowRatio.SetNumZeroed(NumFrames);
	MidRatio.SetNumZeroed(NumFrames);
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

		// Four bands so A/I/U/O-style shapes pull apart more clearly.
		const float Low = BandPower(S, Count, SampleRate, 250.0f) + BandPower(S, Count, SampleRate, 450.0f);
		const float Mid = BandPower(S, Count, SampleRate, 900.0f) + BandPower(S, Count, SampleRate, 1400.0f);
		const float High = BandPower(S, Count, SampleRate, 2200.0f) + BandPower(S, Count, SampleRate, 3200.0f);
		const float Air = BandPower(S, Count, SampleRate, 5000.0f) + BandPower(S, Count, SampleRate, 7000.0f);
		const float Total = Low + Mid + High + Air + 1e-9f;
		LowRatio[i] = Low / Total;
		MidRatio[i] = Mid / Total;
		HighRatio[i] = (High + Air * 0.5f) / Total;
		Sibilance[i] = Air / (Low + Mid + 1e-9f);
	}

	// ~95th percentile normalizer so a few peaks don't squash the whole take.
	TArray<float> SortedEnergy = Energy;
	SortedEnergy.Sort();
	const float NoiseFloor = SortedEnergy[FMath::Clamp(SortedEnergy.Num() / 5, 0, SortedEnergy.Num() - 1)];
	const float NormPeak = FMath::Max(
		SortedEnergy[FMath::Clamp((SortedEnergy.Num() * 95) / 100, 0, SortedEnergy.Num() - 1)],
		PeakRms * 0.55f);
	const float SpeechGate = FMath::Max(NormPeak * 0.07f, NoiseFloor * 2.5f);

	for (int32 i = 0; i < NumFrames; ++i)
	{
		const float ERaw = FMath::Clamp(Energy[i] / NormPeak, 0.0f, 1.35f);
		const float E = FMath::Clamp(FMath::Pow(ERaw, 0.55f) * 1.18f, 0.0f, 1.0f);
		const float Gate = Energy[i] / FMath::Max(SpeechGate, 1e-6f);
		FCineVisemeFrame& F = Frames[i];

		// Below speech floor: silence (close applied in the final pass).
		if (Gate < 0.45f)
		{
			continue;
		}

		// Open amount from energy; marginal frames still get full shape analysis
		// so A/I/U/O aren't reduced to pure jaw open/close.
		const float OpenAmt = (Gate < 1.0f)
			? E * FMath::Clamp(Gate, 0.0f, 1.0f) * 0.9f
			: E;

		const float L = LowRatio[i];
		const float M = MidRatio[i];
		const float H = HighRatio[i];

		// Competitive vowel scores (formant-ish band cues).
		// A (ah): open mid, not too bright/dark.
		// I/E (ee/eh): high-band energy.
		// U (oo): low-band, dark.
		// O (oh): mid-low round.
		float ScoreA = FMath::Clamp(M * 2.05f + L * 0.5f - H * 0.9f + 0.10f, 0.0f, 1.0f);
		float ScoreI = FMath::Clamp(H * 2.75f - L * 1.05f - M * 0.12f - 0.10f, 0.0f, 1.0f);
		float ScoreU = FMath::Clamp(L * 2.95f - H * 1.4f - M * 0.28f - 0.16f, 0.0f, 1.0f);
		float ScoreO = FMath::Clamp(M * 1.55f + L * 1.2f - H * 1.15f - 0.06f, 0.0f, 1.0f);

		if (Sibilance[i] > 1.15f && E > 0.08f)
		{
			F.Sibilant = FMath::Clamp((Sibilance[i] - 1.15f) * 0.85f, 0.0f, 1.0f);
			// S/SH: teeth + slight width, not a big open A.
			ScoreI = FMath::Max(ScoreI, F.Sibilant);
			ScoreA *= 0.28f;
			ScoreU *= 0.25f;
			ScoreO *= 0.28f;
		}

		// Softmax so one vowel leads without always defaulting to Jaw=energy.
		constexpr float Sharp = 2.6f;
		float WA = FMath::Pow(FMath::Max(ScoreA, 1e-4f), Sharp);
		float WI = FMath::Pow(FMath::Max(ScoreI, 1e-4f), Sharp);
		float WU = FMath::Pow(FMath::Max(ScoreU, 1e-4f), Sharp);
		float WO = FMath::Pow(FMath::Max(ScoreO, 1e-4f), Sharp);
		const float WSum = WA + WI + WU + WO;

		// Amplitude lives on the shape channels (VRM A/I/U/O are exclusive morphs).
		const float ShapeAmt = FMath::Clamp(OpenAmt * 1.28f, 0.0f, 1.0f);
		F.Jaw = ShapeAmt * (WA / WSum);
		F.Wide = ShapeAmt * (WI / WSum);
		F.Pucker = ShapeAmt * (WU / WSum);
		F.Funnel = ShapeAmt * (WO / WSum);

		// Weak spectral contrast: still seed all four so exclusive mode isn't
		// pure A open/close — lightly cycle toward O/I/U with energy.
		const float MaxScore = FMath::Max(ScoreA, FMath::Max3(ScoreI, ScoreU, ScoreO));
		if (MaxScore < 0.20f && OpenAmt > 0.16f)
		{
			// Pseudo-syllable phase from frame index so weak frames still vary.
			const float Phase = FMath::Fmod((float)i * 0.37f, 4.0f);
			if (Phase < 1.0f)
			{
				F.Jaw = FMath::Max(F.Jaw, OpenAmt * 0.85f);
			}
			else if (Phase < 2.0f)
			{
				F.Wide = FMath::Max(F.Wide, OpenAmt * 0.8f);
				F.Jaw = FMath::Max(F.Jaw, OpenAmt * 0.2f);
			}
			else if (Phase < 3.0f)
			{
				F.Pucker = FMath::Max(F.Pucker, OpenAmt * 0.85f);
			}
			else
			{
				F.Funnel = FMath::Max(F.Funnel, OpenAmt * 0.85f);
			}
		}

		if (i > 1 && i + 2 < NumFrames)
		{
			const float Around = (Energy[i - 2] + Energy[i - 1] + Energy[i + 1] + Energy[i + 2]) / (4.0f * NormPeak);
			if (Around > 0.11f && ERaw < Around * 0.42f)
			{
				F.Close = 1.0f;
				F.Jaw = 0.0f;
				F.Wide = 0.0f;
				F.Pucker = 0.0f;
				F.Funnel = 0.0f;
				F.Sibilant = 0.0f;
			}
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
	SmoothChannel(&FCineVisemeFrame::Jaw, 0.7f, 0.58f);
	SmoothChannel(&FCineVisemeFrame::Wide, 0.55f, 0.5f);
	SmoothChannel(&FCineVisemeFrame::Pucker, 0.55f, 0.5f);
	SmoothChannel(&FCineVisemeFrame::Funnel, 0.55f, 0.5f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.9f, 0.68f);
	SmoothChannel(&FCineVisemeFrame::Sibilant, 0.55f, 0.45f);

	// Mild expand so mid syllables read bigger.
	for (int32 i = 0; i < NumFrames; ++i)
	{
		auto Punch = [](float V) { return FMath::Clamp(FMath::Pow(V, 0.72f) * 1.12f, 0.0f, 1.0f); };
		Frames[i].Jaw = Punch(Frames[i].Jaw);
		Frames[i].Wide = Punch(Frames[i].Wide);
		Frames[i].Pucker = Punch(Frames[i].Pucker);
		Frames[i].Funnel = Punch(Frames[i].Funnel);
	}

	for (int32 i = 0; i < NumFrames; ++i)
	{
		const float Gate = Energy[i] / FMath::Max(SpeechGate, 1e-6f);
		if (Gate < 0.42f)
		{
			const float Scale = Gate < 0.18f ? 0.0f : FMath::Clamp(Gate / 0.42f, 0.0f, 1.0f);
			Frames[i].Jaw *= Scale;
			Frames[i].Wide *= Scale;
			Frames[i].Pucker *= Scale;
			Frames[i].Funnel *= Scale;
			Frames[i].Sibilant *= Scale;
			if (Gate < 0.18f)
			{
				Frames[i].Close = FMath::Max(Frames[i].Close, 0.85f);
			}
		}
	}

	// Winner counts so we can see A/I/U/O balance in the Output Log.
	int32 WinA = 0, WinI = 0, WinU = 0, WinO = 0, WinClose = 0, WinRest = 0;
	float PeakA = 0, PeakI = 0, PeakU = 0, PeakO = 0;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const FCineVisemeFrame& F = Frames[i];
		PeakA = FMath::Max(PeakA, F.Jaw);
		PeakI = FMath::Max(PeakI, F.Wide);
		PeakU = FMath::Max(PeakU, F.Pucker);
		PeakO = FMath::Max(PeakO, F.Funnel);
		if (F.Close > 0.5f) { ++WinClose; continue; }
		const float Vals[4] = { F.Jaw, F.Wide, F.Pucker, F.Funnel };
		int32 W = 0;
		float Best = Vals[0];
		for (int32 k = 1; k < 4; ++k)
		{
			if (Vals[k] > Best) { Best = Vals[k]; W = k; }
		}
		if (Best < 0.06f) { ++WinRest; }
		else if (W == 0) { ++WinA; }
		else if (W == 1) { ++WinI; }
		else if (W == 2) { ++WinU; }
		else { ++WinO; }
	}
	UE_LOG(LogCineDirectorLipsync, Log,
		TEXT("Analyzed %.1fs → %d frames (gate=%.4f). winners A/I/U/O/close/rest=%d/%d/%d/%d/%d/%d peaks A=%.2f I=%.2f U=%.2f O=%.2f"),
		(float)Mono.Num() / SampleRate, NumFrames, SpeechGate,
		WinA, WinI, WinU, WinO, WinClose, WinRest, PeakA, PeakI, PeakU, PeakO);
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
		const float Peak = Rand.FRandRange(0.5f, 0.98f);
		// One exclusive vowel per syllable: 0=A, 1=I, 2=U, 3=O
		const int32 Shape = Rand.RandRange(0, 3);

		const int32 Start = FMath::RoundToInt32(Time * Fps);
		const int32 Len = FMath::Max(2, FMath::RoundToInt32(SylLen * Fps));
		for (int32 i = 0; i < Len && Start + i < NumFrames; ++i)
		{
			const float T = (float)i / Len;
			// Sin envelope returns to zero at both ends → full close between syllables.
			const float Env = FMath::Sin(T * PI) * Peak;
			FCineVisemeFrame& F = Frames[Start + i];
			switch (Shape)
			{
			case 1: F.Wide = FMath::Max(F.Wide, Env); break;
			case 2: F.Pucker = FMath::Max(F.Pucker, Env); break;
			case 3: F.Funnel = FMath::Max(F.Funnel, Env); break;
			default: F.Jaw = FMath::Max(F.Jaw, Env); break;
			}
		}

		// Brief closure at the end of every syllable (not just pauses).
		const int32 CloseAt = Start + Len;
		if (CloseAt < NumFrames)
		{
			Frames[CloseAt].Close = 1.0f;
			Frames[CloseAt].Jaw = 0.0f;
			Frames[CloseAt].Wide = 0.0f;
			Frames[CloseAt].Pucker = 0.0f;
			Frames[CloseAt].Funnel = 0.0f;
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
	SmoothChannel(&FCineVisemeFrame::Funnel, 0.50f, 0.55f);
	SmoothChannel(&FCineVisemeFrame::Close, 0.90f, 0.70f);
	return Frames;
}


FString FCineLipsync::EstimateEmotionFromAudio(const TArray<float>& Mono, int32 SampleRate)
{
	// Simple, reliable detector: absolute energy/brightness/dynamics per ~2s chunk.
	// Prefer real expressions over "calm" whenever speech energy is present.
	if (Mono.Num() < SampleRate / 8 || SampleRate <= 0)
	{
		return TEXT("happy");
	}

	const int32 Fps = 20;
	const int32 Hop = FMath::Max(1, SampleRate / Fps);
	const int32 Window = Hop * 2;
	const int32 NumFrames = FMath::Max(1, Mono.Num() / Hop);

	TArray<float> Energy, Bright;
	Energy.SetNumZeroed(NumFrames);
	Bright.SetNumZeroed(NumFrames);
	float PeakRms = 1e-6f;
	for (int32 i = 0; i < NumFrames; ++i)
	{
		const int32 Start = FMath::Clamp(i * Hop - Hop / 2, 0, Mono.Num() - 1);
		const int32 Count = FMath::Min(Window, Mono.Num() - Start);
		const float* S = Mono.GetData() + Start;
		float SumSq = 0.0f;
		for (int32 j = 0; j < Count; ++j) { SumSq += S[j] * S[j]; }
		const float Rms = FMath::Sqrt(SumSq / FMath::Max(1, Count));
		Energy[i] = Rms;
		PeakRms = FMath::Max(PeakRms, Rms);
		const float Low = BandPower(S, Count, SampleRate, 250.0f) + BandPower(S, Count, SampleRate, 500.0f);
		const float Mid = BandPower(S, Count, SampleRate, 1500.0f) + BandPower(S, Count, SampleRate, 2500.0f);
		const float High = BandPower(S, Count, SampleRate, 4500.0f) + BandPower(S, Count, SampleRate, 6500.0f);
		Bright[i] = (Mid + High) / (Low + Mid + High + 1e-9f);
	}

	TArray<float> Sorted = Energy;
	Sorted.Sort();
	const float NoiseFloor = Sorted[FMath::Clamp(Sorted.Num() / 5, 0, Sorted.Num() - 1)];
	const float SpeechGate = FMath::Max(PeakRms * 0.06f, NoiseFloor * 2.0f);

	const float DurationSec = (float)Mono.Num() / SampleRate;
	const int32 NumSegs = FMath::Clamp(FMath::CeilToInt(DurationSec / 2.0f), 1, 8);
	const int32 FramesPerSeg = FMath::Max(1, NumFrames / NumSegs);

	TArray<FString> Labels;
	for (int32 Seg = 0; Seg < NumSegs; ++Seg)
	{
		const int32 A = Seg * FramesPerSeg;
		const int32 B = (Seg == NumSegs - 1) ? NumFrames : FMath::Min(NumFrames, (Seg + 1) * FramesPerSeg);
		double SumE = 0.0, SumE2 = 0.0, SumBright = 0.0;
		int32 Speech = 0;
		int32 Attacks = 0;
		float Prev = 0.0f;
		for (int32 i = A; i < B; ++i)
		{
			if (Energy[i] < SpeechGate) { Prev = Energy[i] / PeakRms; continue; }
			const float En = Energy[i] / PeakRms;
			SumE += En;
			SumE2 += En * En;
			SumBright += Bright[i];
			if (En > Prev + 0.15f) { ++Attacks; }
			Prev = En;
			++Speech;
		}
		if (Speech < 2)
		{
			// Keep prior emotion across short silences instead of inserting calm.
			// Copy first — Add(Labels.Last()) is illegal (ref into container being modified).
			if (Labels.Num() > 0)
			{
				const FString Carry = Labels.Last();
				Labels.Add(Carry);
			}
			else
			{
				Labels.Add(TEXT("happy"));
			}
			continue;
		}

		const float MeanE = (float)(SumE / Speech);
		const float VarE = FMath::Max(0.0f, (float)(SumE2 / Speech) - MeanE * MeanE);
		const float Dyn = FMath::Sqrt(VarE);
		const float MeanBright = (float)(SumBright / Speech);
		const float PauseRatio = 1.0f - (float)Speech / (float)FMath::Max(1, B - A);
		const float AttackRate = (float)Attacks / FMath::Max(0.5f, (B - A) / (float)Fps);
		const float Arousal = MeanE * 0.55f + Dyn * 1.3f + AttackRate * 0.12f;

		FString Name = TEXT("happy");
		if (Arousal > 0.55f && MeanBright < 0.48f && Dyn > 0.10f)
		{
			Name = TEXT("angry");
		}
		else if (Arousal > 0.5f && MeanBright > 0.58f && AttackRate > 0.8f)
		{
			Name = TEXT("surprised");
		}
		else if (Arousal > 0.48f && MeanBright > 0.55f && PauseRatio < 0.4f)
		{
			Name = TEXT("scared");
		}
		else if (PauseRatio > 0.55f && MeanE < 0.35f)
		{
			Name = TEXT("sad");
		}
		else if (Arousal > 0.35f && MeanBright > 0.48f)
		{
			Name = TEXT("happy");
		}
		else if (Arousal > 0.42f && MeanBright < 0.45f)
		{
			Name = TEXT("suspicious");
		}
		else if (MeanE < 0.22f && PauseRatio > 0.45f)
		{
			Name = TEXT("sad");
		}
		else
		{
			// Default speaking engagement — never blank.
			Name = TEXT("happy");
		}

		// Avoid immediate identical spam: if same as last, try a mild alternate.
		if (Labels.Num() > 0 && Labels.Last().Equals(Name, ESearchCase::IgnoreCase) && NumSegs >= 3)
		{
			if (Name.Equals(TEXT("happy")) && MeanBright < 0.5f) { Name = TEXT("angry"); }
			else if (Name.Equals(TEXT("angry")) && MeanE < 0.4f) { Name = TEXT("sad"); }
			else if (Name.Equals(TEXT("sad")) && MeanE > 0.4f) { Name = TEXT("happy"); }
		}
		Labels.Add(Name);
	}

	// Collapse consecutive duplicates.
	TArray<FString> Collapsed;
	for (const FString& L : Labels)
	{
		if (Collapsed.Num() > 0 && Collapsed.Last().Equals(L, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Collapsed.Add(L);
	}
	if (Collapsed.Num() == 0)
	{
		return TEXT("happy");
	}
	// Cap arc length.
	while (Collapsed.Num() > 5)
	{
		Collapsed.RemoveAt(Collapsed.Num() / 2);
	}

	const FString Result = FString::Join(Collapsed, TEXT(" then "));
	UE_LOG(LogCineDirectorLipsync, Log, TEXT("Audio emotion estimate: %s (%.1fs, %d segs)"),
		*Result, DurationSec, NumSegs);
	return Result;
}