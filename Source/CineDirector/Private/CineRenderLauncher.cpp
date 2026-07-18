// Copyright Roundtree. All Rights Reserved.

#include "CineRenderLauncher.h"

#include "Editor.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "MoviePipelineAntiAliasingSetting.h"
#include "MoviePipelineCommandLineEncoder.h"
#include "MoviePipelineCommandLineEncoderSettings.h"
#include "MoviePipelineDeferredPasses.h"
#include "MoviePipelineImageSequenceOutput.h"
#include "MoviePipelineSetting.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"

#define LOCTEXT_NAMESPACE "CineDirector"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorRender, Log, All);

FString FCineRenderLauncher::FindFFmpegExecutable()
	{
		const UMoviePipelineCommandLineEncoderSettings* Settings = GetDefault<UMoviePipelineCommandLineEncoderSettings>();
		if (!Settings->ExecutablePath.IsEmpty() && FPaths::FileExists(Settings->ExecutablePath))
		{
			return Settings->ExecutablePath;
		}

		FString StdOut, StdErr;
		int32 ReturnCode = -1;
		FPlatformProcess::ExecProcess(TEXT("where"), TEXT("ffmpeg"), &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode == 0)
		{
			TArray<FString> Lines;
			StdOut.ParseIntoArrayLines(Lines);
			if (Lines.Num() > 0 && FPaths::FileExists(Lines[0].TrimStartAndEnd()))
			{
				return Lines[0].TrimStartAndEnd();
			}
		}

		const FString WinGetPackages =
			FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")) / TEXT("Microsoft/WinGet/Packages");
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *WinGetPackages, TEXT("ffmpeg.exe"), /*Files*/ true, /*Dirs*/ false);
		for (const FString& Candidate : Found)
		{
			if (Candidate.Contains(TEXT("Gyan.FFmpeg")))
			{
				return Candidate;
			}
		}

		return FString();
	}

