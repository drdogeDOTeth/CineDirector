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
		double Duration, double FadeInDur, double FadeOutStart, double FadeOutDur, const FString& OutFile, FString& OutError)
	{
		const TCHAR* Font = Style.bBoldTitleFont
			? TEXT("fontfile='C\\:/Windows/Fonts/arialbd.ttf'")
			: TEXT("fontfile='C\\:/Windows/Fonts/constan.ttf'");
		const FString Filter = FString::Printf(
			TEXT("drawtext=%s:text='%s':fontcolor=0xC8C8CC:fontsize=%d:x=(w-text_w)/2:y=(h-text_h)/2,")
			TEXT("noise=alls=7:allf=t,fade=t=in:st=0:d=%.2f,fade=t=out:st=%.2f:d=%.2f"),
			Font, *SpacedText, FontSize, FadeInDur, FadeOutStart, FadeOutDur);
		const FString Args = FString::Printf(
			TEXT("-y -f lavfi -i \"color=c=black:s=1920x1080:d=%.2f:r=24\" -vf \"%s\" -c:v libx264 -crf 18 -pix_fmt yuv420p \"%s\""),
			Duration, *Filter, *OutFile);
		return RunTool(FFmpeg, Args, OutError);
	}

	bool MakeBeat(const FString& FFmpeg, const FString& Source, const FCineTrailerStyle& Style,
		double Start, double Duration, const FString& CameraTag, const FString& OutFile, FString& OutError)
	{
		FString Filter;

		if (Style.bShake)
		{
			Filter += TEXT("crop=w=iw-48:h=ih-28:x='24+12*sin(2.3*t)+7*sin(7.7*t)':y='14+7*cos(3.1*t)+5*sin(9.3*t)',scale=1920:1080,");
		}
		if (!Style.GradeFilter.IsEmpty())
		{
			Filter += Style.GradeFilter + TEXT(",");
		}
		if (Style.bFlicker)
		{
			Filter += TEXT("eq=eval=frame:brightness='0.0+0.035*sin(15*t)+0.025*sin(47*t)',");
		}
		if (Style.bChromaShift)
		{
			Filter += TEXT("chromashift=cbh=3:crh=-3,");
		}
		if (Style.GrainLevel > 0)
		{
			Filter += FString::Printf(TEXT("noise=alls=%d:allf=t+u,"), Style.GrainLevel);
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

		Filter += FString::Printf(TEXT("fade=t=in:st=0:d=0.35,fade=t=out:st=%.2f:d=0.4"), FMath::Max(Duration - 0.4, 0.0));

		const FString Args = FString::Printf(
			TEXT("-y -ss %.3f -t %.3f -i \"%s\" -vf \"%s\" -an -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 \"%s\""),
			Start, Duration, *Source, *Filter, *OutFile);
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
		UE_LOG(LogCineDirectorTrailer, Log, TEXT("Trailer style: %s"), *Style.Summary);

		const double SourceDuration = ProbeDurationSeconds(FFprobe, Options.SourceVideo);
		if (SourceDuration < 8.0)
		{
			OutResult = FString::Printf(TEXT("Source is too short to cut a trailer (%.1fs; need at least 8s)."), SourceDuration);
			return false;
		}

		const FString Temp = FPaths::ProjectSavedDir() / TEXT("CineDirectorTrailer");
		IFileManager::Get().MakeDirectory(*Temp, true);

		// ---- Beats -----------------------------------------------------------------
		const int32 NumBeats = Style.BeatFractions.Num();
		FString Error;
		double Cursor = 0.0;
		TArray<double> BeatStarts, BeatDurations;
		for (int32 i = 0; i < NumBeats; ++i)
		{
			const double Duration = SourceDuration * Style.BeatFractions[i];
			BeatStarts.Add(Cursor);
			BeatDurations.Add(Duration);
			if (!MakeBeat(FFmpeg, Options.SourceVideo, Style, Cursor, Duration, Options.CameraTag,
				Temp / FString::Printf(TEXT("beat%d.mp4"), i + 1), Error))
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
			if (!MakeCard(FFmpeg, Style, Spaced, 46, 2.4, 0.5, 1.9, 0.5, Temp / FString::Printf(TEXT("tcard%d.mp4"), i + 1), Error))
			{
				OutResult = TEXT("Rendering a title card failed: ") + Error;
				return false;
			}
		}

		const FString TitleClean = SanitizeCardText(Options.MovieTitle.IsEmpty() ? TEXT("UNTITLED") : Options.MovieTitle);
		if (!MakeCard(FFmpeg, Style, Letterspace(TitleClean), 130, 4.0, 1.2, 3.2, 0.8, Temp / TEXT("ttitle.mp4"), Error) ||
			!MakeCard(FFmpeg, Style, Letterspace(TEXT("COMING SOON")), 44, 3.0, 0.6, 2.2, 0.8, Temp / TEXT("tcoming.mp4"), Error))
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
		if (!RunTool(FFmpeg, FString::Printf(
			TEXT("-y -f concat -safe 0 -i \"%s\" -c:v libx264 -crf 18 -pix_fmt yuv420p -r 24 \"%s\""), *ListFile, *VideoCut), Error))
		{
			OutResult = TEXT("Assembling the trailer failed: ") + Error;
			return false;
		}

		// ---- Score + final encode ---------------------------------------------------
		FString OutName = TitleClean;
		OutName.ReplaceInline(TEXT(" "), TEXT(""));
		IFileManager::Get().MakeDirectory(*Options.OutputDirectory, true);
		const FString FinalPath = Options.OutputDirectory / (OutName + TEXT("_Trailer.mp4"));

		if (Style.Mood == FCineTrailerStyle::EMood::None)
		{
			if (!RunTool(FFmpeg, FString::Printf(
				TEXT("-y -i \"%s\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -an \"%s\""), *VideoCut, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode failed: ") + Error;
				return false;
			}
		}
		else
		{
			const double TotalDuration = SourceDuration + 3 * 2.4 + 4.0 + 3.0;
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
				TEXT("-y -i \"%s\" -i \"%s\" -c:v libx264 -crf 23 -preset slow -pix_fmt yuv420p -c:a copy -shortest \"%s\""),
				*VideoCut, *ScoreFile, *FinalPath), Error))
			{
				OutResult = TEXT("Final encode failed: ") + Error;
				return false;
			}
		}

		UE_LOG(LogCineDirectorTrailer, Log, TEXT("Trailer cut: %s"), *FinalPath);
		OutResult = FinalPath;
		return true;
	}
}

