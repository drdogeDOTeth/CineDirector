// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineBodyRig.h"
#include "CineBodyTypes.h"
#include "CoreMinimal.h"

/**
 * Compiles a parsed FCineBodySpec into a keyframed, humanized FCineBodyAnimDef:
 * sit/stand base poses, smoke drag cycles with hand-to-mouth IK, look-around
 * sweeps, mood-driven tempo/slouch/tick habits, and ambient breathing/sway.
 */
class FCineBodyAuthor
{
public:
	static FCineBodyAnimDef Build(const FCineBodyRig& Rig, const FCineBodySpec& Spec);

	/** Asset-name slug, e.g. "Body_Sit_Smoke_Look_Nervous". */
	static FString MakeSlug(const FCineBodySpec& Spec);
};
