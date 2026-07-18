// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** What the user picked in the panel's Render section. */
struct FCineRenderOptions
{
	FIntPoint Resolution = FIntPoint(1920, 1080);

	enum class EFormat : uint8
	{
		PNG,
		JPEG,
		EXR,
		BMP,
		MP4,
	};
	EFormat Format = EFormat::PNG;

	/** MP4 only: encoder quality tier, 0 (low) .. 3 (epic). */
	int32 EncodeQuality = 2;

	/**
	 * Anti-aliasing accumulation. 1/1 = draft (engine AA only). Higher temporal
	 * counts add true sub-frame motion blur at the cost of render time.
	 */
	int32 TemporalSamples = 1;
	int32 SpatialSamples = 1;

	/** Empty = Movie Render Queue's default ({project}/Saved/MovieRenders). */
	FString OutputDirectory;
};

/**
 * Queues the Level Sequence currently open in Sequencer into Movie Render Queue
 * and starts an in-editor (PIE) render with the given options.
 */
class FCineRenderLauncher
{
public:
	/**
	 * @param OnFinished Called on the game thread when the render completes (bSuccess).
	 * @return false with OutError set if the render could not start.
	 */
	static bool StartRender(const FCineRenderOptions& Options, FText& OutError, TFunction<void(bool)> OnFinished = nullptr);

	/** Locate ffmpeg: project settings, then PATH, then the WinGet install layout. Empty if missing. */
	static FString FindFFmpegExecutable();
};
