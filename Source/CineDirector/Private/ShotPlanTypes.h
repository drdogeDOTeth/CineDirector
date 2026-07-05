// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;

/** Primary camera movement over the duration of a shot segment. */
enum class ECineMoveType : uint8
{
	Static,
	DollyIn,
	DollyOut,
	OrbitCW,
	OrbitCCW,
	TruckLeft,
	TruckRight,
	CraneUp,
	CraneDown,
	PanLeft,
	PanRight,
	TiltUp,
	TiltDown,
	ZoomIn,
	ZoomOut,
	Flyover,
};

/** How tightly the subject is framed. Drives camera distance from the target's bounds. */
enum class ECineShotSize : uint8
{
	Unspecified,
	ExtremeCloseUp,
	CloseUp,
	MediumCloseUp,
	Medium,
	Wide,
	ExtremeWide,
};

/** Vertical placement relative to the subject. */
enum class ECineAngle : uint8
{
	EyeLevel,
	Low,
	High,
	Overhead,
};

/** Which side of the subject the camera starts on, relative to the subject's facing. */
enum class ECineViewSide : uint8
{
	Front,
	Behind,
	Left,
	Right,
	OverShoulder,
};

enum class ECineEasing : uint8
{
	EaseInOut,
	Linear,
};

/** One camera + one move. A description like "close-up, then orbit" yields two of these. */
struct FCineShotSegment
{
	/** The clause of the description this segment was parsed from. */
	FString RawText;

	/** Label for the spawned camera actor ("CineDirector Shot 1"). Filled by the executor if empty. */
	FString CameraLabel;

	/** Resolved subject of the shot; may be null for target-less shots (placed at the viewport camera). */
	TWeakObjectPtr<AActor> TargetActor;
	FString TargetLabel;

	/** Second target for rack focus ("rack focus from the knight to the door"). */
	TWeakObjectPtr<AActor> RackFocusToActor;
	FString RackFocusToLabel;

	ECineMoveType Move = ECineMoveType::Static;
	ECineShotSize ShotSize = ECineShotSize::Unspecified;
	ECineAngle Angle = ECineAngle::EyeLevel;
	ECineViewSide ViewSide = ECineViewSide::Front;
	ECineEasing Easing = ECineEasing::EaseInOut;

	double DurationSeconds = 5.0;

	/**
	 * Magnitude of the move. Meaning depends on Move:
	 *  - Orbit / Pan / Tilt: degrees (90 = quarter orbit)
	 *  - Dolly / Truck / Crane / Flyover: centimeters (0 = pick a sensible default)
	 *  - Zoom: target focal length in mm (0 = double / halve the current focal length)
	 */
	double MoveAmount = 0.0;

	/** 0 = use default (35mm). */
	float FocalLengthMm = 0.0f;

	/** 0 = use default (f/2.8). */
	float Aperture = 0.0f;

	/** Keep autofocus locked on the target actor for the whole shot. */
	bool bTrackFocus = false;

	/** Aim the camera at the target throughout the move (orbits/trucks stay framed). */
	bool bLookAtTarget = true;

	/** 0 = locked off. ~0.4 subtle, ~0.8 handheld, ~1.5 very shaky. Baked as transform noise keys. */
	float HandheldIntensity = 0.0f;

	/** Camera roll in degrees for dutch/canted angles. */
	float DutchAngleDeg = 0.0f;

	/** Post-process overrides applied to the shot's camera. 0 = leave the camera default. */
	float FilmGrainIntensity = 0.0f;
	float VignetteIntensity = 0.0f;
	float ChromaticAberrationIntensity = 0.0f;
	float BloomIntensity = 0.0f;
	float LensFlareIntensity = 0.0f;

	/** Notes the parser wants surfaced to the user (ignored words, assumptions made). */
	TArray<FString> ParseNotes;
};

/** The full result of interpreting one description. */
struct FCineShotPlan
{
	TArray<FCineShotSegment> Segments;

	/** Create a camera-cut section per segment so the sequence plays shot to shot. */
	bool bCreateCameraCuts = true;

	/**
	 * Chain every segment onto ONE camera as a single unbroken take: each move
	 * starts where the previous one ended, with no cuts in between.
	 */
	bool bOneContinuousShot = false;
};

/** Snapshot of one level actor the parser can match target names against. */
struct FCineSceneActorInfo
{
	TWeakObjectPtr<AActor> Actor;
	FString Label;
	FVector Location = FVector::ZeroVector;
	/** Radius of the actor's bounds sphere, cm. Used to compute framing distances. */
	double BoundsRadius = 100.0;
	FRotator Facing = FRotator::ZeroRotator;
};

/** Everything a shot-plan provider is allowed to know about the level. */
struct FCineSceneContext
{
	TArray<FCineSceneActorInfo> Actors;

	/** Editor viewport camera, used as the anchor for target-less shots. */
	FVector ViewportLocation = FVector::ZeroVector;
	FRotator ViewportRotation = FRotator::ZeroRotator;

	const FCineSceneActorInfo* FindByActor(const AActor* InActor) const
	{
		for (const FCineSceneActorInfo& Info : Actors)
		{
			if (Info.Actor.Get() == InActor)
			{
				return &Info;
			}
		}
		return nullptr;
	}
};
