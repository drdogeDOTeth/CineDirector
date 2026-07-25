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

/**
 * Scene lighting mood for a shot, applied to the level's sun (directional light).
 * Overcast is weather rather than a time: it flattens the light but keeps the sun angle.
 */
enum class ECineTimeOfDay : uint8
{
	Unchanged,
	Dawn,
	Morning,
	Noon,
	Afternoon,
	GoldenHour,
	Sunset,
	Dusk,
	Night,
	Midnight,
	Overcast,
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

	/**
	 * Explicit aim target ("orbit around the tower LOOKING AT the knight").
	 * When set, the lens points here for the whole move even though the move
	 * itself pivots on / starts from TargetActor.
	 */
	TWeakObjectPtr<AActor> LookAtActor;
	FString LookAtLabel;

	ECineMoveType Move = ECineMoveType::Static;
	ECineShotSize ShotSize = ECineShotSize::Unspecified;
	ECineAngle Angle = ECineAngle::EyeLevel;
	ECineViewSide ViewSide = ECineViewSide::Front;

	/**
	 * True when the side was phrased possessively ("its left", "their back") and is
	 * relative to the actor's own root rotation; false means viewport-relative
	 * ("from the left" = screen left as the user currently sees the actor).
	 */
	bool bActorRelativeSide = false;

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

	/**
	 * Keep depth of field locked on the look-at / target actor.
	 * Defaulted on by the parser whenever a subject is present (unless deep/fixed focus).
	 */
	bool bTrackFocus = false;

	/**
	 * Deep / near-infinite focus. Skips subject tracking and holds a long manual
	 * focus distance so the whole stage stays sharp.
	 */
	bool bDeepFocus = false;

	/**
	 * Hold one fixed manual focus distance from setup (does not re-pull as the
	 * camera moves or the cut lands on a new subject). Explicit "fixed focus".
	 */
	bool bFixedFocus = false;

	/** Aim the camera at the target throughout the move (orbits/trucks stay framed). */
	bool bLookAtTarget = true;

	/**
	 * Explicit "follow / track / lock on" language: keep the camera's offset to the
	 * subject in the subject's space and re-bake position every sample so the cam
	 * travels with them (not just re-aiming at a fixed world point).
	 */
	bool bFollowSubjectPosition = false;

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

	/**
	 * Color grade / look pack from style words ("horror", "Nolan", "bodycam", …).
	 * Only applied when bApplyLookGrade is true so clean shots stay untouched.
	 */
	bool bApplyLookGrade = false;
	/** Global saturation multiplier (1 = neutral, <1 desat, >1 punchy). */
	float LookSaturation = 1.0f;
	/** Global contrast multiplier (1 = neutral). */
	float LookContrast = 1.0f;
	/** Global gain / exposure-ish lift (1 = neutral, 0.9 darker, 1.1 brighter). */
	float LookGain = 1.0f;
	/** White balance Kelvin; 0 = leave camera default (~6500). */
	float WhiteTempKelvin = 0.0f;
	/** Magenta↔green white tint (-1..1 style; 0 = none). */
	float WhiteTint = 0.0f;
	/** Multiplies scene color (tints the whole plate). White = no tint. */
	FLinearColor SceneColorTint = FLinearColor::White;
	/** Motion blur amount 0..1; negative = leave default. */
	float MotionBlurAmount = -1.0f;

	/**
	 * Optional filmback override for scope / IMAX-ish framing.
	 * SensorHeight 0 = leave the cine camera default filmback.
	 */
	float FilmbackSensorWidthMm = 0.0f;
	float FilmbackSensorHeightMm = 0.0f;

	/** Human label for the style kit applied (shown in shot notes). */
	FString StyleKitName;

	/** Sun/sky mood for this shot, keyed on the level's directional light per cut. */
	ECineTimeOfDay TimeOfDay = ECineTimeOfDay::Unchanged;

	/** Height-fog density for this shot. Negative = leave the level's fog untouched. */
	float FogDensity = -1.0f;

	/** Enable light-shaft bloom (god rays) on the sun. Set once, not keyed. */
	bool bGodRays = false;

	/** Enable volumetric fog on the height fog actor. Set once, not keyed. */
	bool bVolumetricFog = false;

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
