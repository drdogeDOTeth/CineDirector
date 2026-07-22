// Copyright Roundtree. All Rights Reserved.

#include "CineBodyAuthor.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorBodyAuthor, Log, All);

using namespace CineBodyRigOps;

namespace
{
	/** Seated base: bum down, thighs forward, shins vertical, forward hunch. */
	void ApplySitBase(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, float SlouchDeg)
	{
		const float ThighPitch = 78.0f;
		const float Splay = 7.0f;

		Key.HipsOffset = -Rig.Up * (Rig.ThighLen * FMath::Sin(FMath::DegreesToRadians(ThighPitch)))
			- Rig.Fwd * 2.0f * Rig.Scale;
		Key.Set(Rig.Leg[0], RotAbout(Rig.Fwd, Splay) * RotAbout(Rig.Right, -ThighPitch));
		Key.Set(Rig.Leg[1], RotAbout(Rig.Fwd, -Splay) * RotAbout(Rig.Right, -ThighPitch));
		Key.Set(Rig.Knee[0], RotAbout(Rig.Fwd, Splay * 0.5f));
		Key.Set(Rig.Knee[1], RotAbout(Rig.Fwd, -Splay * 0.5f));
		Key.Set(Rig.Ankle[0], FQuat::Identity);
		Key.Set(Rig.Ankle[1], FQuat::Identity);
		// Stoop-sitter hunch (positive about Right tips segment tops toward Fwd).
		Key.Set(Rig.Spine, RotAbout(Rig.Right, 6.0f + SlouchDeg * 0.4f));
		Key.Set(Rig.Chest, RotAbout(Rig.Right, 11.0f + SlouchDeg));
		if (!Rig.Neck.IsNone())
		{
			Key.Set(Rig.Neck, RotAbout(Rig.Right, 5.0f + SlouchDeg * 0.3f));
		}
		Key.Set(Rig.Head, RotAbout(Rig.Right, 2.0f));
	}

	/** Hand rested on the same-side thigh (call after ApplySitBase). */
	void RestHandOnThigh(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, int32 Side, bool bCigGrip)
	{
		TArray<FTransform> Local, CS;
		SolvePose(Rig, Key, Local, CS);
		const FVector Hip = CS[Rig.Index(Rig.Leg[Side])].GetLocation();
		const FVector Knee = CS[Rig.Index(Rig.Knee[Side])].GetLocation();
		const float SideSign = Side == 1 ? 1.0f : -1.0f;
		if (bCigGrip)
		{
			// Cig hand hovers just past the knee, palm in-and-down.
			SolveArmIK(Rig, Key, Side,
				Knee + Rig.Fwd * 6.0f * Rig.Scale + Rig.Up * 9.0f * Rig.Scale + Rig.Right * SideSign * 3.0f * Rig.Scale,
				-Rig.Up * 0.6f + Rig.Right * SideSign * 0.55f - Rig.Fwd * 0.3f, /*Droop*/ 10.0f,
				(-Rig.Up * 0.8f - Rig.Right * SideSign * 0.3f).GetSafeNormal());
		}
		else
		{
			SolveArmIK(Rig, Key, Side,
				FMath::Lerp(Hip, Knee, 0.62f) + Rig.Up * 3.5f * Rig.Scale,
				-Rig.Up * 0.55f + Rig.Right * SideSign * 0.75f - Rig.Fwd * 0.35f, /*Droop*/ 10.0f,
				/*Palm*/ -Rig.Up);
		}
		CurlFingers(Rig, Key, Side, 20.0f, 14.0f, bCigGrip);
	}

