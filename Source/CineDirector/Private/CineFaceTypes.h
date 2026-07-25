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
	VisemeFV,        // labiodental fricatives (F/V)
	VisemeL,         // tongue-up alveolar (L)
	VisemeTH,        // dental fricative (TH)
	VisemeCH,        // affricates (CH/J/SH)
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

/** Character-specific face calibration, persisted per skeletal mesh. */
struct FCineFaceCalibration
{
	float JawGain = 1.0f;
	float JawOffset = 0.0f;
	float StretchGain = 1.0f;
	float StretchOffset = 0.0f;
	float SmileGain = 1.0f;
	float SmileOffset = 0.0f;
	float PuckerGain = 1.0f;
	float PuckerOffset = 0.0f;
	float LowerLipGain = 1.0f;
	float LowerLipOffset = 0.0f;
	float BrowGain = 1.0f;
	float BrowOffset = 0.0f;

	float GainForSlot(ECineFaceSlot Slot) const
	{
		switch (Slot)
		{
		case ECineFaceSlot::JawOpen: return JawGain;
		case ECineFaceSlot::MouthWide: return StretchGain;
		case ECineFaceSlot::MouthSmile: return SmileGain;
		case ECineFaceSlot::MouthPucker:
		case ECineFaceSlot::MouthFunnel: return PuckerGain;
		case ECineFaceSlot::MouthLowerDown: return LowerLipGain;
		case ECineFaceSlot::BrowUp:
		case ECineFaceSlot::BrowDown:
		case ECineFaceSlot::BrowSad: return BrowGain;
		default: return 1.0f;
		}
	}

	float OffsetForSlot(ECineFaceSlot Slot) const
	{
		switch (Slot)
		{
		case ECineFaceSlot::JawOpen: return JawOffset;
		case ECineFaceSlot::MouthWide: return StretchOffset;
		case ECineFaceSlot::MouthSmile: return SmileOffset;
		case ECineFaceSlot::MouthPucker:
		case ECineFaceSlot::MouthFunnel: return PuckerOffset;
		case ECineFaceSlot::MouthLowerDown: return LowerLipOffset;
		case ECineFaceSlot::BrowUp:
		case ECineFaceSlot::BrowDown:
		case ECineFaceSlot::BrowSad: return BrowOffset;
		default: return 0.0f;
		}
	}
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

	/**
	 * Mesh has a rich layered face set (ARKit-style jaw/pucker/smile/brows, etc.).
	 * Void FBX exports often ship ARKit + VRM vowels together — when this is set,
	 * the analyzer prefers ARKit targets and the baker eases emotion weights so
	 * lips/brows don't stretch from double-driving.
	 */
	bool bLayeredBlendshapes = false;

	/**
	 * The user explicitly selected the ARKit mouth path on a dual VRM+ARKit
	 * face. This is narrower than bLayeredBlendshapes: it records which mouth
	 * representation the analyzer selected so the baker can tune it safely.
	 */
	bool bLayeredArkitMouth = false;

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
	float FV = 0.0f;
	float L = 0.0f;
	float TH = 0.0f;
	float CH = 0.0f;
	float Confidence = 0.0f;
};

/** Continuous, confidence-weighted audio emotion at one analysis frame. */
struct FCineEmotionFrame
{
	float Happy = 0.0f;
	float Angry = 0.0f;
	float Sad = 0.0f;
	float Surprised = 0.0f;
	float Scared = 0.0f;
	float Disgusted = 0.0f;
	float Pain = 0.0f;
	float Suspicious = 0.0f;
	float Confidence = 0.0f;
};
