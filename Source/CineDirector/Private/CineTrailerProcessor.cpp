// Copyright Roundtree. All Rights Reserved.

#include "CineTrailerProcessor.h"

#include "Async/Async.h"
#include "CineRenderLauncher.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorTrailer, Log, All);

namespace CineTrailer
{
	bool ContainsAnyOf(const FString& Text, std::initializer_list<const TCHAR*> Phrases)
	{
		for (const TCHAR* Phrase : Phrases)
		{
			if (Text.Contains(Phrase))
			{
				return true;
			}
		}
		return false;
	}

	bool RunTool(const FString& Exe, const FString& Args, FString& OutError)
	{
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		const bool bLaunched = FPlatformProcess::ExecProcess(*Exe, *Args, &ReturnCode, &StdOut, &StdErr);
		if (!bLaunched || ReturnCode != 0)
		{
			OutError = StdErr.Right(500);
			UE_LOG(LogCineDirectorTrailer, Warning, TEXT("ffmpeg failed (code %d): %s\nargs: %s"), ReturnCode, *OutError, *Args);
			return false;
		}
		return true;
	}

	/** Strip characters that would break drawtext, and uppercase. */
	FString SanitizeCardText(FString In)
	{
		In.ReplaceInline(TEXT("'"), TEXT(""));
		In.ReplaceInline(TEXT("\""), TEXT(""));
		In.ReplaceInline(TEXT(":"), TEXT(""));
		In.ReplaceInline(TEXT("\\"), TEXT(""));
		In.ReplaceInline(TEXT(";"), TEXT(""));
		In.ReplaceInline(TEXT("%"), TEXT(""));
		return In.ToUpper().TrimStartAndEnd();
	}

