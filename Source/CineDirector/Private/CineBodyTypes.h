// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Mood presets recognized by the body grammar. */
enum class ECineBodyMood : uint8
{
	Neutral,
	Nervous, // jittery: faster tempo, ticks on, snappier joints
	Chill,   // slower tempo, deeper slouch, softer overshoot
	Alert,   // upright, wide fast scans, minimal idle motion
};

/**
 * A parsed plain-language body performance request, e.g.
 * "sitting and smoking nervously, looking around, for 20 seconds".
 * Produced by FCineBodyGrammar; consumed by FCineBodyAuthor.
 * A future Claude-backed provider can emit this (or full keyframe specs)
 * without touching the author — same split as IShotPlanProvider.
 */
struct FCineBodySpec
{
	bool bSitting = false;     // default is standing
	bool bSmoke = false;
	bool bLookAround = false;

	ECineBodyMood Mood = ECineBodyMood::Neutral;
	/** "very"/"slightly" on the mood word. 1 = plain. */
	float MoodIntensity = 1.0f;

	/** 0 = pick a sensible default from the activities. */
	float DurationSeconds = 0.0f;

	/** Varies procedural phases so two characters never sync up. */
	int32 Seed = 0;

	/** Human-readable interpretation for the status line / log. */
	FString Describe() const;
};

/** The humanization dials a mood maps onto. */
struct FCineBodyMoodDials
{
	float Tempo = 1.0f;         // >1 = everything happens faster
	float Ambient = 1.0f;       // breathing/sway/head-drift amplitude
	float LagMul = 1.0f;        // scales per-joint follow-through lag
	float OvershootMul = 1.0f;  // scales per-joint overshoot
	float SlouchDeg = 0.0f;     // added to the spine/chest forward curl
	float LookYawMul = 1.0f;    // scales look-around sweep width
	bool bFootTap = false;
	bool bKneeBounce = false;

	static FCineBodyMoodDials For(ECineBodyMood Mood, float Intensity);
};
