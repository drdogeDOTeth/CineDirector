// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShotPlanTypes.h"

/** Outcome of executing a plan, for display in the panel. */
struct FCineExecuteResult
{
	bool bSuccess = false;
	FText Error;

	/** Per-shot summaries and parser/executor warnings, in timeline order. */
	TArray<FString> Notes;

	int32 NumShots = 0;
	double TotalDurationSeconds = 0.0;
};

/**
 * Turns an FCineShotPlan into real editor state: spawns one ACineCameraActor per
 * segment, binds it into the Level Sequence currently open in Sequencer, and
 * authors transform keys, focal-length / focus-distance tracks and camera cuts.
 *
 * New shots are appended after the last existing camera cut, so running twice
 * extends the sequence instead of stomping it. Everything happens inside one
 * scoped transaction and is undoable with a single Ctrl+Z.
 */
class FShotPlanExecutor
{
public:
	/** Snapshot the current editor level (actor labels, bounds, viewport camera) for a provider. */
	static FCineSceneContext BuildSceneContext();

	static FCineExecuteResult Execute(const FCineShotPlan& Plan);
};
