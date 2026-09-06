// Copyright Roundtree. All Rights Reserved.

#include "ShotPlanJson.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "CineDirector"

namespace CineShotPlanJson
{
	/**
	 * Enum <-> wire-token tables. The tokens are the enum values in the schema,
	 * so adding a move means adding it here and in the schema's enum list.
	 */
	template <typename EnumType>
	struct TToken
	{
		const TCHAR* Token;
		EnumType Value;
	};

	static const TToken<ECineMoveType> MoveTokens[] = {
		{ TEXT("static"),      ECineMoveType::Static },
		{ TEXT("dolly_in"),    ECineMoveType::DollyIn },
		{ TEXT("dolly_out"),   ECineMoveType::DollyOut },
		{ TEXT("orbit_cw"),    ECineMoveType::OrbitCW },
		{ TEXT("orbit_ccw"),   ECineMoveType::OrbitCCW },
		{ TEXT("truck_left"),  ECineMoveType::TruckLeft },
		{ TEXT("truck_right"), ECineMoveType::TruckRight },
		{ TEXT("crane_up"),    ECineMoveType::CraneUp },
		{ TEXT("crane_down"),  ECineMoveType::CraneDown },
		{ TEXT("pan_left"),    ECineMoveType::PanLeft },
		{ TEXT("pan_right"),   ECineMoveType::PanRight },
		{ TEXT("tilt_up"),     ECineMoveType::TiltUp },
		{ TEXT("tilt_down"),   ECineMoveType::TiltDown },
		{ TEXT("zoom_in"),     ECineMoveType::ZoomIn },
		{ TEXT("zoom_out"),    ECineMoveType::ZoomOut },
		{ TEXT("flyover"),     ECineMoveType::Flyover },
	};

	static const TToken<ECineShotSize> SizeTokens[] = {
		{ TEXT("unspecified"),       ECineShotSize::Unspecified },
		{ TEXT("extreme_close_up"),  ECineShotSize::ExtremeCloseUp },
		{ TEXT("close_up"),          ECineShotSize::CloseUp },
		{ TEXT("medium_close_up"),   ECineShotSize::MediumCloseUp },
		{ TEXT("medium"),            ECineShotSize::Medium },
		{ TEXT("wide"),              ECineShotSize::Wide },
		{ TEXT("extreme_wide"),      ECineShotSize::ExtremeWide },
	};

	static const TToken<ECineAngle> AngleTokens[] = {
		{ TEXT("eye_level"), ECineAngle::EyeLevel },
		{ TEXT("low"),       ECineAngle::Low },
		{ TEXT("high"),      ECineAngle::High },
		{ TEXT("overhead"),  ECineAngle::Overhead },
	};

	static const TToken<ECineViewSide> SideTokens[] = {
		{ TEXT("front"),         ECineViewSide::Front },
		{ TEXT("behind"),        ECineViewSide::Behind },
		{ TEXT("left"),          ECineViewSide::Left },
		{ TEXT("right"),         ECineViewSide::Right },
		{ TEXT("over_shoulder"), ECineViewSide::OverShoulder },
	};

	static const TToken<ECineEasing> EasingTokens[] = {
		{ TEXT("ease_in_out"), ECineEasing::EaseInOut },
		{ TEXT("linear"),      ECineEasing::Linear },
	};

	static const TToken<ECineTimeOfDay> TimeTokens[] = {
		{ TEXT("unchanged"),    ECineTimeOfDay::Unchanged },
		{ TEXT("dawn"),         ECineTimeOfDay::Dawn },
		{ TEXT("morning"),      ECineTimeOfDay::Morning },
		{ TEXT("noon"),         ECineTimeOfDay::Noon },
		{ TEXT("afternoon"),    ECineTimeOfDay::Afternoon },
		{ TEXT("golden_hour"),  ECineTimeOfDay::GoldenHour },
		{ TEXT("sunset"),       ECineTimeOfDay::Sunset },
		{ TEXT("dusk"),         ECineTimeOfDay::Dusk },
		{ TEXT("night"),        ECineTimeOfDay::Night },
		{ TEXT("midnight"),     ECineTimeOfDay::Midnight },
		{ TEXT("overcast"),     ECineTimeOfDay::Overcast },
	};