FCineTrailerStyle FCineTrailerProcessor::ParseStyle(const FString& Description)
{
	using namespace CineTrailer;

	FCineTrailerStyle Style;
	const FString Text = Description.ToLower();
	TArray<FString> Bits;

	// ---- Found-footage kit ---------------------------------------------------------
	const bool bFoundFootage = ContainsAnyOf(Text,
		{ TEXT("found footage"), TEXT("security cam"), TEXT("cctv"), TEXT("vhs"), TEXT("bodycam"), TEXT("body cam"),
		  TEXT("camcorder"), TEXT("surveillance"), TEXT("recovered tape") });
	if (bFoundFootage)
	{
		Style.bShake = Style.bFlicker = Style.bChromaShift = Style.bCamOverlays = true;
		Bits.Add(TEXT("found-footage kit (shake, flicker, fringing, REC/timecode)"));
	}
	if (ContainsAnyOf(Text, { TEXT("handheld"), TEXT("shaky"), TEXT("shake") }))
	{
		Style.bShake = true;
		if (!bFoundFootage)
		{
			Bits.Add(TEXT("handheld shake"));
		}
	}
	if (Text.Contains(TEXT("flicker")))
	{
		Style.bFlicker = true;
	}

	// ---- Frame ------------------------------------------------------------------------
	const bool bCinematicWord = ContainsAnyOf(Text, { TEXT("cinematic"), TEXT("cinemascope"), TEXT("letterbox"), TEXT("widescreen"), TEXT("anamorphic") });
	if (bCinematicWord)
	{
		Style.bLetterbox = true;
		Bits.Add(TEXT("cinemascope letterbox"));
	}

	// ---- Look / grade -------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("black and white"), TEXT("b&w"), TEXT("bw"), TEXT("noir"), TEXT("monochrome") }))
	{
		Style.GradeFilter = TEXT("hue=s=0,eq=contrast=1.3:brightness=-0.04");
		Bits.Add(TEXT("black & white"));
	}
	else if (ContainsAnyOf(Text, { TEXT("green"), TEXT("alien grade"), TEXT("sickly"), TEXT("toxic") }))
	{
		Style.GradeFilter = TEXT("eq=contrast=1.28:brightness=-0.07:saturation=0.4,colorbalance=rs=-0.12:gs=0.10:bs=0.02:rm=-0.08:gm=0.06");
		Bits.Add(TEXT("sickly green horror grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("warm"), TEXT("sepia"), TEXT("golden") }))
	{
		Style.GradeFilter = TEXT("eq=contrast=1.15:saturation=0.85,colorbalance=rs=0.08:gs=0.02:bs=-0.08:rm=0.05:bm=-0.05");
		Bits.Add(TEXT("warm grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("cold"), TEXT("icy"), TEXT("blue tint"), TEXT("bleak") }))
	{
		Style.GradeFilter = TEXT("eq=contrast=1.2:saturation=0.75,colorbalance=rs=-0.08:bs=0.10:bm=0.06");
		Bits.Add(TEXT("cold blue grade"));
	}
	else if (ContainsAnyOf(Text, { TEXT("natural"), TEXT("original color"), TEXT("no grade"), TEXT("ungraded"), TEXT("clean") }))
	{
		Bits.Add(TEXT("natural colors"));
	}
	else if (bCinematicWord)
	{
		Style.GradeFilter = TEXT("eq=contrast=1.22:brightness=-0.05:saturation=0.6");
		Bits.Add(TEXT("neutral cinematic grade"));
	}
	else
	{
		Bits.Add(TEXT("natural colors"));
	}

	// ---- Grain -------------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("no grain"), TEXT("crisp") }))
	{
		Style.GrainLevel = 0;
	}
	else if (ContainsAnyOf(Text, { TEXT("heavy grain"), TEXT("grainy"), TEXT("gritty") }))
	{
		Style.GrainLevel = 14;
		Bits.Add(TEXT("heavy grain"));
	}
	else
	{
		Style.GrainLevel = bFoundFootage ? 13 : 8;
	}

	// ---- Pacing --------------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("fast"), TEXT("quick cut"), TEXT("rapid"), TEXT("frantic"), TEXT("punchy") }))
	{
		Style.BeatFractions = { 0.25, 0.21, 0.18, 0.15, 0.12, 0.09 };
		Bits.Add(TEXT("fast escalating cuts (6 beats)"));
	}
	else
	{
		Style.BeatFractions = { 0.25, 0.25, 0.25, 0.25 };
		Bits.Add(TEXT("long beats (4)"));
	}

	// ---- Music -------------------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("no music"), TEXT("silent"), TEXT("no audio"), TEXT("no sound") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::None;
		Bits.Add(TEXT("no music"));
	}
	else if (ContainsAnyOf(Text, { TEXT("tense"), TEXT("pulse"), TEXT("heartbeat"), TEXT("thriller"), TEXT("urgent") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Tense;
		Bits.Add(TEXT("tense pulsing score"));
	}
	else if (ContainsAnyOf(Text, { TEXT("somber"), TEXT("sad"), TEXT("melanchol"), TEXT("mournful"), TEXT("quiet") }))
	{
		Style.Mood = FCineTrailerStyle::EMood::Somber;
		Bits.Add(TEXT("somber score"));
	}
	else
	{
		Bits.Add(TEXT("eerie drone score"));
	}
	if (ContainsAnyOf(Text, { TEXT("no whistle"), TEXT("no melody"), TEXT("no theme"), TEXT("drone only") }))
	{
		Style.bWhistleMotif = false;
	}

	// ---- Titles ----------------------------------------------------------------------------------
	if (ContainsAnyOf(Text, { TEXT("bold title"), TEXT("bold text"), TEXT("blockbuster") }))
	{
		Style.bBoldTitleFont = true;
		Bits.Add(TEXT("bold titles"));
	}

	Style.Summary = FString::Join(Bits, TEXT(" • "));
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