	/** Relaxed standing base with contrapposto; hands hang (cig side keeps grip). */
	void ApplyStandBase(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, float SlouchDeg, bool bRightCigGrip)
	{
		Key.Set(Rig.Hips, RotAbout(Rig.Fwd, 2.0f));
		Key.Set(Rig.Spine, RotAbout(Rig.Fwd, -1.5f) * RotAbout(Rig.Right, SlouchDeg * 0.4f));
		Key.Set(Rig.Chest, RotAbout(Rig.Fwd, -1.0f) * RotAbout(Rig.Right, -3.0f + SlouchDeg));
		if (!Rig.Neck.IsNone())
		{
			Key.Set(Rig.Neck, FQuat::Identity);
		}
		Key.Set(Rig.Head, RotAbout(Rig.Right, 2.0f));
		Key.Set(Rig.Knee[0], FQuat::Identity);
		Key.Set(Rig.Knee[1], FQuat::Identity);

		TArray<FTransform> Local, CS;
		SolvePose(Rig, Key, Local, CS);
		// Left hand hangs near the hip pocket, palm in.
		const FVector LHip = CS[Rig.Index(Rig.Leg[0])].GetLocation();
		SolveArmIK(Rig, Key, 0,
			LHip - Rig.Right * 6.0f * Rig.Scale + Rig.Fwd * 7.0f * Rig.Scale - Rig.Up * 8.0f * Rig.Scale,
			-Rig.Right * 0.7f - Rig.Fwd * 0.4f - Rig.Up * 0.3f, /*Droop*/ 12.0f, /*Palm*/ Rig.Right);
		CurlFingers(Rig, Key, 0, 30.0f, 16.0f, false);
		// Right hand at the side (grip if smoking).
		const FVector RHip = CS[Rig.Index(Rig.Leg[1])].GetLocation();
		SolveArmIK(Rig, Key, 1,
			RHip + Rig.Right * 7.0f * Rig.Scale + Rig.Fwd * (bRightCigGrip ? 10.0f : 6.0f) * Rig.Scale - Rig.Up * 8.0f * Rig.Scale,
			Rig.Right * 0.7f - Rig.Fwd * 0.4f - Rig.Up * 0.3f, /*Droop*/ 12.0f,
			/*Palm*/ (-Rig.Right * 0.7f - Rig.Up * 0.4f).GetSafeNormal());
		CurlFingers(Rig, Key, 1, bRightCigGrip ? 0.0f : 30.0f, bRightCigGrip ? 20.0f : 16.0f, bRightCigGrip);
	}

	/** One drag cycle: rest → raise → drag → half-drop → exhale → rest. */
	void KeySmokeCycle(const FCineBodyRig& Rig, FCineBodyAnimDef& Anim, float TStart, float Span,
		TFunctionRef<FCineBodyPoseKey&(float)> MakeBaseKey)
	{
		const FVector ElbowPole = -Rig.Up * 0.6f + Rig.Right * 0.55f - Rig.Fwd * 0.3f;
		const FVector PalmToFace = (-Rig.Fwd + Rig.Up * 0.3f).GetSafeNormal();
		const FVector MouthPole = (Rig.Right * 0.75f - Rig.Up * 0.45f + Rig.Fwd * 0.1f);

		auto CigRest = [&](FCineBodyPoseKey& K)
		{
			// MakeBaseKey already parks the cig hand; nothing extra at rest.
			(void)K;
		};

		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart);
			CigRest(K);
		}
		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart + Span * 0.18f);
			K.Add(Rig.Head, RotAbout(Rig.Right, 3.5f));
			K.Add(Rig.Chest, RotAbout(Rig.Right, 2.0f));
			if (!Rig.Shoulder[1].IsNone())
			{
				K.Add(Rig.Shoulder[1], RotAbout(Rig.Fwd, 6.0f));
			}
			SolveArmIK(Rig, K, 1, MouthTarget(Rig, K) + Rig.Fwd * 9.5f * Rig.Scale - Rig.Up * 1.0f * Rig.Scale,
				MouthPole, /*Droop*/ -14.0f, PalmToFace);
			CurlFingers(Rig, K, 1, 0.0f, 20.0f, true);
		}
		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart + Span * 0.34f);
			K.Add(Rig.Head, RotAbout(Rig.Right, 3.0f));
			K.Add(Rig.Chest, RotAbout(Rig.Right, -2.5f));
			if (!Rig.Shoulder[1].IsNone())
			{
				K.Add(Rig.Shoulder[1], RotAbout(Rig.Fwd, 4.0f));
			}
			SolveArmIK(Rig, K, 1, MouthTarget(Rig, K) + Rig.Fwd * 9.5f * Rig.Scale - Rig.Up * 1.0f * Rig.Scale,
				(Rig.Right * 0.65f - Rig.Up * 0.5f + Rig.Fwd * 0.1f), /*Droop*/ -12.0f, PalmToFace);
			CurlFingers(Rig, K, 1, 0.0f, 20.0f, true);
		}
		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart + Span * 0.48f);
			K.Add(Rig.Chest, RotAbout(Rig.Right, -3.0f));
			TArray<FTransform> Local, CS;
			SolvePose(Rig, K, Local, CS);
			const FVector Knee = CS[Rig.Index(Rig.Knee[1])].GetLocation();
			SolveArmIK(Rig, K, 1, Knee + Rig.Fwd * 12.0f * Rig.Scale + Rig.Up * 25.0f * Rig.Scale, ElbowPole,
				/*Droop*/ 8.0f, /*Palm*/ (-Rig.Up * 0.6f - Rig.Fwd * 0.4f).GetSafeNormal());
			CurlFingers(Rig, K, 1, 0.0f, 20.0f, true);
		}
		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart + Span * 0.66f);
			K.Add(Rig.Head, RotAbout(Rig.Right, -9.0f) * RotAbout(Rig.Up, 8.0f));
			if (!Rig.Neck.IsNone())
			{
				K.Add(Rig.Neck, RotAbout(Rig.Right, -3.0f));
			}
			if (!Rig.Shoulder[1].IsNone())
			{
				K.Add(Rig.Shoulder[1], RotAbout(Rig.Fwd, -2.0f));
			}
			TArray<FTransform> Local, CS;
			SolvePose(Rig, K, Local, CS);
			const FVector Knee = CS[Rig.Index(Rig.Knee[1])].GetLocation();
			SolveArmIK(Rig, K, 1, Knee + Rig.Fwd * 10.0f * Rig.Scale + Rig.Up * 21.0f * Rig.Scale,
				-Rig.Up * 0.55f + Rig.Right * 0.65f - Rig.Fwd * 0.3f, /*Droop*/ 12.0f,
				/*Palm*/ (-Rig.Up * 0.5f - Rig.Fwd * 0.3f).GetSafeNormal());
			CurlFingers(Rig, K, 1, 0.0f, 20.0f, true);
		}
		{
			FCineBodyPoseKey& K = MakeBaseKey(TStart + Span * 0.86f);
			CigRest(K);
		}
	}
}