	template <typename EnumType, int32 N>
	EnumType ParseToken(const FString& Token, const TToken<EnumType>(&Table)[N], EnumType Fallback)
	{
		for (const TToken<EnumType>& Entry : Table)
		{
			if (Token.Equals(Entry.Token, ESearchCase::IgnoreCase))
			{
				return Entry.Value;
			}
		}
		return Fallback;
	}

	/** Reverse lookup, for describing a plan back in the wire vocabulary. */
	template <typename EnumType, int32 N>
	const TCHAR* TokenFor(EnumType Value, const TToken<EnumType>(&Table)[N])
	{
		for (const TToken<EnumType>& Entry : Table)
		{
			if (Entry.Value == Value)
			{
				return Entry.Token;
			}
		}
		return TEXT("?");
	}

	/** Field accessors that tolerate a missing or wrong-typed value. */
	FString GetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Value;
		return Obj->TryGetStringField(Field, Value) ? Value.TrimStartAndEnd() : FString();
	}

	double GetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, double Default)
	{
		double Value = 0.0;
		return Obj->TryGetNumberField(Field, Value) ? Value : Default;
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, bool Default)
	{
		bool Value = false;
		return Obj->TryGetBoolField(Field, Value) ? Value : Default;
	}

	/**
	 * Match a label the model wrote against the level. Exact (case-insensitive)
	 * wins; then either string containing the other, longest match first, which
	 * handles "the Knight" vs "Knight_BP_2".
	 */
	const FCineSceneActorInfo* ResolveActor(const FString& Label, const FCineSceneContext& Scene)
	{
		if (Label.IsEmpty())
		{
			return nullptr;
		}

		for (const FCineSceneActorInfo& Info : Scene.Actors)
		{
			if (Info.Label.Equals(Label, ESearchCase::IgnoreCase))
			{
				return &Info;
			}
		}

		const FCineSceneActorInfo* Best = nullptr;
		int32 BestLength = 0;
		for (const FCineSceneActorInfo& Info : Scene.Actors)
		{
			const bool bOverlaps =
				Info.Label.Contains(Label, ESearchCase::IgnoreCase) ||
				Label.Contains(Info.Label, ESearchCase::IgnoreCase);

			if (bOverlaps && Info.Label.Len() > BestLength)
			{
				Best = &Info;
				BestLength = Info.Label.Len();
			}
		}
		return Best;
	}

	/** Resolve one label field, noting on the segment when the model named something absent. */
	void BindActor(const FString& Label, const FCineSceneContext& Scene, const TCHAR* Role,
		TWeakObjectPtr<AActor>& OutActor, FString& OutLabel, TArray<FString>& Notes)
	{
		if (Label.IsEmpty())
		{
			return;
		}

		if (const FCineSceneActorInfo* Info = ResolveActor(Label, Scene))
		{
			OutActor = Info->Actor;
			OutLabel = Info->Label;
		}
		else
		{
			Notes.Add(FString::Printf(
				TEXT("No actor named \"%s\" in the level — %s ignored."), *Label, Role));
		}
	}
}

