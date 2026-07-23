// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CineFaceTypes.h"
#include "CoreMinimal.h"

class USkeletalMesh;

/**
 * Inspects a skeletal mesh's morph targets (or MetaHuman control curves) and
 * maps them onto CineDirector's canonical face slots. Understands ARKit
 * blendshape names, Oculus/CC visemes, MetaHuman CTRL_expressions curves, and
 * falls back to keyword fuzzy-matching for arbitrary morph names.
 */
class FCineFaceAnalyzer
{
public:
	/**
	 * @param bPreferLayeredArkitMouth  When true on dual VRM+ARKit meshes (voids),
	 *   keep ARKit mouth micros and disable exclusive A/I/U/O so jaw co-articulation
	 *   and MetaHuman-style layering run. Default false = safer exclusive vowels.
	 */
	static FCineFaceProfile Analyze(USkeletalMesh* Mesh, bool bPreferLayeredArkitMouth = false);

	/** One-line "Mapped 9/15 slots (...)" summary for the panel status. */
	static FString DescribeProfile(const FCineFaceProfile& Profile);
};