FString FCineBodyAuthor::MakeSlug(const FCineBodySpec& Spec)
{
	FString Slug = Spec.bSitting ? TEXT("Body_Sit") : TEXT("Body_Stand");
	if (Spec.bSmoke)
	{
		Slug += TEXT("_Smoke");
	}
	if (Spec.bLookAround)
	{
		Slug += TEXT("_Look");
	}
	switch (Spec.Mood)
	{
	case ECineBodyMood::Nervous: Slug += TEXT("_Nervous"); break;
	case ECineBodyMood::Chill: Slug += TEXT("_Chill"); break;
	case ECineBodyMood::Alert: Slug += TEXT("_Alert"); break;
	default: break;
	}
	return Slug;
}

FCineBodyAnimDef FCineBodyAuthor::Build(const FCineBodyRig& Rig, const FCineBodySpec& Spec)
{
	const FCineBodyMoodDials Dials = FCineBodyMoodDials::For(Spec.Mood, Spec.MoodIntensity);

	FCineBodyAnimDef Anim;
	Anim.Name = MakeSlug(Spec);
	Anim.Duration = Spec.DurationSeconds;
	Anim.LagMul = Dials.LagMul;
	Anim.OvershootMul = Dials.OvershootMul;

	// Every key carries the full base pose (both hands parked) so sparse
	// activity keys never interpolate limbs toward the reference pose.
	auto MakeBaseKey = [&Rig, &Spec, &Dials, &Anim](float Time) -> FCineBodyPoseKey&
	{
		FCineBodyPoseKey& Key = Anim.Keys.AddDefaulted_GetRef();
		Key.Time = Time;
		if (Spec.bSitting)
		{
			ApplySitBase(Rig, Key, Dials.SlouchDeg);
			RestHandOnThigh(Rig, Key, 0, false);
			RestHandOnThigh(Rig, Key, 1, Spec.bSmoke);
		}
		else
		{
			ApplyStandBase(Rig, Key, Dials.SlouchDeg, Spec.bSmoke);
		}
		return Key;
	};

	// --- Drag cycles claim their windows first.
	const float CycleLen = 8.5f / Dials.Tempo;
	TArray<TPair<float, float>> DragWindows;
	if (Spec.bSmoke)
	{
		float Start = 1.0f / Dials.Tempo;
		while (Start + CycleLen + 1.5f < Anim.Duration)
		{
			KeySmokeCycle(Rig, Anim, Start, CycleLen, MakeBaseKey);
			DragWindows.Emplace(Start - 0.4f, Start + CycleLen * 0.86f + 0.4f);
			Start += CycleLen + 5.0f / Dials.Tempo;
		}
		if (DragWindows.Num() == 0)
		{
			// Too short for a full cycle — still park the pose at the loop ends.
			MakeBaseKey(0.0f);
			MakeBaseKey(Anim.Duration * 0.5f);
		}
	}

	// --- Base / look keys fill the space between drags.
	{
		const float Step = 2.8f / Dials.Tempo;
		int32 LookIndex = Spec.Seed % 3;
		for (float T = 0.0f; T < Anim.Duration - 0.5f; T += Step)
		{
			bool bInsideDrag = false;
			for (const TPair<float, float>& Window : DragWindows)
			{
				if (T > Window.Key && T < Window.Value)
				{
					bInsideDrag = true;
					break;
				}
			}
			if (bInsideDrag)
			{
				continue;
			}
			FCineBodyPoseKey& Key = MakeBaseKey(T);
			if (Spec.bLookAround)
			{
				// Alternating held sweeps; seed staggers the pattern per actor.
				const int32 Pair = LookIndex / 2;
				const float Hold = (LookIndex % 2 == 0) ? 1.0f : 0.9f;
				const float Yaw = (34.0f + 11.0f * ((Pair * 7) % 3))
					* ((Pair % 2 == 0) ? 1.0f : -1.0f) * Dials.LookYawMul * Hold;
				const float Pitch = (float)(((Pair * 5) % 3) - 1) * 6.0f;
				Key.Add(Rig.Head, RotAbout(Rig.Up, Yaw * 0.65f) * RotAbout(Rig.Right, Pitch)
					* RotAbout(Rig.Fwd, Yaw * 0.10f));
				if (!Rig.Neck.IsNone())
				{
					Key.Add(Rig.Neck, RotAbout(Rig.Up, Yaw * 0.35f));
				}
				Key.Add(Rig.Chest, RotAbout(Rig.Up, Yaw * 0.3f));
				++LookIndex;
			}
			else if (LookIndex++ % 3 == 1)
			{
				// Plain idles still glance occasionally.
				Key.Add(Rig.Head, RotAbout(Rig.Up, (LookIndex % 2 == 0 ? -14.0f : 9.0f)));
			}
		}
	}

	Anim.Keys.Sort([](const FCineBodyPoseKey& A, const FCineBodyPoseKey& B) { return A.Time < B.Time; });

	// --- Ambient layers + mood ticks.
	const FVector Right = Rig.Right;
	const FVector Fwd = Rig.Fwd;
	const FVector Up = Rig.Up;
	const FName Chest = Rig.Chest;
	const FName Spine = Rig.Spine;
	const FName Hips = Rig.Hips;
	const FName Head = Rig.Head;
	const FName ShoulderL = Rig.Shoulder[0];
	const FName ShoulderR = Rig.Shoulder[1];
	const FName ThighR = Rig.Leg[1];
	const FName AnkleR = Rig.Ankle[1];
	const float Ambient = Dials.Ambient;
	const float Phase = (float)(Spec.Seed % 97) * 0.13f;
	const bool bSit = Spec.bSitting;
	const bool bTap = Dials.bFootTap && Spec.bSitting;
	const bool bBounce = Dials.bKneeBounce && Spec.bSitting;
	const float TapStart = Anim.Duration * 0.35f;
	const float BounceA = Anim.Duration * 0.25f;
	const float BounceB = Anim.Duration * 0.55f;

	Anim.Procedural = [=](float T0, FCineBodyPoseKey& Pose)
	{
		const float T = T0 + Phase;
		const float Breath = FMath::Sin(2.0f * PI * T / 4.2f);
		const float SwayA = FMath::Sin(2.0f * PI * T / 7.3f + 0.8f);
		const float SwayB = FMath::Sin(2.0f * PI * T / 11.1f + 2.1f);
		Pose.Add(Chest, RotAbout(Right, -1.1f * Breath * Ambient));
		Pose.Add(Spine, RotAbout(Right, -0.5f * Breath * Ambient) * RotAbout(Fwd, 0.7f * SwayA * Ambient));
		Pose.Add(Hips, RotAbout(Fwd, 0.6f * SwayB * Ambient));
		Pose.Add(Head,
			RotAbout(Up, 1.6f * FMath::Sin(2.0f * PI * T / 5.7f + 1.3f) * Ambient) *
			RotAbout(Right, 0.9f * FMath::Sin(2.0f * PI * T / 8.9f + 0.4f) * Ambient));
		if (!ShoulderR.IsNone())
		{
			Pose.Add(ShoulderR, RotAbout(Fwd, 0.8f * Breath * Ambient));
		}
		if (!ShoulderL.IsNone())
		{
			Pose.Add(ShoulderL, RotAbout(Fwd, -0.7f * Breath * Ambient));
		}
		Pose.HipsOffset += Up * (0.35f * Breath * Ambient)
			+ Right * (0.8f * FMath::Sin(2.0f * PI * T / 9.7f + 1.9f) * Ambient);

		if (bTap && T0 > TapStart && T0 < TapStart + 2.8f)
		{
			const float Tap = FMath::Max(0.0f, FMath::Sin(2.0f * PI * 1.9f * (T0 - TapStart)));
			Pose.Add(AnkleR, RotAbout(Right, -7.0f * Tap));
		}
		if (bBounce && T0 > BounceA && T0 < BounceB)
		{
			const float Fade = FMath::Clamp(
				FMath::Min(T0 - BounceA, BounceB - T0) / 0.5f, 0.0f, 1.0f);
			Pose.Add(ThighR, RotAbout(Right, 1.4f * Fade * FMath::Sin(2.0f * PI * 2.7f * T0)));
		}
		(void)bSit;
	};

	UE_LOG(LogCineDirectorBodyAuthor, Display, TEXT("Built '%s': %d keys, %.0fs, %d drag cycles"),
		*Anim.Name, Anim.Keys.Num(), Anim.Duration, DragWindows.Num());
	return Anim;
}