FString FCineShotPlanJson::BuildSchema()
{
	// Strict schema: every property required, additionalProperties false. Neutral
	// values stand in for "not applicable" so nothing has to be nullable.
	return TEXT(R"JSON({
  "type": "object",
  "additionalProperties": false,
  "required": ["one_continuous_shot", "create_camera_cuts", "segments"],
  "properties": {
    "one_continuous_shot": {
      "type": "boolean",
      "description": "True only for an explicit single unbroken take: every segment chains onto one camera with no cuts."
    },
    "create_camera_cuts": {
      "type": "boolean",
      "description": "Normally true so the sequence plays shot to shot. False leaves the cameras uncut."
    },
    "segments": {
      "type": "array",
      "description": "One entry per shot, in playback order.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": [
          "raw_text", "target", "look_at", "rack_focus_to", "move", "move_amount",
          "shot_size", "angle", "view_side", "actor_relative_side", "easing",
          "duration_seconds", "focal_length_mm", "aperture", "track_focus",
          "deep_focus", "fixed_focus", "look_at_target", "follow_subject",
          "handheld_intensity", "dutch_angle_deg", "film_grain", "vignette",
          "chromatic_aberration", "bloom", "lens_flare", "look", "style_kit_name",
          "time_of_day", "fog_density", "god_rays", "volumetric_fog", "notes"
        ],
        "properties": {
          "raw_text": { "type": "string", "description": "The clause of the request this shot came from." },
          "target": { "type": "string", "description": "Exact label of the subject actor, copied from the scene list. Empty means no subject: the shot is framed from the current viewport camera." },
          "look_at": { "type": "string", "description": "Actor the lens stays aimed at when it differs from target (orbit around the tower looking at the knight). Empty otherwise." },
          "rack_focus_to": { "type": "string", "description": "Second actor for a rack focus pull. Empty otherwise." },
          "move": { "type": "string", "enum": ["static", "dolly_in", "dolly_out", "orbit_cw", "orbit_ccw", "truck_left", "truck_right", "crane_up", "crane_down", "pan_left", "pan_right", "tilt_up", "tilt_down", "zoom_in", "zoom_out", "flyover"] },
          "move_amount": { "type": "number", "description": "0 picks a sensible default. Orbit/pan/tilt: degrees. Dolly/truck/crane/flyover: centimetres. Zoom: target focal length in mm." },
          "shot_size": { "type": "string", "enum": ["unspecified", "extreme_close_up", "close_up", "medium_close_up", "medium", "wide", "extreme_wide"] },
          "angle": { "type": "string", "enum": ["eye_level", "low", "high", "overhead"] },
          "view_side": { "type": "string", "enum": ["front", "behind", "left", "right", "over_shoulder"] },
          "actor_relative_side": { "type": "boolean", "description": "True when the side was phrased possessively (its left, their back); false means screen-relative as the viewport sees it now." },
          "easing": { "type": "string", "enum": ["ease_in_out", "linear"] },
          "duration_seconds": { "type": "number", "description": "Shot length. Default 5 when unstated; 2-3 for cuts in an action beat, 8-12 for a slow establishing move." },
          "focal_length_mm": { "type": "number", "description": "0 uses the 35mm default. 18-24 wide, 35 neutral, 85 portrait, 135+ telephoto." },
          "aperture": { "type": "number", "description": "f-number. 0 uses f/2.8. 1.4-2 shallow, 8-16 deep." },
          "track_focus": { "type": "boolean", "description": "Keep autofocus locked on the subject. True whenever there is a subject and focus is not deep or fixed." },
          "deep_focus": { "type": "boolean", "description": "Hold near-infinite focus so the whole stage stays sharp." },
          "fixed_focus": { "type": "boolean", "description": "Lock one manual focus distance from setup and never re-pull." },
          "look_at_target": { "type": "boolean", "description": "Aim the camera at the subject for the whole move. Usually true when there is a subject." },
          "follow_subject": { "type": "boolean", "description": "Travel with a moving subject, keeping the camera offset in the subject's space. Only for explicit follow/track/lock-on language." },
          "handheld_intensity": { "type": "number", "description": "0 locked off, 0.4 subtle, 0.8 handheld, 1.5 very shaky." },
          "dutch_angle_deg": { "type": "number", "description": "Camera roll in degrees for canted framing. 0 for level." },
          "film_grain": { "type": "number", "description": "0-1. 0 leaves the camera default." },
          "vignette": { "type": "number", "description": "0-1. 0 leaves the camera default." },
          "chromatic_aberration": { "type": "number", "description": "0-1. 0 leaves the camera default." },
          "bloom": { "type": "number", "description": "0-2. 0 leaves the camera default." },
          "lens_flare": { "type": "number", "description": "0-1. 0 leaves the camera default." },
          "look": {
            "type": "object",
            "additionalProperties": false,
            "required": ["apply_grade", "saturation", "contrast", "gain", "white_temp_kelvin", "white_tint", "motion_blur"],
            "description": "Colour grade. Leave apply_grade false and the neutral values unless the request asks for a look (horror, noir, bodycam, warm, bleak).",
            "properties": {
              "apply_grade": { "type": "boolean" },
              "saturation": { "type": "number", "description": "1 neutral, below 1 desaturated, above 1 punchy." },
              "contrast": { "type": "number", "description": "1 neutral." },
              "gain": { "type": "number", "description": "1 neutral, 0.9 darker, 1.1 brighter." },
              "white_temp_kelvin": { "type": "number", "description": "0 leaves the camera default (~6500). 3200 warm, 9000 cold." },
              "white_tint": { "type": "number", "description": "-1 magenta to 1 green, 0 none." },
              "motion_blur": { "type": "number", "description": "0-1, or -1 to leave the default." }
            }
          },
          "style_kit_name": { "type": "string", "description": "Short name for the look applied, shown in the shot notes. Empty when no style was asked for." },
          "time_of_day": { "type": "string", "enum": ["unchanged", "dawn", "morning", "noon", "afternoon", "golden_hour", "sunset", "dusk", "night", "midnight", "overcast"], "description": "Re-keys the level's sun. Use unchanged unless the request names a time or weather." },
          "fog_density": { "type": "number", "description": "-1 leaves the level fog alone. 0.02 light haze, 0.2 heavy." },
          "god_rays": { "type": "boolean" },
          "volumetric_fog": { "type": "boolean" },
          "notes": { "type": "array", "items": { "type": "string" }, "description": "Short notes on assumptions made or parts of the request that could not be honoured. Empty array when everything was straightforward." }
        }
      }
    }
  }
})JSON");
}