	/** "THE VISITORS" -> "T H E   V I S I T O R S" (trailer letterspacing). */
	FString Letterspace(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (C == TEXT(' '))
			{
				Out += TEXT("  ");
			}
			else
			{
				Out.AppendChar(C);
				if (i + 1 < In.Len())
				{
					Out.AppendChar(TEXT(' '));
				}
			}
		}
		return Out;
	}

	bool MakeCard(const FString& FFmpeg, const FCineTrailerStyle& Style, const FString& SpacedText, int32 FontSize,
		double Duration, double FadeInDur, double FadeOutStart, double FadeOutDur, const FString& OutFile, FString& OutError,
		bool bWithSilentAudio)
	{
		const TCHAR* Font = Style.bBoldTitleFont
			? TEXT("fontfile='C\\:/Windows/Fonts/arialbd.ttf'")
			: TEXT("fontfile='C\\:/Windows/Fonts/constan.ttf'");
		const FString Filter = FString::Printf(
			TEXT("drawtext=%s:text='%s':fontcolor=0xC8C8CC:fontsize=%d:x=(w-text_w)/2:y=(h-text_h)/2,")
			TEXT("noise=alls=7:allf=t,fade=t=in:st=0:d=%.2f,fade=t=out:st=%.2f:d=%.2f"),
			Font, *SpacedText, FontSize, FadeInDur, FadeOutStart, FadeOutDur);

		// When the trailer keeps A/V streams for concat, cards need silent audio
		// so every segment has matching stream layout.
		FString Args;
		if (bWithSilentAudio)
		{
			Args = FString::Printf(
				TEXT("-y -f lavfi -i \"color=c=black:s=1920x1080:d=%.2f:r=24\" ")
				TEXT("-f lavfi -i \"anullsrc=channel_layout=stereo:sample_rate=48000\" -t %.2f ")
				TEXT("-vf \"%s\" -c:v libx264 -crf 18 -pix_fmt yuv420p -c:a aac -ar 48000 -ac 2 -shortest \"%s\""),
				Duration, Duration, *Filter, *OutFile);
		}
		else
		{
			Args = FString::Printf(
				TEXT("-y -f lavfi -i \"color=c=black:s=1920x1080:d=%.2f:r=24\" -vf \"%s\" -an -c:v libx264 -crf 18 -pix_fmt yuv420p \"%s\""),
				Duration, *Filter, *OutFile);
		}
		return RunTool(FFmpeg, Args, OutError);
	}

	bool MakeBeat(const FString& FFmpeg, const FString& Source, const FCineTrailerStyle& Style,
		double Start, double Duration, const FString& CameraTag, const FString& OutFile, FString& OutError,
		bool bKeepAudio, bool bSourceHasAudio)
	{
		FString Filter;

		// Order: geometry → grade → texture/FX → frame bars → overlays → transitions.
		if (Style.bMirror)
		{
			Filter += TEXT("hflip,");
		}
		if (Style.bDutch)
		{
			// Mild dutch / canted angle, black fill around rotated frame.
			Filter += TEXT("rotate=a=0.12:ow=rotw(0.12):oh=roth(0.12):c=black,scale=1920:1080,");
		}
		if (Style.bZoomPunch)
		{
			// Slow push-in across the beat (zoompan duration in frames @ 24fps).
			const int32 Frames = FMath::Max(2, FMath::RoundToInt32(Duration * 24.0));
			Filter += FString::Printf(
				TEXT("zoompan=z='min(1.0+0.12*on/%d\\,1.12)':d=%d:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':s=1920x1080:fps=24,"),
				Frames, Frames);
		}
		if (Style.bShake)
		{
			const float S = FMath::Clamp(Style.ShakeIntensity, 0.4f, 2.5f);
			const int32 Edge = FMath::RoundToInt32(48.0f * S);
			const int32 EdgeY = FMath::RoundToInt32(28.0f * S);
			const int32 AmpX = FMath::RoundToInt32(12.0f * S);
			const int32 AmpY = FMath::RoundToInt32(7.0f * S);
			Filter += FString::Printf(
				TEXT("crop=w=iw-%d:h=ih-%d:x='%d/2+%d*sin(2.3*t)+%d*sin(7.7*t)':y='%d/2+%d*cos(3.1*t)+%d*sin(9.3*t)',scale=1920:1080,"),
				Edge, EdgeY, Edge, AmpX, FMath::Max(AmpX / 2, 3), EdgeY, AmpY, FMath::Max(AmpY / 2, 2));
		}
		if (Style.bPixelate)
		{
			// Chunky music-video / 8-bit block look.
			Filter += TEXT("scale=160:90:flags=neighbor,scale=1920:1080:flags=neighbor,");
		}
		if (!Style.GradeFilter.IsEmpty())
		{
			Filter += Style.GradeFilter + TEXT(",");
		}
		if (Style.bInvert)
		{
			Filter += TEXT("negate,");
		}
		if (Style.bSoft)
		{
			Filter += TEXT("gblur=sigma=1.6,");
		}
		if (Style.bBloom)
		{
			// Soft glow: mild blur + lift midtones (lightweight stand-in for bloom).
			Filter += TEXT("gblur=sigma=0.8,eq=brightness=0.04:saturation=1.08,");
		}
		if (Style.bSharpen)
		{
			Filter += TEXT("unsharp=5:5:0.8:3:3:0.4,");
		}
		if (Style.bFlicker)
		{
			Filter += TEXT("eq=eval=frame:brightness='0.0+0.035*sin(15*t)+0.025*sin(47*t)',");
		}
		if (Style.bChromaShift || Style.bGlitch)
		{
			const int32 Shift = Style.bGlitch ? 6 : 3;
			Filter += FString::Printf(TEXT("chromashift=cbh=%d:crh=-%d,"), Shift, Shift);
		}
		if (Style.bGlitch)
		{
			// Brief horizontal tear + noise spikes.
			Filter += TEXT("rgbashift=rh=2:bv=-2,noise=alls=12:allf=t,");
		}
		if (Style.GrainLevel > 0)
		{
			Filter += FString::Printf(TEXT("noise=alls=%d:allf=t+u,"), Style.GrainLevel);
		}
		if (Style.bScanlines)
		{
			// Darken every 3rd scan row — reads as CRT / music-video scanlines.
			// Commas inside geq expressions must be escaped for the filtergraph.
			Filter += TEXT("geq=r='r(X\\,Y)*(if(eq(mod(Y\\,3)\\,0)\\,0.52\\,1))':")
				TEXT("g='g(X\\,Y)*(if(eq(mod(Y\\,3)\\,0)\\,0.52\\,1))':")
				TEXT("b='b(X\\,Y)*(if(eq(mod(Y\\,3)\\,0)\\,0.52\\,1))',");
		}
		if (Style.bVignette)
		{
			Filter += TEXT("vignette=PI/4,");
		}
		if (Style.bLetterbox)
		{
			Filter += TEXT("crop=1920:804:0:138,pad=1920:1080:0:138:black,");
		}
		if (Style.bCamOverlays)
		{
			// Timecode continues across beats so the cuts read as one recovered tape.
			const int32 Tc = 3 * 3600 + 41 * 60 + FMath::FloorToInt32(Start);
			const int32 TopY = Style.bLetterbox ? 158 : 34;
			const int32 BottomY = Style.bLetterbox ? 880 : 1020;
			Filter += FString::Printf(
				TEXT("drawtext=fontfile='C\\:/Windows/Fonts/arialbd.ttf':text='● REC':fontcolor=0xE03030:fontsize=34:x=40:y=%d:enable='lt(mod(t,1.2),0.75)',")
				TEXT("drawtext=fontfile='C\\:/Windows/Fonts/consola.ttf':text='%s':fontcolor=0xC8C8C8:fontsize=28:x=w-text_w-40:y=%d,")
				TEXT("drawtext=fontfile='C\\:/Windows/Fonts/consola.ttf':timecode='%02d\\:%02d\\:%02d\\:00':rate=24:fontcolor=0xC8C8C8:fontsize=28:x=40:y=%d,"),
				TopY, *CameraTag, TopY + 2, Tc / 3600, (Tc / 60) % 60, Tc % 60, BottomY);
		}

		const float FadeIn = FMath::Max(Style.BeatFadeIn, 0.0f);
		const float FadeOut = FMath::Max(Style.BeatFadeOut, 0.0f);
		if (FadeIn > 0.001f || FadeOut > 0.001f)
		{
			const float FadeOutStart = FMath::Max((float)Duration - FadeOut, 0.0f);
			if (FadeIn > 0.001f)
			{
				Filter += FString::Printf(TEXT("fade=t=in:st=0:d=%.2f,"), FadeIn);
			}
			if (FadeOut > 0.001f)
			{
				Filter += FString::Printf(TEXT("fade=t=out:st=%.2f:d=%.2f,"), FadeOutStart, FadeOut);
			}
		}
		// Strip trailing comma so -vf never ends with ",".
		while (Filter.EndsWith(TEXT(",")))
		{
			Filter.LeftChopInline(1);
		}
		if (Filter.IsEmpty())
		{
			Filter = TEXT("null");
		}

		FString Args;
		if (bKeepAudio && bSourceHasAudio)
		{
			// Carry the render's audio for this beat, with short fades matching the cut.
			const double FadeOutAt = FMath::Max(Duration - 0.2, 0.05);
			Args = FString::Printf(
				TEXT("-y -ss %.3f -t %.3f -i \"%s\" -vf \"%s\" ")
				TEXT("-af \"afade=t=in:st=0:d=0.12,afade=t=out:st=%.2f:d=0.2\" ")
				TEXT("-c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 -c:a aac -ar 48000 -ac 2 -shortest \"%s\""),
				Start, Duration, *Source, *Filter, FadeOutAt, *OutFile);
		}
		else if (bKeepAudio)
		{
			// Source has no audio stream — pad silence so concat stream layout stays uniform.
			Args = FString::Printf(
				TEXT("-y -ss %.3f -t %.3f -i \"%s\" ")
				TEXT("-f lavfi -i \"anullsrc=channel_layout=stereo:sample_rate=48000\" -t %.3f ")
				TEXT("-vf \"%s\" -map 0:v:0 -map 1:a:0 -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 ")
				TEXT("-c:a aac -ar 48000 -ac 2 -shortest \"%s\""),
				Start, Duration, *Source, Duration, *Filter, *OutFile);
		}
		else
		{
			Args = FString::Printf(
				TEXT("-y -ss %.3f -t %.3f -i \"%s\" -vf \"%s\" -an -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 \"%s\""),
				Start, Duration, *Source, *Filter, *OutFile);
		}
		return RunTool(FFmpeg, Args, OutError);
	}

	/** The original four-note whistle motif with vibrato + echo (~6s with tail). */
	bool MakeMotif(const FString& FFmpeg, const FString& OutFile, FString& OutError)
	{
		const FString Args = FString::Printf(
			TEXT("-y -f lavfi -i \"sine=frequency=659:duration=1.0\" -f lavfi -i \"sine=frequency=784:duration=1.0\" ")
			TEXT("-f lavfi -i \"sine=frequency=740:duration=0.8\" -f lavfi -i \"sine=frequency=587:duration=1.4\" ")
			TEXT("-filter_complex \"")
			TEXT("[0:a]vibrato=f=5.5:d=0.5,afade=t=in:st=0:d=0.06,afade=t=out:st=0.7:d=0.3,adelay=0:all=1[n1];")
			TEXT("[1:a]vibrato=f=5.5:d=0.5,afade=t=in:st=0:d=0.06,afade=t=out:st=0.7:d=0.3,adelay=1100:all=1[n2];")
			TEXT("[2:a]vibrato=f=5.5:d=0.5,afade=t=in:st=0:d=0.06,afade=t=out:st=0.5:d=0.3,adelay=2200:all=1[n3];")
			TEXT("[3:a]vibrato=f=5.5:d=0.5,afade=t=in:st=0:d=0.06,afade=t=out:st=1.1:d=0.3,adelay=3100:all=1[n4];")
			TEXT("[n1][n2][n3][n4]amix=inputs=4:duration=longest:normalize=0,aecho=0.7:0.55:250|500:0.30|0.18,apad=pad_dur=1.5[m]\" ")
			TEXT("-map \"[m]\" -c:a aac \"%s\""),
			*OutFile);
		return RunTool(FFmpeg, Args, OutError);
	}

	bool MakeScore(const FString& FFmpeg, const FString& MotifFile, const FCineTrailerStyle& Style,
		double TotalDuration, const FString& OutFile, FString& OutError)
	{
		// Per-mood bed: two low tones + filtered noise, with different movement.
		double Freq1 = 36.0, Freq2 = 47.0, NoiseLowpass = 240.0;
		double Vol1 = 0.40, Vol2 = 0.28, VolNoise = 0.45, MotifVol = 0.22;
		FString Tone2Extra = TEXT(",tremolo=f=0.4:d=0.8");
		double MotifInterval = 13.0;

		switch (Style.Mood)
		{
		case FCineTrailerStyle::EMood::Tense:
			Freq1 = 40.0; Freq2 = 53.0; NoiseLowpass = 200.0;
			Vol1 = 0.45; Vol2 = 0.18; VolNoise = 0.35;
			Tone2Extra = TEXT(",tremolo=f=2.5:d=0.85"); // heartbeat-ish pulse
			MotifInterval = 9.0;
			break;
		case FCineTrailerStyle::EMood::Somber:
			Freq1 = 32.0; Freq2 = 43.0; NoiseLowpass = 150.0;
			Vol1 = 0.40; Vol2 = 0.15; VolNoise = 0.25;
			Tone2Extra = TEXT("");
			MotifInterval = 16.0;
			MotifVol = 0.16;
			break;
		case FCineTrailerStyle::EMood::Upbeat:
			// Higher, faster bed for music-video edits (no eerie drone).
			Freq1 = 55.0; Freq2 = 82.5; NoiseLowpass = 480.0;
			Vol1 = 0.38; Vol2 = 0.30; VolNoise = 0.22;
			Tone2Extra = TEXT(",tremolo=f=4.0:d=0.7");
			MotifInterval = 7.0;
			MotifVol = 0.12;
			break;
		default:
			break;
		}

		TArray<int32> DelaysMs;
		if (Style.bWhistleMotif)
		{
			for (double S = 4.0; S < TotalDuration - 10.0; S += MotifInterval)
			{
				DelaysMs.Add(FMath::RoundToInt32(S * 1000.0));
			}
		}

		FString Filter = FString::Printf(
			TEXT("[0:a]volume=%.2f[d1];[1:a]volume=%.2f%s[d2];[2:a]lowpass=f=%.0f,volume=%.2f[ns];"),
			Vol1, Vol2, *Tone2Extra, NoiseLowpass, VolNoise);

		if (DelaysMs.Num() > 0)
		{
			Filter += FString::Printf(TEXT("[3:a]volume=%.2f,asplit=%d"), MotifVol, DelaysMs.Num());
			for (int32 i = 0; i < DelaysMs.Num(); ++i)
			{
				Filter += FString::Printf(TEXT("[s%d]"), i);
			}
			Filter += TEXT(";");
			for (int32 i = 0; i < DelaysMs.Num(); ++i)
			{
				Filter += FString::Printf(TEXT("[s%d]adelay=%d:all=1[o%d];"), i, DelaysMs[i], i);
			}
		}

		Filter += TEXT("[d1][d2][ns]");
		for (int32 i = 0; i < DelaysMs.Num(); ++i)
		{
			Filter += FString::Printf(TEXT("[o%d]"), i);
		}
		Filter += FString::Printf(
			TEXT("amix=inputs=%d:duration=first:normalize=0,afade=t=in:st=0:d=2.5,afade=t=out:st=%.2f:d=4[a]"),
			3 + DelaysMs.Num(), FMath::Max(TotalDuration - 4.2, 0.0));

		FString Args = FString::Printf(
			TEXT("-y -f lavfi -i \"sine=frequency=%.0f:duration=%.2f\" -f lavfi -i \"sine=frequency=%.0f:duration=%.2f\" ")
			TEXT("-f lavfi -i \"anoisesrc=color=brown:sample_rate=44100:duration=%.2f\""),
			Freq1, TotalDuration, Freq2, TotalDuration, TotalDuration);
		if (DelaysMs.Num() > 0)
		{
			Args += FString::Printf(TEXT(" -i \"%s\""), *MotifFile);
		}
		Args += FString::Printf(TEXT(" -filter_complex \"%s\" -map \"[a]\" -c:a aac -b:a 192k \"%s\""), *Filter, *OutFile);

		return RunTool(FFmpeg, Args, OutError);
	}

	double ProbeDurationSeconds(const FString& FFprobe, const FString& File)
	{
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(*FFprobe,
			*FString::Printf(TEXT("-v error -show_entries format=duration -of csv=p=0 \"%s\""), *File),
			&ReturnCode, &StdOut, &StdErr);
		return ReturnCode == 0 ? FCString::Atod(*StdOut.TrimStartAndEnd()) : 0.0;
	}

	bool ProbeHasAudio(const FString& FFprobe, const FString& File)
	{
		int32 ReturnCode = -1;
		FString StdOut, StdErr;
		FPlatformProcess::ExecProcess(*FFprobe,
			*FString::Printf(TEXT("-v error -select_streams a -show_entries stream=codec_type -of csv=p=0 \"%s\""), *File),
			&ReturnCode, &StdOut, &StdErr);
		return ReturnCode == 0 && StdOut.Contains(TEXT("audio"));
	}

	/** The blocking pipeline; runs on a worker thread. Returns final path or error text. */
	bool Process(const FCineTrailerOptions& Options, FString& OutResult)
	{
		const FString FFmpeg = FCineRenderLauncher::FindFFmpegExecutable();
		if (FFmpeg.IsEmpty())
		{
			OutResult = TEXT("ffmpeg not found — install it with: winget install Gyan.FFmpeg");
			return false;
		}
		FString FFprobe = FFmpeg;
		FFprobe.ReplaceInline(TEXT("ffmpeg.exe"), TEXT("ffprobe.exe"), ESearchCase::IgnoreCase);

		if (!FPaths::FileExists(Options.SourceVideo))
		{
			OutResult = FString::Printf(TEXT("Rendered movie not found: %s"), *Options.SourceVideo);
			return false;
		}

		const FCineTrailerStyle Style = FCineTrailerProcessor::ParseStyle(Options.StyleDescription);
		const bool bKeepSourceAudio = Options.bKeepSourceAudio;
		const bool bAddScore = Options.bAddSyntheticScore;
		// Segments need a uniform A/V layout whenever we will mux any audio at the end.
		const bool bNeedAudioOnSegments = bKeepSourceAudio || bAddScore;

		UE_LOG(LogCineDirectorTrailer, Log,
			TEXT("Trailer style desc: \"%s\" → %s | audio: keepSource=%d syntheticScore=%d"),
			*Options.StyleDescription, *Style.Summary, bKeepSourceAudio ? 1 : 0, bAddScore ? 1 : 0);

		const double SourceDuration = ProbeDurationSeconds(FFprobe, Options.SourceVideo);
		if (SourceDuration < 8.0)
		{
			OutResult = FString::Printf(TEXT("Source is too short to cut a trailer (%.1fs; need at least 8s)."), SourceDuration);
			return false;
		}

		const bool bSourceHasAudio = ProbeHasAudio(FFprobe, Options.SourceVideo);
		if (bKeepSourceAudio && !bSourceHasAudio)
		{
			UE_LOG(LogCineDirectorTrailer, Warning,
				TEXT("Keep source audio is on, but the render MP4 has no audio stream — beats will be silent."));
		}

		const FString Temp = FPaths::ProjectSavedDir() / TEXT("CineDirectorTrailer");
		IFileManager::Get().MakeDirectory(*Temp, true);

		// ---- Beats -----------------------------------------------------------------
		// Segment audio layout:
		//  - keep source → copy (or silence-pad if render has no audio)
		//  - score only  → silence track so concat + score mux share a layout
		//  - silent      → no audio stream (-an)
		const bool bBeatsCarryAudio = bNeedAudioOnSegments;
		const bool bBeatsUseSourceAudio = bKeepSourceAudio && bSourceHasAudio;

		const int32 NumBeats = Style.BeatFractions.Num();
		FString Error;
		double Cursor = 0.0;
		for (int32 i = 0; i < NumBeats; ++i)
		{
			const double Duration = SourceDuration * Style.BeatFractions[i];
			if (!MakeBeat(FFmpeg, Options.SourceVideo, Style, Cursor, Duration, Options.CameraTag,
				Temp / FString::Printf(TEXT("beat%d.mp4"), i + 1), Error,
				/*bKeepAudio*/ bBeatsCarryAudio,
				/*bSourceHasAudio*/ bBeatsUseSourceAudio))
			{
				OutResult = TEXT("Cutting a beat failed: ") + Error;
				return false;
			}
			Cursor += Duration;
		}

		// ---- Cards ------------------------------------------------------------------
		TArray<FString> Cards = Options.CardLines;
		Cards.RemoveAll([](const FString& S) { return S.TrimStartAndEnd().IsEmpty(); });
		while (Cards.Num() < 3)
		{
			static const TCHAR* Defaults[] = {
				TEXT("They told us we would be safe here"),
				TEXT("But something came in with us"),
				TEXT("It was already inside") };
			Cards.Add(Defaults[Cards.Num()]);
		}
		Cards.SetNum(3);

		for (int32 i = 0; i < 3; ++i)
		{
			const FString Spaced = Letterspace(SanitizeCardText(Cards[i]));
			if (!MakeCard(FFmpeg, Style, Spaced, 46, 2.4, 0.5, 1.9, 0.5,
				Temp / FString::Printf(TEXT("tcard%d.mp4"), i + 1), Error, bNeedAudioOnSegments))
			{
				OutResult = TEXT("Rendering a title card failed: ") + Error;
				return false;
			}
		}

		const FString TitleClean = SanitizeCardText(Options.MovieTitle.IsEmpty() ? TEXT("UNTITLED") : Options.MovieTitle);
		if (!MakeCard(FFmpeg, Style, Letterspace(TitleClean), 130, 4.0, 1.2, 3.2, 0.8, Temp / TEXT("ttitle.mp4"), Error, bNeedAudioOnSegments) ||
			!MakeCard(FFmpeg, Style, Letterspace(TEXT("COMING SOON")), 44, 3.0, 0.6, 2.2, 0.8, Temp / TEXT("tcoming.mp4"), Error, bNeedAudioOnSegments))
		{
			OutResult = TEXT("Rendering the title failed: ") + Error;
			return false;
		}

		// ---- Assemble: card 1 after beat 1, card 2 after beat 2, card 3 before the last beat.
		FString List;
		for (int32 i = 0; i < NumBeats; ++i)
		{
			List += FString::Printf(TEXT("file 'beat%d.mp4'\n"), i + 1);
			if (i == 0)
			{
				List += TEXT("file 'tcard1.mp4'\n");
			}
			else if (i == 1)
			{
				List += TEXT("file 'tcard2.mp4'\n");
			}
			else if (i == NumBeats - 2)
			{
				List += TEXT("file 'tcard3.mp4'\n");
			}
		}
		List += TEXT("file 'ttitle.mp4'\nfile 'tcoming.mp4'\n");

		const FString ListFile = Temp / TEXT("tlist.txt");
		FFileHelper::SaveStringToFile(List, *ListFile, FFileHelper::EEncodingOptions::ForceAnsi);

		const FString VideoCut = Temp / TEXT("tvideo.mp4");
		// When segments carry audio, re-encode both streams so concat is clean.
		const FString ConcatArgs = bNeedAudioOnSegments
			? FString::Printf(
				TEXT("-y -f concat -safe 0 -i \"%s\" -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 -c:a aac -ar 48000 -ac 2 \"%s\""),
				*ListFile, *VideoCut)
			: FString::Printf(
				TEXT("-y -f concat -safe 0 -i \"%s\" -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 -an \"%s\""),
				*ListFile, *VideoCut);
		if (!RunTool(FFmpeg, ConcatArgs, Error))
		{
			OutResult = TEXT("Assembling the trailer failed: ") + Error;
			return false;
		}

		// ---- Final encode (audio mode from panel toggles) ---------------------------
		FString OutName = TitleClean;
		OutName.ReplaceInline(TEXT(" "), TEXT(""));
		IFileManager::Get().MakeDirectory(*Options.OutputDirectory, true);
		const FString FinalPath = Options.OutputDirectory / (OutName + TEXT("_Trailer.mp4"));

		const double TotalDuration = SourceDuration + 3 * 2.4 + 4.0 + 3.0;
		FString AudioNote;

		if (!bKeepSourceAudio && !bAddScore)
		{
			// Silent trailer.
			if (!RunTool(FFmpeg, FString::Printf(
				TEXT("-y -i \"%s\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -an \"%s\""),
				*VideoCut, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode failed: ") + Error;
				return false;
			}
			AudioNote = TEXT("silent");
		}
		else if (bKeepSourceAudio && !bAddScore)
		{
			// Original render audio only (already on the cut).
			if (!RunTool(FFmpeg, FString::Printf(
				TEXT("-y -i \"%s\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -ar 48000 -ac 2 \"%s\""),
				*VideoCut, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode failed: ") + Error;
				return false;
			}
			AudioNote = bSourceHasAudio ? TEXT("source audio") : TEXT("source audio requested (none on render)");
		}
		else if (!bKeepSourceAudio && bAddScore)
		{
			// Synthetic score only (classic trailer bed).
			const FString MotifFile = Temp / TEXT("tmotif.m4a");
			const FString ScoreFile = Temp / TEXT("tscore.m4a");
			if (Style.bWhistleMotif && !MakeMotif(FFmpeg, MotifFile, Error))
			{
				OutResult = TEXT("Building the motif failed: ") + Error;
				return false;
			}
			if (!MakeScore(FFmpeg, MotifFile, Style, TotalDuration, ScoreFile, Error))
			{
				OutResult = TEXT("Building the score failed: ") + Error;
				return false;
			}
			if (!RunTool(FFmpeg, FString::Printf(
				TEXT("-y -i \"%s\" -i \"%s\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -shortest \"%s\""),
				*VideoCut, *ScoreFile, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode failed: ") + Error;
				return false;
			}
			AudioNote = TEXT("synthetic score");
		}
		else
		{
			// Mix source cut audio under the synthetic score.
			const FString MotifFile = Temp / TEXT("tmotif.m4a");
			const FString ScoreFile = Temp / TEXT("tscore.m4a");
			if (Style.bWhistleMotif && !MakeMotif(FFmpeg, MotifFile, Error))
			{
				OutResult = TEXT("Building the motif failed: ") + Error;
				return false;
			}
			if (!MakeScore(FFmpeg, MotifFile, Style, TotalDuration, ScoreFile, Error))
			{
				OutResult = TEXT("Building the score failed: ") + Error;
				return false;
			}
			// Drop score a bit under the original so dialogue/music stay lead.
			if (!RunTool(FFmpeg, FString::Printf(
				TEXT("-y -i \"%s\" -i \"%s\" -filter_complex ")
				TEXT("\"[0:a]volume=1.0[a0];[1:a]volume=0.35[a1];[a0][a1]amix=inputs=2:duration=first:dropout_transition=2[a]\" ")
				TEXT("-map 0:v -map \"[a]\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -c:a aac -b:a 192k -shortest \"%s\""),
				*VideoCut, *ScoreFile, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode (mix) failed: ") + Error;
				return false;
			}
			AudioNote = TEXT("source + synthetic mix");
		}

		UE_LOG(LogCineDirectorTrailer, Log, TEXT("Trailer cut: %s | %s | audio=%s"),
			*FinalPath, *Style.Summary, *AudioNote);
		OutResult = FString::Printf(TEXT("%s  [%s · %s]"), *FinalPath, *Style.Summary, *AudioNote);
		return true;
	}
}

FString FCineTrailerProcessor::GetStyleVocabulary()
{
	return TEXT(
		"KITS: music video · found footage / security cam / cctv / bodycam · horror · action · "
		"thriller · romance · documentary · fashion · vaporwave · cyberpunk · noir · blockbuster · "
		"anime OP · commercial / promo\n"
		"FRAME: cinematic / letterbox / cinemascope / anamorphic · dutch / canted · zoom in / push in · "
		"mirror / flip · handheld / shaky / chaotic cam\n"
		"GRADE: black and white / noir · warm / sepia / golden hour · cold / icy / bleak · "
		"teal and orange · neon · cyberpunk · green / sickly / toxic · bleach bypass · vintage / 70s / 80s · "
		"high contrast · low contrast · desaturated · overexposed · crushed blacks · natural / clean\n"
		"TEXTURE: film grain / grainy / heavy grain / no grain · scanlines / crt / vhs · "
		"flicker · chromatic aberration / fringing · glitch · pixelate / 8-bit · vignette · "
		"bloom / glow · soft / dreamy · sharp / crisp\n"
		"PACING: fast cuts / smash cuts / montage / frantic · slow burn / lingering · hard cuts · soft fades · "
		"rhythmic · trailer pace\n"
		"SCORE (if synthetic score is on): eerie · tense / heartbeat · somber · upbeat / hype · "
		"no music · drone only · no whistle\n"
		"TITLES: bold title / blockbuster titles\n"
		"Combine freely, e.g. \"music video, scanlines, film grain, zoom in, hard cuts\" "
		"or \"horror, cold, vignette, heavy grain, slow burn, dutch\"."
	);
}

FCineTrailerStyle FCineTrailerProcessor::ParseStyle(const FString& Description)
{
	using namespace CineTrailer;

	FCineTrailerStyle Style;
	const FString Text = Description.ToLower().TrimStartAndEnd();
	TArray<FString> Bits;
	bool bGradeLocked = false; // named grade won; kits shouldn't stomp it later
	bool bPacingSet = false;

	auto LockGrade = [&](const FString& Filter, const FString& Label)
	{
		Style.GradeFilter = Filter;
		bGradeLocked = true;
		Bits.Add(Label);
	};

	// Empty description → clean cinematic defaults (NOT found-footage).
	if (Text.IsEmpty())
	{
		Style.bLetterbox = true;
		Style.GradeFilter = TEXT("eq=contrast=1.18:brightness=-0.03:saturation=0.85");
		Style.GrainLevel = 6;
		Style.BeatFractions = { 0.25, 0.25, 0.25, 0.25 };
		Style.Mood = FCineTrailerStyle::EMood::Eerie;
		Style.Summary = TEXT("default cinematic (letterbox, light grain) — type a Style to customize");
		return Style;
	}

	// ---- Style kits (bundles; individual words can still override) --------------------
	const bool bFoundFootage = ContainsAnyOf(Text,
		{ TEXT("found footage"), TEXT("security cam"), TEXT("cctv"), TEXT("bodycam"), TEXT("body cam"),
		  TEXT("camcorder"), TEXT("surveillance"), TEXT("recovered tape"), TEXT("dashcam"), TEXT("dash cam") });
	const bool bVhsKit = Text.Contains(TEXT("vhs")) && (bFoundFootage || ContainsAnyOf(Text,
		{ TEXT("tape"), TEXT("security"), TEXT("found"), TEXT("camcorder") }));
	if (bFoundFootage || bVhsKit)
	{
		Style.bShake = Style.bFlicker = Style.bChromaShift = Style.bCamOverlays = true;
		Style.ShakeIntensity = 1.15f;
		Bits.Add(TEXT("found-footage kit"));
	}

	const bool bMusicVideo = ContainsAnyOf(Text,
		{ TEXT("music video"), TEXT("music-video"), TEXT("musicvideo"), TEXT("mv style"),
		  TEXT("pop video"), TEXT("hip hop video"), TEXT("rap video"), TEXT("lyric video") });
	if (bMusicVideo)
	{
		Style.bBoldTitleFont = true;
		Style.bWhistleMotif = false;
		Style.Mood = FCineTrailerStyle::EMood::Upbeat;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.2:saturation=1.28:brightness=0.02");
		}
		Bits.Add(TEXT("music-video kit"));
	}

	if (ContainsAnyOf(Text, { TEXT("horror"), TEXT("scary"), TEXT("creepy"), TEXT("haunted") }) && !bFoundFootage)
	{
		Style.bVignette = true;
		Style.bFlicker = true;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.25:brightness=-0.06:saturation=0.55,colorbalance=rs=-0.05:bs=0.08");
		}
		Style.Mood = FCineTrailerStyle::EMood::Eerie;
		Bits.Add(TEXT("horror kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("action trailer"), TEXT("action movie"), TEXT("action cut"),
		TEXT("explosive"), TEXT("blockbuster trailer"), TEXT("set piece") }))
	{
		Style.bBoldTitleFont = true;
		Style.bZoomPunch = true;
		Style.bSharpen = true;
		Style.Mood = FCineTrailerStyle::EMood::Tense;
		Style.bWhistleMotif = false;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.3:saturation=1.05:brightness=-0.02");
		}
		Bits.Add(TEXT("action kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("thriller") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Tense;
		Style.bVignette = true;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.22:saturation=0.7:brightness=-0.04,colorbalance=bs=0.06");
		}
		Bits.Add(TEXT("thriller kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("romance"), TEXT("romantic"), TEXT("love story") }))
	{
		Style.bSoft = true;
		Style.bBloom = true;
		Style.Mood = FCineTrailerStyle::EMood::Somber;
		Style.bWhistleMotif = false;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.05:saturation=1.1:brightness=0.03,colorbalance=rs=0.06:bs=-0.04");
		}
		Bits.Add(TEXT("romance kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("documentary"), TEXT("doc style"), TEXT("interview") }))
	{
		Style.GrainLevel = 5;
		Style.BeatFadeIn = 0.15f;
		Style.BeatFadeOut = 0.2f;
		if (!bGradeLocked)
		{
			Style.GradeFilter.Empty();
		}
		Bits.Add(TEXT("documentary kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("fashion"), TEXT("editorial"), TEXT("lookbook") }))
	{
		Style.bBoldTitleFont = true;
		Style.bSharpen = true;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.15:saturation=0.9:brightness=0.02");
		}
		Bits.Add(TEXT("fashion kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("vaporwave"), TEXT("synthwave"), TEXT("retrowave") }))
	{
		Style.bScanlines = true;
		Style.bBloom = true;
		Style.Mood = FCineTrailerStyle::EMood::Upbeat;
		Style.bWhistleMotif = false;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.15:saturation=1.4,colorbalance=rs=0.12:bs=0.15:rm=0.08:bm=0.1");
		}
		Bits.Add(TEXT("vaporwave kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("cyberpunk"), TEXT("neon noir") }))
	{
		Style.bVignette = true;
		Style.bChromaShift = true;
		Style.Mood = FCineTrailerStyle::EMood::Tense;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.35:saturation=1.45:brightness=-0.05,colorbalance=rs=0.15:bs=0.2:rm=0.1:bm=0.12");
		}
		Bits.Add(TEXT("cyberpunk kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("anime op"), TEXT("anime opening"), TEXT("anime style") }))
	{
		Style.bBoldTitleFont = true;
		Style.bZoomPunch = true;
		Style.Mood = FCineTrailerStyle::EMood::Upbeat;
		Style.bWhistleMotif = false;
		if (!bGradeLocked)
		{
			Style.GradeFilter = TEXT("eq=contrast=1.18:saturation=1.35:brightness=0.04");
		}
		Bits.Add(TEXT("anime-op kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("commercial"), TEXT("promo"), TEXT("advert"), TEXT("ad style") }))
	{
		Style.bBoldTitleFont = true;
		Style.bSharpen = true;
		Style.GrainLevel = 0;
		Style.Mood = FCineTrailerStyle::EMood::Upbeat;
		Style.bWhistleMotif = false;
		Bits.Add(TEXT("commercial kit"));
	}
	if (ContainsAnyOf(Text, { TEXT("noir") }) && !Text.Contains(TEXT("neon")))
	{
		// "noir" also hits B&W grade below; kit adds vignette + hard mood.
		Style.bVignette = true;
		Style.Mood = FCineTrailerStyle::EMood::Somber;
	}

	// ---- Camera / frame effects -------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("handheld"), TEXT("shaky"), TEXT("shake"), TEXT("doc cam"), TEXT("run and gun") }))
	{
		Style.bShake = true;
		Bits.Add(TEXT("handheld shake"));
	}
	if (ContainsAnyOf(Text, { TEXT("chaotic"), TEXT("wild cam"), TEXT("whip pan"), TEXT("unstable") }))
	{
		Style.bShake = true;
		Style.ShakeIntensity = 1.7f;
		Bits.Add(TEXT("chaotic cam"));
	}
	if (ContainsAnyOf(Text, { TEXT("dutch"), TEXT("canted"), TEXT("tilted frame"), TEXT("oblique") }))
	{
		Style.bDutch = true;
		Bits.Add(TEXT("dutch angle"));
	}
	if (ContainsAnyOf(Text, { TEXT("zoom in"), TEXT("zoom punch"), TEXT("push in"), TEXT("crash zoom"), TEXT("punch in"), TEXT("ken burns") }))
	{
		Style.bZoomPunch = true;
		Bits.Add(TEXT("zoom push-in"));
	}
	if (ContainsAnyOf(Text, { TEXT("mirror"), TEXT("flip"), TEXT("flipped"), TEXT("hflip") }))
	{
		Style.bMirror = true;
		Bits.Add(TEXT("mirrored"));
	}
	if (Text.Contains(TEXT("flicker")) || ContainsAnyOf(Text, { TEXT("strobe"), TEXT("flashing"), TEXT("exposure pop") }))
	{
		Style.bFlicker = true;
		Bits.Add(TEXT("exposure flicker"));
	}

	// ---- Scanlines / CRT / VHS texture ------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("scanline"), TEXT("scan line"), TEXT("crt"), TEXT("raster"), TEXT("tube tv"), TEXT("old tv") })
		|| (Text.Contains(TEXT("vhs")) && !bFoundFootage && !bVhsKit))
	{
		Style.bScanlines = true;
		Bits.Add(TEXT("scanlines"));
	}

	// ---- Frame / letterbox ------------------------------------------------------------
	const bool bCinematicWord = ContainsAnyOf(Text,
		{ TEXT("cinematic"), TEXT("cinemascope"), TEXT("letterbox"), TEXT("widescreen"), TEXT("anamorphic"),
		  TEXT("scope"), TEXT("2.39"), TEXT("2.35") });
	if (bCinematicWord)
	{
		Style.bLetterbox = true;
		Bits.Add(TEXT("cinemascope letterbox"));
	}

	// ---- Color grades (first match wins; most specific phrases first) -----------------
	if (ContainsAnyOf(Text, { TEXT("black and white"), TEXT("black & white"), TEXT("b&w"), TEXT("b and w"),
		TEXT("monochrome"), TEXT("greyscale"), TEXT("grayscale") })
		|| (Text.Contains(TEXT("noir")) && !Text.Contains(TEXT("neon"))))
	{
		LockGrade(TEXT("hue=s=0,eq=contrast=1.32:brightness=-0.04"), TEXT("black & white"));
	}
	else if (ContainsAnyOf(Text, { TEXT("bleach bypass"), TEXT("bleach"), TEXT("silver retention") }))
	{
		LockGrade(TEXT("eq=contrast=1.4:saturation=0.35:brightness=-0.02"), TEXT("bleach bypass"));
	}
	else if (ContainsAnyOf(Text, { TEXT("teal and orange"), TEXT("teal orange"), TEXT("blockbuster grade"), TEXT("hollywood") }))
	{
		LockGrade(TEXT("eq=contrast=1.2:saturation=1.1,colorbalance=rs=0.1:bs=-0.08:gs=0.02:rm=0.06:bm=-0.05"), TEXT("teal & orange"));
	}
	else if (ContainsAnyOf(Text, { TEXT("neon"), TEXT("cyberpunk"), TEXT("synthwave"), TEXT("vaporwave") }))
	{
		// Kit may have set similar grade; lock explicit neon wording.
		LockGrade(TEXT("eq=contrast=1.3:saturation=1.5:brightness=-0.03,colorbalance=rs=0.14:bs=0.18:rm=0.1:bm=0.12"), TEXT("neon grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("green"), TEXT("alien grade"), TEXT("sickly"), TEXT("toxic"), TEXT("matrix green") }))
	{
		LockGrade(TEXT("eq=contrast=1.28:brightness=-0.07:saturation=0.4,colorbalance=rs=-0.12:gs=0.10:bs=0.02:rm=-0.08:gm=0.06"), TEXT("sickly green"));
	}
	else if (ContainsAnyOf(Text, { TEXT("sepia"), TEXT("brown tone") }))
	{
		LockGrade(TEXT("colorchannelmixer=.393:.769:.189:0:.349:.686:.168:0:.272:.534:.131,eq=contrast=1.1"), TEXT("sepia"));
	}
	else if (ContainsAnyOf(Text, { TEXT("warm"), TEXT("golden hour"), TEXT("golden"), TEXT("sunset grade"), TEXT("amber") }))
	{
		LockGrade(TEXT("eq=contrast=1.12:saturation=1.05,colorbalance=rs=0.1:gs=0.03:bs=-0.1:rm=0.06:bm=-0.06"), TEXT("warm grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("cold"), TEXT("icy"), TEXT("blue tint"), TEXT("bleak"), TEXT("winter"), TEXT("steel") }))
	{
		LockGrade(TEXT("eq=contrast=1.2:saturation=0.75,colorbalance=rs=-0.08:bs=0.12:bm=0.08"), TEXT("cold blue grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("vintage"), TEXT("retro"), TEXT("70s"), TEXT("1970"), TEXT("old film"), TEXT("faded film") }))
	{
		LockGrade(TEXT("eq=contrast=1.08:saturation=0.7:brightness=0.04,colorbalance=rs=0.08:gs=0.04:bs=-0.06"), TEXT("vintage fade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("80s"), TEXT("1980"), TEXT("miami") }))
	{
		LockGrade(TEXT("eq=contrast=1.15:saturation=1.35,colorbalance=rs=0.12:bs=0.1:rm=0.08"), TEXT("80s pop grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("high contrast"), TEXT("crushed"), TEXT("punchy contrast") }))
	{
		LockGrade(TEXT("eq=contrast=1.45:brightness=-0.05:saturation=1.05"), TEXT("high contrast"));
	}
	else if (ContainsAnyOf(Text, { TEXT("low contrast"), TEXT("flat grade"), TEXT("muted contrast"), TEXT("pastel") }))
	{
		LockGrade(TEXT("eq=contrast=0.85:saturation=0.9:brightness=0.05"), TEXT("low contrast"));
	}
	else if (ContainsAnyOf(Text, { TEXT("desaturated"), TEXT("muted colors"), TEXT("washed out"), TEXT("desat") }))
	{
		LockGrade(TEXT("eq=contrast=1.1:saturation=0.45"), TEXT("desaturated"));
	}
	else if (ContainsAnyOf(Text, { TEXT("overexposed"), TEXT("blown out"), TEXT("high key"), TEXT("bright look") }))
	{
		LockGrade(TEXT("eq=contrast=0.95:brightness=0.12:saturation=0.95"), TEXT("overexposed"));
	}
	else if (ContainsAnyOf(Text, { TEXT("underexposed"), TEXT("crushed blacks"), TEXT("low key"), TEXT("dark look"), TEXT("moody dark") }))
	{
		LockGrade(TEXT("eq=contrast=1.25:brightness=-0.12:saturation=0.85"), TEXT("crushed / low-key"));
	}
	else if (ContainsAnyOf(Text, { TEXT("natural"), TEXT("original color"), TEXT("no grade"), TEXT("ungraded"), TEXT("clean color") }))
	{
		Style.GradeFilter.Empty();
		bGradeLocked = true;
		Bits.Add(TEXT("natural colors"));
	}
	else if (!bGradeLocked && bCinematicWord)
	{
		Style.GradeFilter = TEXT("eq=contrast=1.22:brightness=-0.05:saturation=0.6");
		Bits.Add(TEXT("neutral cinematic grade"));
	}
	else if (!bGradeLocked && Style.GradeFilter.IsEmpty())
	{
		Bits.Add(TEXT("natural colors"));
	}

	// ---- Optical / polish effects -----------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("vignette"), TEXT("dark edges"), TEXT("tunnel") }))
	{
		Style.bVignette = true;
		Bits.Add(TEXT("vignette"));
	}
	if (ContainsAnyOf(Text, { TEXT("bloom"), TEXT("glow"), TEXT("halation"), TEXT("soft glow") }))
	{
		Style.bBloom = true;
		Bits.Add(TEXT("bloom/glow"));
	}
	if (ContainsAnyOf(Text, { TEXT("soft focus"), TEXT("dreamy"), TEXT("dreamlike"), TEXT("ethereal"), TEXT("gauzy"), TEXT("soft look") }))
	{
		Style.bSoft = true;
		Bits.Add(TEXT("soft/dreamy"));
	}
	if (ContainsAnyOf(Text, { TEXT("sharpen"), TEXT("sharpened"), TEXT("razor sharp"), TEXT("crisp detail") }))
	{
		Style.bSharpen = true;
		Bits.Add(TEXT("sharpened"));
	}
	if (ContainsAnyOf(Text, { TEXT("chromatic"), TEXT("fringing"), TEXT("rgb split"), TEXT("aberration"), TEXT("prism") }))
	{
		Style.bChromaShift = true;
		Bits.Add(TEXT("chromatic aberration"));
	}
	if (ContainsAnyOf(Text, { TEXT("glitch"), TEXT("datamosh"), TEXT("corrupt"), TEXT("digital error"), TEXT("broken signal") }))
	{
		Style.bGlitch = true;
		Bits.Add(TEXT("glitch"));
	}
	if (ContainsAnyOf(Text, { TEXT("pixelate"), TEXT("pixelated"), TEXT("8-bit"), TEXT("8 bit"), TEXT("8bit"), TEXT("mosaic"), TEXT("blocky") }))
	{
		Style.bPixelate = true;
		Bits.Add(TEXT("pixelate"));
	}
	if (ContainsAnyOf(Text, { TEXT("invert"), TEXT("negative"), TEXT("inverted colors") }))
	{
		Style.bInvert = true;
		Bits.Add(TEXT("inverted"));
	}

	// ---- Grain ------------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("no grain"), TEXT("grainless") })
		|| (Text.Contains(TEXT("crisp")) && !Text.Contains(TEXT("grain"))))
	{
		Style.GrainLevel = 0;
		Bits.Add(TEXT("no grain"));
	}
	else if (ContainsAnyOf(Text, { TEXT("heavy grain"), TEXT("grainy"), TEXT("gritty"), TEXT("lots of grain"), TEXT("16mm"), TEXT("super 8"), TEXT("super8") }))
	{
		Style.GrainLevel = 14;
		Bits.Add(TEXT("heavy grain"));
	}
	else if (ContainsAnyOf(Text, { TEXT("film grain"), TEXT("film-grain"), TEXT("35mm") })
		|| (Text.Contains(TEXT("grain")) && !Text.Contains(TEXT("no grain"))))
	{
		Style.GrainLevel = 11;
		Bits.Add(TEXT("film grain"));
	}
	else if (Style.GrainLevel == 8) // still at default — apply kit-aware defaults
	{
		Style.GrainLevel = (bFoundFootage || bVhsKit) ? 13 : (bMusicVideo ? 7 : 6);
		if (Style.GrainLevel > 0)
		{
			Bits.Add(Style.GrainLevel >= 12 ? TEXT("heavy grain") : TEXT("light grain"));
		}
	}

	// ---- Transitions ------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("hard cut"), TEXT("hard cuts"), TEXT("smash cut"), TEXT("smash cuts"),
		TEXT("jump cut"), TEXT("jump cuts"), TEXT("no fade"), TEXT("no fades") }))
	{
		Style.BeatFadeIn = 0.0f;
		Style.BeatFadeOut = 0.05f;
		Bits.Add(TEXT("hard cuts"));
	}
	else if (ContainsAnyOf(Text, { TEXT("soft fade"), TEXT("soft fades"), TEXT("crossfade"), TEXT("dissolve"), TEXT("gentle fade") }))
	{
		Style.BeatFadeIn = 0.55f;
		Style.BeatFadeOut = 0.6f;
		Bits.Add(TEXT("soft fades"));
	}

	// ---- Pacing -----------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("fast cut"), TEXT("fast cuts"), TEXT("quick cut"), TEXT("rapid"), TEXT("frantic"),
		TEXT("punchy"), TEXT("montage"), TEXT("rhythmic"), TEXT("trailer pace"), TEXT("music video") })
		|| bMusicVideo
		|| ContainsAnyOf(Text, { TEXT("action trailer"), TEXT("anime op") }))
	{
		Style.BeatFractions = { 0.20, 0.18, 0.16, 0.15, 0.12, 0.10, 0.09 };
		bPacingSet = true;
		Bits.Add(TEXT("fast escalating cuts"));
	}
	else if (ContainsAnyOf(Text, { TEXT("slow burn"), TEXT("slow"), TEXT("long take"), TEXT("lingering"), TEXT("meditative"), TEXT("patient") }))
	{
		Style.BeatFractions = { 0.40, 0.30, 0.30 };
		bPacingSet = true;
		Bits.Add(TEXT("slow burn (3 long beats)"));
	}
	else if (ContainsAnyOf(Text, { TEXT("classic trailer"), TEXT("standard pace"), TEXT("four beat") }))
	{
		Style.BeatFractions = { 0.28, 0.24, 0.24, 0.24 };
		bPacingSet = true;
		Bits.Add(TEXT("classic 4-beat pace"));
	}
	if (!bPacingSet)
	{
		Style.BeatFractions = { 0.25, 0.25, 0.25, 0.25 };
		Bits.Add(TEXT("even beats (4)"));
	}

	// ---- Score mood (only describes synthetic score when that checkbox is on) ---------
	if (ContainsAnyOf(Text, { TEXT("no music"), TEXT("silent score"), TEXT("no score"), TEXT("score off") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::None;
		Bits.Add(TEXT("no synthetic score mood"));
	}
	else if (ContainsAnyOf(Text, { TEXT("tense"), TEXT("pulse"), TEXT("heartbeat"), TEXT("thriller score"), TEXT("urgent") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Tense;
		Bits.Add(TEXT("tense score mood"));
	}
	else if (ContainsAnyOf(Text, { TEXT("somber"), TEXT("sad"), TEXT("melanchol"), TEXT("mournful"), TEXT("quiet score") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Somber;
		Bits.Add(TEXT("somber score mood"));
	}
	else if (ContainsAnyOf(Text, { TEXT("upbeat"), TEXT("energetic"), TEXT("hype"), TEXT("club"), TEXT("dance") })
		|| bMusicVideo)
	{
		Style.Mood = FCineTrailerStyle::EMood::Upbeat;
		Bits.Add(TEXT("upbeat score mood"));
	}
	else if (ContainsAnyOf(Text, { TEXT("eerie"), TEXT("horror score"), TEXT("drone"), TEXT("uneasy") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Eerie;
		Bits.Add(TEXT("eerie score mood"));
	}

	if (ContainsAnyOf(Text, { TEXT("no whistle"), TEXT("no melody"), TEXT("no theme"), TEXT("drone only") })
		|| bMusicVideo
		|| Style.Mood == FCineTrailerStyle::EMood::Upbeat)
	{
		Style.bWhistleMotif = false;
	}

	// ---- Titles -----------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("bold title"), TEXT("bold text"), TEXT("blockbuster"), TEXT("impact title"), TEXT("big titles") })
		|| bMusicVideo)
	{
		Style.bBoldTitleFont = true;
		Bits.Add(TEXT("bold titles"));
	}

	// De-dupe bits while keeping order.
	{
		TArray<FString> Unique;
		for (const FString& B : Bits)
		{
			if (!Unique.Contains(B))
			{
				Unique.Add(B);
			}
		}
		Bits = MoveTemp(Unique);
	}

	Style.Summary = FString::Join(Bits, TEXT(" • "));
	if (Style.Summary.IsEmpty())
	{
		Style.Summary = TEXT("minimal treatment");
	}
	return Style;
}

void FCineTrailerProcessor::ProcessAsync(const FCineTrailerOptions& Options, TFunction<void(bool, FString)> OnDone)
{
	Async(EAsyncExecution::Thread, [Options, OnDone]()
	{
		FString Result;
		const bool bSuccess = CineTrailer::Process(Options, Result);
		AsyncTask(ENamedThreads::GameThread, [bSuccess, Result, OnDone]()
		{
			if (OnDone)
			{
				OnDone(bSuccess, Result);
			}
		});
	});
}

FString FCineTrailerProcessor::FindNewestMp4(const FString& Directory, const FDateTime& MinTimeUtc)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.mp4")), /*Files*/ true, /*Dirs*/ false);

	FString Newest;
	FDateTime NewestTime = MinTimeUtc - FTimespan::FromSeconds(30);
	for (const FString& File : Files)
	{
		const FString FullPath = Directory / File;
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*FullPath);
		if (Stamp >= NewestTime)
		{
			NewestTime = Stamp;
			Newest = FullPath;
		}
	}
	return Newest;
}