bool FCineRenderLauncher::StartRender(const FCineRenderOptions& Options, FText& OutError, TFunction<void(bool)> OnFinished)
{
	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!Sequence)
	{
		OutError = LOCTEXT("RenderNoSequence", "No Level Sequence is open in Sequencer — create some shots first.");
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = LOCTEXT("RenderNoWorld", "No editor world available.");
		return false;
	}

	// Movie Render Queue loads the map by asset path, so an unsaved level can't render.
	const FString WorldPackageName = World->GetOutermost()->GetName();
	if (WorldPackageName.StartsWith(TEXT("/Temp/")) || WorldPackageName.Contains(TEXT("Untitled")))
	{
		OutError = LOCTEXT("RenderUnsavedMap", "Save your level first (File ▸ Save Current Level) — Movie Render Queue loads the map from disk.");
		return false;
	}

	UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
	if (!Subsystem)
	{
		OutError = LOCTEXT("RenderNoMRQ", "Movie Render Queue is unavailable — enable the \"Movie Render Queue\" plugin and restart the editor.");
		return false;
	}
	if (Subsystem->IsRendering())
	{
		OutError = LOCTEXT("RenderBusy", "A render is already in progress.");
		return false;
	}

	// The sequence asset must also exist on disk for MRQ to load it.
	if (Sequence->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		OutError = LOCTEXT("RenderUnsavedSequence", "Save the Level Sequence asset first — Movie Render Queue loads it from disk.");
		return false;
	}

	UMoviePipelineQueue* Queue = Subsystem->GetQueue();
	Queue->Modify();
	Queue->DeleteAllJobs();

	UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	Job->Sequence = FSoftObjectPath(Sequence);
	Job->Map = FSoftObjectPath(World);
	Job->JobName = Sequence->GetName();

	UMoviePipelinePrimaryConfig* Config = Job->GetConfiguration();
	Config->FindOrAddSettingByClass(UMoviePipelineDeferredPassBase::StaticClass());

	switch (Options.Format)
	{
	case FCineRenderOptions::EFormat::JPEG:
		Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_JPG::StaticClass());
		break;
	case FCineRenderOptions::EFormat::EXR:
	{
		// MoviePipelineEXROutput.h drags the OpenEXR third-party headers into our
		// module, so resolve the class reflectively instead of including it.
		if (UClass* EXRClass = LoadClass<UMoviePipelineSetting>(nullptr, TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_EXR")))
		{
			Config->FindOrAddSettingByClass(EXRClass);
		}
		else
		{
			UE_LOG(LogCineDirectorRender, Warning, TEXT("EXR output class not found — falling back to PNG."));
			Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
		}
		break;
	}
	case FCineRenderOptions::EFormat::BMP:
		Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_BMP::StaticClass());
		break;
	case FCineRenderOptions::EFormat::MP4:
	{
		const FString FFmpegPath = FindFFmpegExecutable();
		if (FFmpegPath.IsEmpty())
		{
			OutError = LOCTEXT("RenderNoFFmpeg",
				"MP4 needs ffmpeg and it wasn't found. Install it (winget install Gyan.FFmpeg) or set the path in Project Settings ▸ Movie Pipeline CLI Encoder.");
			return false;
		}

		// Point the project's CLI-encoder settings at ffmpeg (persisted so the
		// stock Movie Render Queue UI benefits too).
		UMoviePipelineCommandLineEncoderSettings* EncoderSettings = GetMutableDefault<UMoviePipelineCommandLineEncoderSettings>();
		EncoderSettings->ExecutablePath = FFmpegPath;
		if (EncoderSettings->VideoCodec.IsEmpty())
		{
			EncoderSettings->VideoCodec = TEXT("libx264");
		}
		if (EncoderSettings->AudioCodec.IsEmpty())
		{
			EncoderSettings->AudioCodec = TEXT("aac");
		}
		if (EncoderSettings->OutputFileExtension.IsEmpty())
		{
			EncoderSettings->OutputFileExtension = TEXT("mp4");
		}
		EncoderSettings->TryUpdateDefaultConfigFile();

		// The encoder consumes a rendered image sequence and deletes it afterwards.
		Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
		UMoviePipelineCommandLineEncoder* Encoder =
			Cast<UMoviePipelineCommandLineEncoder>(Config->FindOrAddSettingByClass(UMoviePipelineCommandLineEncoder::StaticClass()));
		Encoder->Quality = static_cast<EMoviePipelineEncodeQuality>(FMath::Clamp(Options.EncodeQuality, 0, 3));
		Encoder->bDeleteSourceFiles = true;
		break;
	}
	case FCineRenderOptions::EFormat::PNG:
	default:
		Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
		break;
	}

	UMoviePipelineOutputSetting* OutputSetting =
		Cast<UMoviePipelineOutputSetting>(Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
	OutputSetting->OutputResolution = Options.Resolution;
	if (!Options.OutputDirectory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Options.OutputDirectory, /*Tree*/ true);
		OutputSetting->OutputDirectory.Path = Options.OutputDirectory;
	}

	if (Options.TemporalSamples > 1 || Options.SpatialSamples > 1)
	{
		UMoviePipelineAntiAliasingSetting* AA =
			Cast<UMoviePipelineAntiAliasingSetting>(Config->FindOrAddSettingByClass(UMoviePipelineAntiAliasingSetting::StaticClass()));
		AA->TemporalSampleCount = FMath::Max(Options.TemporalSamples, 1);
		AA->SpatialSampleCount = FMath::Max(Options.SpatialSamples, 1);
	}

	UMoviePipelineExecutorBase* Executor = Subsystem->RenderQueueWithExecutor(UMoviePipelinePIEExecutor::StaticClass());
	if (!Executor)
	{
		OutError = LOCTEXT("RenderStartFailed", "Movie Render Queue refused to start the render — check the Output Log.");
		return false;
	}

	if (OnFinished)
	{
		Executor->OnExecutorFinished().AddLambda(
			[OnFinished](UMoviePipelineExecutorBase* /*InExecutor*/, bool bSuccess)
			{
				OnFinished(bSuccess);
			});
	}

	UE_LOG(LogCineDirectorRender, Log, TEXT("Render started: sequence '%s', map '%s', %dx%d, temporal %d / spatial %d, out '%s'"),
		*Sequence->GetName(), *WorldPackageName,
		Options.Resolution.X, Options.Resolution.Y,
		Options.TemporalSamples, Options.SpatialSamples,
		Options.OutputDirectory.IsEmpty() ? TEXT("<default Saved/MovieRenders>") : *Options.OutputDirectory);

	return true;
}

#undef LOCTEXT_NAMESPACE