FString FCineShotPlanJson::BuildSystemPrompt()
{
	return TEXT(R"PROMPT(You are a cinematographer laying out camera coverage inside Unreal Engine's Sequencer. You turn a plain-language request into a shot plan that a deterministic executor spawns cine cameras from.

Rules:
- Return only the plan, matching the given schema exactly. No prose, no markdown fence.
- Split the request into shots the way an editor would: a new framing, a new subject, or a cut phrase ("then", "cut to", "next") starts a new segment. A single continuous move stays one segment.
- Only name actors that appear in the scene list, copying the label exactly. If the request names something that is not there, leave target empty and say so in notes.
- With no subject the shot is anchored to the current viewport camera, which is a legitimate choice for establishing and abstract moves.
- Motivate every choice: lens and framing follow the emotional beat, not a default. Close, long lenses and shallow focus for intimacy; wide lenses and deep focus for scale and unease; low angles for power; handheld for urgency.
- Timing is part of the grammar. A held wide breathes at 8-12s; a cut inside an action beat is 1.5-3s. Do not give every shot the same length.
- Leave a field at its neutral value when the request does not call for it. Unrequested grain, vignette, dutch angles and colour grades read as noise, not style.
- Set time_of_day, fog and god rays only when the request describes light or weather; they re-key the whole level.
- Keep notes short and only for real assumptions or things you could not do.)PROMPT");
}

