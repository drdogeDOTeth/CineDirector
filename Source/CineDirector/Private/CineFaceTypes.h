// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Canonical facial slots CineDirector animates. A mesh's actual morph targets
 * (or a MetaHuman's control curves) are mapped onto these by the analyzer, so
 * lipsync and emotions are authored once against slots and work on any face.
 */
enum class ECineFaceSlot : uint8
{
	JawOpen,
	MouthClose,      // lips pressed shut over the jaw (M/B/P)
	MouthWide,       // stretched, EE/IH shapes and smiles' width
	MouthPucker,     // rounded OO/UW
	MouthFunnel,     // open-rounded OH
	MouthSmile,
	MouthFrown,
	MouthPress,      // lips pressed together, tension
	MouthUpperUp,    // upper lip raise — upper-teeth reveal on open/wide vowels
	MouthLowerDown,  // lower lip depress — lower-teeth reveal
	NoseSneer,
	BrowUp,          // outer/general brow raise
	BrowDown,        // knit / anger
	BrowSad,         // inner brows up (worry, sadness)
	EyeBlink,
	EyeWide,
	EyeSquint,

	// Gaze — look-direction morphs (ARKit eyeLook*, VRM LookLeft/Right/Up/Down).
	EyeLookLeft,
	EyeLookRight,
	EyeLookUp,
	EyeLookDown,

	// Full-face expression morphs (VRM Joy/Angry/Sorrow/Surprised, etc.).
	// Driven as complete poses so anime-style faces read clearly.
	ExprHappy,
	ExprAngry,
	ExprSad,
	ExprSurprised,

	Count
};

/** Human-readable slot name (for status text and logs). */
const TCHAR* CineFaceSlotName(ECineFaceSlot Slot);

/** One target curve driven by a slot. A slot may drive several (left/right pairs). */
struct FCineFaceCurveTarget
{
	FName CurveName;
	float Scale = 1.0f;
};

/** How one mesh's face maps onto the canonical slots. */
struct FCineFaceProfile
{
	FString MeshName;

	/** MetaHuman face detected: curves are CTRL_expressions_* rig-logic controls, not morphs. */
	bool bMetaHuman = false;

	/**
	 * Mesh uses mutually-exclusive vowel morphs (VRM/MMD A/I/U/E/O). When set,
	 * lipsync picks one dominant mouth shape per frame instead of layering ARKit-style.
	 */
	bool bExclusiveVisemes = false;

	/** Per-slot curve targets; an empty array means the slot is unmapped on this mesh. */
	TArray<FCineFaceCurveTarget> Slots[(int32)ECineFaceSlot::Count];

	/** Analyzer commentary: what matched, what didn't. */
	TArray<FString> Notes;

	bool HasSlot(ECineFaceSlot Slot) const { return Slots[(int32)Slot].Num() > 0; }

	int32 NumMappedSlots() const
	{
		int32 Count = 0;
		for (int32 i = 0; i < (int32)ECineFaceSlot::Count; ++i)
		{
			Count += Slots[i].Num() > 0 ? 1 : 0;
		}
		return Count;
	}
};

/** One 30fps frame of mouth shape weights derived from audio (or synthesized). */
struct FCineVisemeFrame
{
	float Jaw = 0.0f;
	float Wide = 0.0f;
	float Pucker = 0.0f;    // rounded OO/UW
	float Funnel = 0.0f;    // open-round OH (VRM "O")
	float Close = 0.0f;     // consonant closure (M/B/P)
	float Sibilant = 0.0f;  // S/SH hiss — teeth together, slightly wide
};