FString FCineShotPlanJson::BuildUserPrompt(const FString& Description, const FCineSceneContext& Scene,
	bool bSendSceneActors, int32 MaxActors)
{
	FString Prompt = TEXT("Shot request:\n") + Description.TrimStartAndEnd() + TEXT("\n\n");

	Prompt += FString::Printf(
		TEXT("Viewport camera: position (%.0f, %.0f, %.0f), facing yaw %.0f, pitch %.0f.\n"),
		Scene.ViewportLocation.X, Scene.ViewportLocation.Y, Scene.ViewportLocation.Z,
		Scene.ViewportRotation.Yaw, Scene.ViewportRotation.Pitch);

	if (!bSendSceneActors || Scene.Actors.Num() == 0)
	{
		Prompt += TEXT("\nScene actors: none available — frame every shot from the viewport camera and leave target empty.\n");
		return Prompt;
	}

	// Nearest actors first: with a cap, the ones around the viewport are the ones
	// the director is most likely talking about.
	TArray<const FCineSceneActorInfo*> Sorted;
	Sorted.Reserve(Scene.Actors.Num());
	for (const FCineSceneActorInfo& Info : Scene.Actors)
	{
		Sorted.Add(&Info);
	}

	const FVector Eye = Scene.ViewportLocation;
	Sorted.Sort([Eye](const FCineSceneActorInfo& A, const FCineSceneActorInfo& B)
	{
		return FVector::DistSquared(A.Location, Eye) < FVector::DistSquared(B.Location, Eye);
	});

	const int32 Count = FMath::Min(Sorted.Num(), FMath::Max(1, MaxActors));
	Prompt += FString::Printf(TEXT("\nScene actors (%d of %d, nearest the viewport first). "
		"Sizes are the bounds radius in cm; use them to judge framing distance:\n"),
		Count, Sorted.Num());

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FCineSceneActorInfo& Info = *Sorted[Index];
		Prompt += FString::Printf(
			TEXT("- \"%s\" at (%.0f, %.0f, %.0f), size %.0f, facing yaw %.0f\n"),
			*Info.Label, Info.Location.X, Info.Location.Y, Info.Location.Z,
			Info.BoundsRadius, Info.Facing.Yaw);
	}

	if (Count < Sorted.Num())
	{
		Prompt += FString::Printf(TEXT("(%d further actors omitted.)\n"), Sorted.Num() - Count);
	}

	return Prompt;
}

bool FCineShotPlanJson::ExtractJsonObject(const FString& Text, FString& OutJson)
{
	int32 Start = INDEX_NONE;
	if (!Text.FindChar(TEXT('{'), Start))
	{
		return false;
	}

	// Walk to the matching brace, skipping over string literals so a "{" inside
	// raw_text or a note does not throw the depth count off.
	int32 Depth = 0;
	bool bInString = false;
	bool bEscaped = false;

	for (int32 Index = Start; Index < Text.Len(); ++Index)
	{
		const TCHAR Char = Text[Index];

		if (bInString)
		{
			if (bEscaped)          { bEscaped = false; }
			else if (Char == TEXT('\\')) { bEscaped = true; }
			else if (Char == TEXT('"'))  { bInString = false; }
			continue;
		}

		if (Char == TEXT('"'))
		{
			bInString = true;
		}
		else if (Char == TEXT('{'))
		{
			++Depth;
		}
		else if (Char == TEXT('}'))
		{
			if (--Depth == 0)
			{
				OutJson = Text.Mid(Start, Index - Start + 1);
				return true;
			}
		}
	}

	return false;
}

FString FCineShotPlanJson::DescribePlan(const FCineShotPlan& Plan)
{
	using namespace CineShotPlanJson;

	FString Out = FString::Printf(TEXT("%d shot%s%s"),
		Plan.Segments.Num(),
		Plan.Segments.Num() == 1 ? TEXT("") : TEXT("s"),
		Plan.bOneContinuousShot ? TEXT(", one continuous take") : TEXT(""));

	for (int32 Index = 0; Index < Plan.Segments.Num(); ++Index)
	{
		const FCineShotSegment& Segment = Plan.Segments[Index];

		Out += FString::Printf(TEXT("\n  %d. %s | %s | %s | %s | %.1fs"),
			Index + 1,
			Segment.TargetLabel.IsEmpty() ? TEXT("(viewport)") : *Segment.TargetLabel,
			TokenFor(Segment.Move, MoveTokens),
			TokenFor(Segment.ShotSize, SizeTokens),
			TokenFor(Segment.Angle, AngleTokens),
			Segment.DurationSeconds);

		if (Segment.FocalLengthMm > 0.0f)
		{
			Out += FString::Printf(TEXT(" | %.0fmm"), Segment.FocalLengthMm);
		}
		if (Segment.Aperture > 0.0f)
		{
			Out += FString::Printf(TEXT(" | f/%.1f"), Segment.Aperture);
		}
		if (Segment.HandheldIntensity > 0.0f)
		{
			Out += FString::Printf(TEXT(" | handheld %.2f"), Segment.HandheldIntensity);
		}
		if (Segment.TimeOfDay != ECineTimeOfDay::Unchanged)
		{
			Out += FString::Printf(TEXT(" | %s"), TokenFor(Segment.TimeOfDay, TimeTokens));
		}
		if (!Segment.StyleKitName.IsEmpty())
		{
			Out += FString::Printf(TEXT(" | %s"), *Segment.StyleKitName);
		}
		if (!Segment.RawText.IsEmpty())
		{
			Out += FString::Printf(TEXT("\n       \"%s\""), *Segment.RawText);
		}

		for (const FString& Note : Segment.ParseNotes)
		{
			Out += FString::Printf(TEXT("\n       note: %s"), *Note);
		}
	}

	return Out;
}

bool FCineShotPlanJson::ParsePlan(const FString& Reply, const FCineSceneContext& Scene,
	FCineShotPlan& OutPlan, FText& OutError)
{
	using namespace CineShotPlanJson;

	FString Json;
	if (!ExtractJsonObject(Reply, Json))
	{
		OutError = LOCTEXT("PlanNotJson", "The model did not return a shot plan (no JSON object in the reply).");
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = LOCTEXT("PlanBadJson", "The model's shot plan was not valid JSON.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
	if (!Root->TryGetArrayField(TEXT("segments"), Segments) || Segments->Num() == 0)
	{
		OutError = LOCTEXT("PlanNoSegments", "The model's shot plan contained no shots.");
		return false;
	}

	OutPlan.bOneContinuousShot = GetBool(Root, TEXT("one_continuous_shot"), false);
	OutPlan.bCreateCameraCuts = GetBool(Root, TEXT("create_camera_cuts"), true);
	OutPlan.Segments.Reset();

	for (const TSharedPtr<FJsonValue>& Value : *Segments)
	{
		const TSharedPtr<FJsonObject>* SegmentObjPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SegmentObjPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Obj = *SegmentObjPtr;

		FCineShotSegment Segment;
		Segment.RawText = GetString(Obj, TEXT("raw_text"));

		BindActor(GetString(Obj, TEXT("target")), Scene, TEXT("subject"),
			Segment.TargetActor, Segment.TargetLabel, Segment.ParseNotes);
		BindActor(GetString(Obj, TEXT("look_at")), Scene, TEXT("look-at target"),
			Segment.LookAtActor, Segment.LookAtLabel, Segment.ParseNotes);
		BindActor(GetString(Obj, TEXT("rack_focus_to")), Scene, TEXT("rack focus target"),
			Segment.RackFocusToActor, Segment.RackFocusToLabel, Segment.ParseNotes);

		Segment.Move = ParseToken(GetString(Obj, TEXT("move")), MoveTokens, ECineMoveType::Static);
		Segment.ShotSize = ParseToken(GetString(Obj, TEXT("shot_size")), SizeTokens, ECineShotSize::Unspecified);
		Segment.Angle = ParseToken(GetString(Obj, TEXT("angle")), AngleTokens, ECineAngle::EyeLevel);
		Segment.ViewSide = ParseToken(GetString(Obj, TEXT("view_side")), SideTokens, ECineViewSide::Front);
		Segment.Easing = ParseToken(GetString(Obj, TEXT("easing")), EasingTokens, ECineEasing::EaseInOut);
		Segment.TimeOfDay = ParseToken(GetString(Obj, TEXT("time_of_day")), TimeTokens, ECineTimeOfDay::Unchanged);

		Segment.bActorRelativeSide = GetBool(Obj, TEXT("actor_relative_side"), false);
		Segment.DurationSeconds = FMath::Clamp(GetNumber(Obj, TEXT("duration_seconds"), 5.0), 0.1, 600.0);
		Segment.MoveAmount = GetNumber(Obj, TEXT("move_amount"), 0.0);
		Segment.FocalLengthMm = FMath::Clamp((float)GetNumber(Obj, TEXT("focal_length_mm"), 0.0), 0.0f, 1000.0f);
		Segment.Aperture = FMath::Clamp((float)GetNumber(Obj, TEXT("aperture"), 0.0), 0.0f, 32.0f);

		Segment.bDeepFocus = GetBool(Obj, TEXT("deep_focus"), false);
		Segment.bFixedFocus = GetBool(Obj, TEXT("fixed_focus"), false);
		Segment.bLookAtTarget = GetBool(Obj, TEXT("look_at_target"), true);
		Segment.bFollowSubjectPosition = GetBool(Obj, TEXT("follow_subject"), false);

		// Focus tracking only means something with a subject, and is mutually
		// exclusive with the two manual-focus modes.
		const bool bHasSubject = Segment.TargetActor.IsValid() || Segment.LookAtActor.IsValid();
		Segment.bTrackFocus = GetBool(Obj, TEXT("track_focus"), bHasSubject)
			&& bHasSubject && !Segment.bDeepFocus && !Segment.bFixedFocus;

		Segment.HandheldIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("handheld_intensity"), 0.0), 0.0f, 5.0f);
		Segment.DutchAngleDeg = (float)GetNumber(Obj, TEXT("dutch_angle_deg"), 0.0);

		Segment.FilmGrainIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("film_grain"), 0.0), 0.0f, 1.0f);
		Segment.VignetteIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("vignette"), 0.0), 0.0f, 1.0f);
		Segment.ChromaticAberrationIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("chromatic_aberration"), 0.0), 0.0f, 5.0f);
		Segment.BloomIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("bloom"), 0.0), 0.0f, 8.0f);
		Segment.LensFlareIntensity = FMath::Clamp((float)GetNumber(Obj, TEXT("lens_flare"), 0.0), 0.0f, 8.0f);

		const TSharedPtr<FJsonObject>* LookObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("look"), LookObj) && LookObj->IsValid())
		{
			Segment.bApplyLookGrade = GetBool(*LookObj, TEXT("apply_grade"), false);
			Segment.LookSaturation = FMath::Clamp((float)GetNumber(*LookObj, TEXT("saturation"), 1.0), 0.0f, 4.0f);
			Segment.LookContrast = FMath::Clamp((float)GetNumber(*LookObj, TEXT("contrast"), 1.0), 0.0f, 4.0f);
			Segment.LookGain = FMath::Clamp((float)GetNumber(*LookObj, TEXT("gain"), 1.0), 0.0f, 4.0f);
			Segment.WhiteTempKelvin = FMath::Clamp((float)GetNumber(*LookObj, TEXT("white_temp_kelvin"), 0.0), 0.0f, 15000.0f);
			Segment.WhiteTint = FMath::Clamp((float)GetNumber(*LookObj, TEXT("white_tint"), 0.0), -1.0f, 1.0f);
			Segment.MotionBlurAmount = FMath::Clamp((float)GetNumber(*LookObj, TEXT("motion_blur"), -1.0), -1.0f, 1.0f);
		}

		Segment.StyleKitName = GetString(Obj, TEXT("style_kit_name"));
		Segment.FogDensity = FMath::Clamp((float)GetNumber(Obj, TEXT("fog_density"), -1.0), -1.0f, 1.0f);
		Segment.bGodRays = GetBool(Obj, TEXT("god_rays"), false);
		Segment.bVolumetricFog = GetBool(Obj, TEXT("volumetric_fog"), false);

		const TArray<TSharedPtr<FJsonValue>>* Notes = nullptr;
		if (Obj->TryGetArrayField(TEXT("notes"), Notes))
		{
			for (const TSharedPtr<FJsonValue>& Note : *Notes)
			{
				FString Text;
				if (Note.IsValid() && Note->TryGetString(Text) && !Text.IsEmpty())
				{
					Segment.ParseNotes.Add(Text);
				}
			}
		}

		OutPlan.Segments.Add(MoveTemp(Segment));
	}

	if (OutPlan.Segments.Num() == 0)
	{
		OutError = LOCTEXT("PlanNoUsableSegments", "The model's shot plan had no readable shots in it.");
		return false;
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
