// Copyright Roundtree. All Rights Reserved.

#include "CineBodyRig.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorBody, Log, All);

namespace
{
	// ------------------------------------------------------------- schemes

	struct FSchemeNames
	{
		const TCHAR* Name;
		const TCHAR* Hips;
		const TCHAR* Spine;
		const TCHAR* Chest;
		const TCHAR* Neck;
		const TCHAR* Head;
		const TCHAR* Eye[2];
		const TCHAR* Shoulder[2];
		const TCHAR* Arm[2];
		const TCHAR* Elbow[2];
		const TCHAR* Wrist[2];
		const TCHAR* Leg[2];
		const TCHAR* Knee[2];
		const TCHAR* Ankle[2];
		const TCHAR* Toe[2];
		/** printf patterns with %d segment; empty string = none. */
		const TCHAR* FingerPatterns[2][4]; // per side: index, middle, ring, pinky
		const TCHAR* ThumbPattern[2];
		int32 FingerFirstSegment;
		int32 ThumbFirstSegment;
	};

	// VRM-style void rigs: Hips / Left-arm / IndexFinger1_L, thumb starts at 0.
	const FSchemeNames VoidScheme = {
		TEXT("VRM-void"),
		TEXT("Hips"), TEXT("Spine"), TEXT("Chest"), TEXT("Neck"), TEXT("Head"),
		{ TEXT("LeftEye"), TEXT("RightEye") },
		{ TEXT("Left-shoulder"), TEXT("Right-shoulder") },
		{ TEXT("Left-arm"), TEXT("Right-arm") },
		{ TEXT("Left-elbow"), TEXT("Right-elbow") },
		{ TEXT("Left-wrist"), TEXT("Right-wrist") },
		{ TEXT("Left-leg"), TEXT("Right-leg") },
		{ TEXT("Left-knee"), TEXT("Right-knee") },
		{ TEXT("Left-ankle"), TEXT("Right-ankle") },
		{ TEXT("Left-toe"), TEXT("Right-toe") },
		{
			{ TEXT("IndexFinger%d_L"), TEXT("MiddleFinger%d_L"), TEXT(""), TEXT("LittleFinger%d_L") },
			{ TEXT("IndexFinger%d_R"), TEXT("MiddleFinger%d_R"), TEXT(""), TEXT("LittleFinger%d_R") },
		},
		{ TEXT("Thumb%d_L"), TEXT("Thumb%d_R") },
		1, 0,
	};

	// Blender-exported Mixamo rigs (wyn / GrillzGang family): mixamorig_*.
	const FSchemeNames MixamoScheme = {
		TEXT("Mixamo"),
		TEXT("mixamorig_Hips"), TEXT("mixamorig_Spine"), TEXT("mixamorig_Spine2"),
		TEXT("mixamorig_Neck"), TEXT("mixamorig_Head"),
		{ TEXT(""), TEXT("") },
		{ TEXT("mixamorig_LeftShoulder"), TEXT("mixamorig_RightShoulder") },
		{ TEXT("mixamorig_LeftArm"), TEXT("mixamorig_RightArm") },
		{ TEXT("mixamorig_LeftForeArm"), TEXT("mixamorig_RightForeArm") },
		{ TEXT("mixamorig_LeftHand"), TEXT("mixamorig_RightHand") },
		{ TEXT("mixamorig_LeftUpLeg"), TEXT("mixamorig_RightUpLeg") },
		{ TEXT("mixamorig_LeftLeg"), TEXT("mixamorig_RightLeg") },
		{ TEXT("mixamorig_LeftFoot"), TEXT("mixamorig_RightFoot") },
		{ TEXT("mixamorig_LeftToeBase"), TEXT("mixamorig_RightToeBase") },
		{
			{ TEXT("mixamorig_LeftHandIndex%d"), TEXT("mixamorig_LeftHandMiddle%d"), TEXT("mixamorig_LeftHandRing%d"), TEXT("mixamorig_LeftHandPinky%d") },
			{ TEXT("mixamorig_RightHandIndex%d"), TEXT("mixamorig_RightHandMiddle%d"), TEXT("mixamorig_RightHandRing%d"), TEXT("mixamorig_RightHandPinky%d") },
		},
		{ TEXT("mixamorig_LeftHandThumb%d"), TEXT("mixamorig_RightHandThumb%d") },
		1, 1,
	};

	bool MeshHasBone(const USkeletalMesh* Mesh, const TCHAR* BoneName)
	{
		return Mesh->GetRefSkeleton().FindBoneIndex(FName(BoneName)) != INDEX_NONE;
	}

	// --------------------------------------------------------- humanization

	float JointLag(ECineBodyJoint Role)
	{
		switch (Role)
		{
		case ECineBodyJoint::Spine:    return 0.16f;
		case ECineBodyJoint::Chest:    return 0.12f;
		case ECineBodyJoint::Neck:     return 0.05f;
		case ECineBodyJoint::Clavicle: return 0.04f;
		case ECineBodyJoint::UpperArm: return 0.02f;
		case ECineBodyJoint::Forearm:  return 0.06f;
		case ECineBodyJoint::Wrist:    return 0.10f;
		case ECineBodyJoint::Finger:   return 0.13f;
		default:                       return 0.0f; // head leads; legs stay planted
		}
	}

	float JointOvershoot(ECineBodyJoint Role)
	{
		switch (Role)
		{
		case ECineBodyJoint::Head:     return 0.10f;
		case ECineBodyJoint::Neck:     return 0.06f;
		case ECineBodyJoint::UpperArm: return 0.04f;
		case ECineBodyJoint::Forearm:  return 0.09f;
		case ECineBodyJoint::Wrist:    return 0.14f;
		case ECineBodyJoint::Chest:    return 0.03f;
		default:                       return 0.0f;
		}
	}

	/** Ease with optional terminal overshoot (ease-out-back); lands exactly at 1. */
	float EaseAlpha(float Alpha, float Overshoot)
	{
		if (Overshoot <= 0.0f)
		{
			return FMath::SmoothStep(0.0f, 1.0f, Alpha);
		}
		const float S = Overshoot * 8.0f;
		const float A1 = Alpha - 1.0f;
		return 1.0f + (1.0f + S) * A1 * A1 * A1 + S * A1 * A1;
	}

	void BracketKeys(const FCineBodyAnimDef& Anim, float Time, int32& OutA, int32& OutB, float& OutAlpha)
	{
		OutA = Anim.Keys.Num() - 1;
		OutB = 0;
		float SegStart = Anim.Keys.Last().Time;
		float SegEnd = Anim.Duration + Anim.Keys[0].Time;
		for (int32 i = 0; i + 1 < Anim.Keys.Num(); ++i)
		{
			if (Time >= Anim.Keys[i].Time && Time <= Anim.Keys[i + 1].Time)
			{
				OutA = i;
				OutB = i + 1;
				SegStart = Anim.Keys[i].Time;
				SegEnd = Anim.Keys[i + 1].Time;
				break;
			}
		}
		OutAlpha = SegEnd > SegStart
			? FMath::Clamp((Time - SegStart) / (SegEnd - SegStart), 0.0f, 1.0f)
			: 0.0f;
		if (Time < Anim.Keys[0].Time)
		{
			OutAlpha = FMath::Clamp((Time + Anim.Duration - SegStart) / (SegEnd - SegStart), 0.0f, 1.0f);
		}
	}

	// --------------------------------------------------------------- drawing

	void DrawLine(TArray<FColor>& Canvas, int32 W, int32 H, FVector2D A, FVector2D B, FColor Color)
	{
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FMath::Max(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y))));
		for (int32 i = 0; i <= Steps; ++i)
		{
			const FVector2D P = FMath::Lerp(A, B, (float)i / Steps);
			const int32 X = FMath::RoundToInt32(P.X);
			const int32 Y = FMath::RoundToInt32(P.Y);
			if (X >= 0 && X < W && Y >= 0 && Y < H)
			{
				Canvas[Y * W + X] = Color;
			}
		}
	}

	void DrawDot(TArray<FColor>& Canvas, int32 W, int32 H, FVector2D P, FColor Color)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				const int32 X = FMath::RoundToInt32(P.X) + dx;
				const int32 Y = FMath::RoundToInt32(P.Y) + dy;
				if (X >= 0 && X < W && Y >= 0 && Y < H)
				{
					Canvas[Y * W + X] = Color;
				}
			}
		}
	}
}

bool FCineBodyRig::Analyze(USkeletalMesh* InMesh, FCineBodyRig& Out, FString& OutError)
{
	if (!InMesh)
	{
		OutError = TEXT("No skeletal mesh.");
		return false;
	}
	Out.Mesh = InMesh;
	Out.RefSkel = &InMesh->GetRefSkeleton();
	const int32 NumBones = Out.RefSkel->GetNum();
	Out.RefLocal = Out.RefSkel->GetRefBonePose();
	Out.RefCS.SetNum(NumBones);
	for (int32 b = 0; b < NumBones; ++b)
	{
		const int32 Parent = Out.RefSkel->GetParentIndex(b);
		Out.RefCS[b] = Parent == INDEX_NONE ? Out.RefLocal[b] : Out.RefLocal[b] * Out.RefCS[Parent];
	}

	const FSchemeNames* Schemes[] = { &VoidScheme, &MixamoScheme };
	const FSchemeNames* Scheme = nullptr;
	for (const FSchemeNames* Candidate : Schemes)
	{
		if (MeshHasBone(InMesh, Candidate->Hips) && MeshHasBone(InMesh, Candidate->Arm[0]))
		{
			Scheme = Candidate;
			break;
		}
	}
	if (!Scheme)
	{
		OutError = FString::Printf(
			TEXT("'%s' does not match a supported skeleton scheme (VRM-void 'Hips'/'Left-arm' or Mixamo 'mixamorig_*')."),
			*InMesh->GetName());
		return false;
	}
	Out.SchemeName = Scheme->Name;

	auto Resolve = [&Out](const TCHAR* Name, ECineBodyJoint Role) -> FName
	{
		if (!Name || !Name[0])
		{
			return NAME_None;
		}
		const FName BoneName(Name);
		if (Out.Index(BoneName) == INDEX_NONE)
		{
			return NAME_None;
		}
		Out.Roles.Add(BoneName, Role);
		return BoneName;
	};

	Out.Hips = Resolve(Scheme->Hips, ECineBodyJoint::Hips);
	Out.Spine = Resolve(Scheme->Spine, ECineBodyJoint::Spine);
	Out.Chest = Resolve(Scheme->Chest, ECineBodyJoint::Chest);
	Out.Neck = Resolve(Scheme->Neck, ECineBodyJoint::Neck);
	Out.Head = Resolve(Scheme->Head, ECineBodyJoint::Head);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		Out.Eye[Side] = Resolve(Scheme->Eye[Side], ECineBodyJoint::Eye);
		Out.Shoulder[Side] = Resolve(Scheme->Shoulder[Side], ECineBodyJoint::Clavicle);
		Out.Arm[Side] = Resolve(Scheme->Arm[Side], ECineBodyJoint::UpperArm);
		Out.Elbow[Side] = Resolve(Scheme->Elbow[Side], ECineBodyJoint::Forearm);
		Out.Wrist[Side] = Resolve(Scheme->Wrist[Side], ECineBodyJoint::Wrist);
		Out.Leg[Side] = Resolve(Scheme->Leg[Side], ECineBodyJoint::Thigh);
		Out.Knee[Side] = Resolve(Scheme->Knee[Side], ECineBodyJoint::Shin);
		Out.Ankle[Side] = Resolve(Scheme->Ankle[Side], ECineBodyJoint::Foot);
		Out.Toe[Side] = Resolve(Scheme->Toe[Side], ECineBodyJoint::Toe);

		for (int32 FingerIndex = 0; FingerIndex < 4; ++FingerIndex)
		{
			const TCHAR* Pattern = Scheme->FingerPatterns[Side][FingerIndex];
			if (!Pattern || !Pattern[0])
			{
				continue;
			}
			TArray<FName> Chain;
			for (int32 Seg = 0; Seg < 3; ++Seg)
			{
				// Runtime pattern → no Printf (UE 5.8 checked format strings).
				const FString SegString = FString(Pattern).Replace(TEXT("%d"),
					*FString::FromInt(Scheme->FingerFirstSegment + Seg));
				const FName SegName(*SegString);
				if (Out.Index(SegName) == INDEX_NONE)
				{
					break;
				}
				Out.Roles.Add(SegName, ECineBodyJoint::Finger);
				Chain.Add(SegName);
			}
			if (Chain.Num() == 3)
			{
				Out.Fingers[Side].Add(MoveTemp(Chain));
			}
		}
		for (int32 Seg = 0; Seg < 3; ++Seg)
		{
			const FString SegString = FString(Scheme->ThumbPattern[Side]).Replace(TEXT("%d"),
				*FString::FromInt(Scheme->ThumbFirstSegment + Seg));
			const FName SegName(*SegString);
			if (Out.Index(SegName) == INDEX_NONE)
			{
				break;
			}
			Out.Roles.Add(SegName, ECineBodyJoint::Finger);
			Out.Thumb[Side].Add(SegName);
		}
	}

	const FName Required[] = {
		Out.Hips, Out.Spine, Out.Chest, Out.Head,
		Out.Arm[0], Out.Arm[1], Out.Elbow[0], Out.Elbow[1], Out.Wrist[0], Out.Wrist[1],
		Out.Leg[0], Out.Leg[1], Out.Knee[0], Out.Knee[1], Out.Ankle[0], Out.Ankle[1],
	};
	for (const FName Bone : Required)
	{
		if (Bone.IsNone())
		{
			OutError = FString::Printf(TEXT("'%s' matched scheme %s but is missing core limb/torso bones."),
				*InMesh->GetName(), Scheme->Name);
			return false;
		}
	}

	// Facing: toes first, eyes as fallback (both are unambiguous on real rigs).
	FVector FacingDir = FVector::ZeroVector;
	if (Out.Has(Out.Toe[0]) && Out.Has(Out.Toe[1]))
	{
		FacingDir = (Out.RefPos(Out.Toe[0]) - Out.RefPos(Out.Ankle[0]))
			+ (Out.RefPos(Out.Toe[1]) - Out.RefPos(Out.Ankle[1]));
	}
	else if (Out.Has(Out.Eye[0]) && Out.Has(Out.Eye[1]))
	{
		FacingDir = (Out.RefPos(Out.Eye[0]) + Out.RefPos(Out.Eye[1])) * 0.5f - Out.RefPos(Out.Head);
	}
	FacingDir.Z = 0.0f;
	if (!FacingDir.Normalize())
	{
		OutError = FString::Printf(TEXT("'%s': could not derive facing (no toe or eye bones)."), *InMesh->GetName());
		return false;
	}
	Out.Fwd = FacingDir;
	Out.Up = FVector::UpVector;
	Out.Right = FVector::CrossProduct(Out.Up, Out.Fwd).GetSafeNormal();
	const FVector ShoulderSpan = Out.RefPos(Out.Arm[1]) - Out.RefPos(Out.Arm[0]);
	if (FVector::DotProduct(ShoulderSpan, Out.Right) < 0.0f)
	{
		UE_LOG(LogCineDirectorBody, Warning, TEXT("%s: shoulder span disagrees with facing — flipping Right."),
			*InMesh->GetName());
		Out.Right = -Out.Right;
		Out.Fwd = FVector::CrossProduct(Out.Right, Out.Up).GetSafeNormal();
	}

	Out.Scale = Out.RefPos(Out.Hips).Z / 92.0f;
	if (Out.Scale < 0.2f || Out.Scale > 5.0f)
	{
		Out.Scale = 1.0f;
	}
	Out.ThighLen = FVector::Dist(Out.RefPos(Out.Leg[0]), Out.RefPos(Out.Knee[0]));

	UE_LOG(LogCineDirectorBody, Display, TEXT("%s: scheme=%s bones=%d fingers L/R=%d/%d scale=%.2f"),
		*InMesh->GetName(), *Out.SchemeName, NumBones, Out.Fingers[0].Num(), Out.Fingers[1].Num(), Out.Scale);
	return true;
}

namespace CineBodyRigOps
{
	FQuat RotAbout(const FVector& Axis, float Degrees)
	{
		return FQuat(Axis.GetSafeNormal(), FMath::DegreesToRadians(Degrees));
	}

	FVector HandPalmNormalRef(const FCineBodyRig& Rig, int32 Side)
	{
		if (Rig.Fingers[Side].Num() < 2 || Rig.Thumb[Side].Num() == 0)
		{
			return FVector::ZeroVector;
		}
		const TArray<FName>& First = Rig.Fingers[Side][0];
		const TArray<FName>& Last = Rig.Fingers[Side].Last();
		const TArray<FName>& Mid = Rig.Fingers[Side][Rig.Fingers[Side].Num() / 2];
		const FVector KnuckleLine = (Rig.RefPos(Last[0]) - Rig.RefPos(First[0])).GetSafeNormal();
		const FVector FingerDir = (Rig.RefPos(Mid[2]) - Rig.RefPos(Mid[0])).GetSafeNormal();
		FVector PalmNormal = FVector::CrossProduct(KnuckleLine, FingerDir).GetSafeNormal();
		if (FVector::DotProduct(PalmNormal, Rig.RefPos(Rig.Thumb[Side][0]) - Rig.RefPos(Mid[0])) < 0.0f)
		{
			PalmNormal = -PalmNormal;
		}
		return PalmNormal;
	}

	void EvaluatePose(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim, float Time,
		const TSet<FName>& AllBones, FCineBodyPoseKey& Out)
	{
		Out.Deltas.Reset();
		Out.LocalDeltas.Reset();
		Out.HipsOffset = FVector::ZeroVector;
		Out.Time = Time;
		if (Anim.Keys.Num() > 0)
		{
			for (const FName Bone : AllBones)
			{
				const ECineBodyJoint Role = Rig.RoleOf(Bone);
				const float BoneTime = FMath::Fmod(
					Time - JointLag(Role) * Anim.LagMul + Anim.Duration, Anim.Duration);
				int32 A, B;
				float Alpha;
				BracketKeys(Anim, BoneTime, A, B, Alpha);
				Alpha = EaseAlpha(Alpha, JointOvershoot(Role) * Anim.OvershootMul);

				const FCineBodyPoseKey& KA = Anim.Keys[A];
				const FCineBodyPoseKey& KB = Anim.Keys[B];
				if (const FQuat* QA = KA.Deltas.Find(Bone))
				{
					const FQuat* QB = KB.Deltas.Find(Bone);
					Out.Deltas.Add(Bone, FQuat::Slerp(*QA, QB ? *QB : FQuat::Identity, Alpha).GetNormalized());
				}
				else if (const FQuat* QB = KB.Deltas.Find(Bone))
				{
					Out.Deltas.Add(Bone, FQuat::Slerp(FQuat::Identity, *QB, Alpha).GetNormalized());
				}
				const FQuat* LA = KA.LocalDeltas.Find(Bone);
				const FQuat* LB = KB.LocalDeltas.Find(Bone);
				if (LA || LB)
				{
					Out.LocalDeltas.Add(Bone, FQuat::Slerp(
						LA ? *LA : FQuat::Identity, LB ? *LB : FQuat::Identity, Alpha).GetNormalized());
				}
			}
			// Hips offset stays on keyed timing so seat/feet contact never swims.
			int32 A, B;
			float Alpha;
			BracketKeys(Anim, Time, A, B, Alpha);
			Alpha = FMath::SmoothStep(0.0f, 1.0f, Alpha);
			Out.HipsOffset = FMath::Lerp(Anim.Keys[A].HipsOffset, Anim.Keys[B].HipsOffset, Alpha);
		}
		if (Anim.Procedural)
		{
			Anim.Procedural(Time, Out);
		}
	}

	void SolvePose(const FCineBodyRig& Rig, const FCineBodyPoseKey& Pose,
		TArray<FTransform>& OutLocal, TArray<FTransform>& OutCS)
	{
		const int32 NumBones = Rig.RefSkel->GetNum();
		OutLocal = Rig.RefLocal;
		OutCS.SetNum(NumBones);
		const int32 HipsIndex = Rig.Index(Rig.Hips);

		for (int32 b = 0; b < NumBones; ++b)
		{
			const int32 Parent = Rig.RefSkel->GetParentIndex(b);
			const FTransform ParentCS = Parent == INDEX_NONE ? FTransform::Identity : OutCS[Parent];
			const FName BoneName = Rig.RefSkel->GetBoneName(b);
			const FQuat* Delta = Pose.Deltas.Find(BoneName);

			FVector Pivot = ParentCS.TransformPosition(Rig.RefLocal[b].GetLocation());
			if (b == HipsIndex)
			{
				Pivot += Pose.HipsOffset;
			}

			const FQuat* LocalDelta = Delta ? nullptr : Pose.LocalDeltas.Find(BoneName);
			if (Delta || b == HipsIndex)
			{
				const FQuat NewRot = Delta
					? (*Delta * Rig.RefCS[b].GetRotation()).GetNormalized()
					: (ParentCS.GetRotation() * Rig.RefLocal[b].GetRotation()).GetNormalized();
				OutCS[b] = FTransform(NewRot, Pivot, Rig.RefCS[b].GetScale3D());
				OutLocal[b] = OutCS[b].GetRelativeTransform(ParentCS);
			}
			else if (LocalDelta)
			{
				OutLocal[b] = Rig.RefLocal[b];
				OutLocal[b].SetRotation((Rig.RefLocal[b].GetRotation() * *LocalDelta).GetNormalized());
				OutCS[b] = OutLocal[b] * ParentCS;
			}
			else
			{
				OutCS[b] = Rig.RefLocal[b] * ParentCS;
			}
		}
	}

	void SolveArmIK(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, int32 Side,
		const FVector& Target, const FVector& PoleDir, float WristDroopDeg, const FVector& PalmTowards)
	{
		const FName UpperName = Rig.Arm[Side];
		const FName ForeName = Rig.Elbow[Side];
		const FName WristName = Rig.Wrist[Side];

		TArray<FTransform> Local, CS;
		SolvePose(Rig, Key, Local, CS);
		const FVector S = CS[Rig.Index(UpperName)].GetLocation();

		const float A = FVector::Dist(Rig.RefPos(UpperName), Rig.RefPos(ForeName));
		const float B = FVector::Dist(Rig.RefPos(ForeName), Rig.RefPos(WristName));

		FVector D = Target - S;
		const float DistRaw = D.Size();
		const float Dist = FMath::Clamp(DistRaw, FMath::Abs(A - B) * 1.02f, (A + B) * 0.995f);
		const FVector Dn = DistRaw > 1e-4f ? D / DistRaw : Rig.Fwd;

		const float CosShoulder = FMath::Clamp((A * A + Dist * Dist - B * B) / (2.0f * A * Dist), -1.0f, 1.0f);
		const float SinShoulder = FMath::Sqrt(FMath::Max(0.0f, 1.0f - CosShoulder * CosShoulder));

		FVector Pole = PoleDir - Dn * FVector::DotProduct(PoleDir, Dn);
		if (!Pole.Normalize())
		{
			Pole = -Rig.Up;
		}
		const FVector ElbowPos = S + Dn * (A * CosShoulder) + Pole * (A * SinShoulder);
		const FVector WristPos = S + Dn * Dist;

		const FVector UpperRefDir = (Rig.RefPos(ForeName) - Rig.RefPos(UpperName)).GetSafeNormal();
		const FVector ForeRefDir = (Rig.RefPos(WristName) - Rig.RefPos(ForeName)).GetSafeNormal();
		const FVector UpperNewDir = (ElbowPos - S).GetSafeNormal();
		const FVector ForeNewDir = (WristPos - ElbowPos).GetSafeNormal();

		Key.Set(UpperName, FQuat::FindBetweenNormals(UpperRefDir, UpperNewDir));
		FQuat ForeDelta = FQuat::FindBetweenNormals(ForeRefDir, ForeNewDir);

		// Palm-facing twist about the forearm axis (pronation/supination) —
		// FindBetweenNormals alone leaves twist wherever the ref pose had it.
		if (!PalmTowards.IsNearlyZero())
		{
			const FVector RefPalm = HandPalmNormalRef(Rig, Side);
			if (!RefPalm.IsNearlyZero())
			{
				const FVector CurrentPalm = ForeDelta.RotateVector(RefPalm);
				FVector From = CurrentPalm - ForeNewDir * FVector::DotProduct(CurrentPalm, ForeNewDir);
				FVector To = PalmTowards - ForeNewDir * FVector::DotProduct(PalmTowards, ForeNewDir);
				if (From.Normalize() && To.Normalize())
				{
					float Angle = FMath::Atan2(
						FVector::DotProduct(FVector::CrossProduct(From, To), ForeNewDir),
						FVector::DotProduct(From, To));
					Angle = FMath::Clamp(Angle, -FMath::DegreesToRadians(80.0f), FMath::DegreesToRadians(80.0f));
					ForeDelta = (FQuat(ForeNewDir, Angle) * ForeDelta).GetNormalized();
				}
			}
		}
		Key.Set(ForeName, ForeDelta);

		// Relaxed wrist break off the forearm line (positive = toward gravity).
		FVector DroopAxis = FVector::CrossProduct(ForeNewDir, -Rig.Up);
		if (DroopAxis.Normalize())
		{
			Key.Set(WristName, FQuat(DroopAxis, FMath::DegreesToRadians(WristDroopDeg)) * ForeDelta);
		}
		else
		{
			Key.Set(WristName, ForeDelta);
		}
	}

	void CurlFingers(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, int32 Side,
		float CurlDeg, float ThumbDeg, bool bCigGrip)
	{
		const FVector PalmNormal = HandPalmNormalRef(Rig, Side);
		if (PalmNormal.IsNearlyZero())
		{
			return; // rigid hands on this mesh
		}
		const TArray<FName>& Mid = Rig.Fingers[Side][Rig.Fingers[Side].Num() / 2];
		const FVector FingerDir = (Rig.RefPos(Mid[2]) - Rig.RefPos(Mid[0])).GetSafeNormal();
		const FVector CurlAxis = FVector::CrossProduct(FingerDir, PalmNormal).GetSafeNormal();

		auto SetLocalCurl = [&Rig, &Key](const FName Bone, const FVector& ComponentAxis, float Degrees)
		{
			const int32 BoneIndex = Rig.Index(Bone);
			if (BoneIndex == INDEX_NONE)
			{
				return;
			}
			const FVector LocalAxis = Rig.RefCS[BoneIndex].GetRotation().Inverse().RotateVector(ComponentAxis);
			Key.SetLocal(Bone, FQuat(LocalAxis.GetSafeNormal(), FMath::DegreesToRadians(Degrees)));
		};

		const float SegScale[3] = { 1.0f, 0.87f, 0.72f };
		for (int32 FingerIndex = 0; FingerIndex < Rig.Fingers[Side].Num(); ++FingerIndex)
		{
			const TArray<FName>& Chain = Rig.Fingers[Side][FingerIndex];
			// Later fingers curl slightly more in a relaxed hand.
			float Base = CurlDeg * (1.0f + 0.06f * FingerIndex);
			if (bCigGrip)
			{
				// Cig pinched between the first two fingers; the rest curl away.
				Base = FingerIndex == 0 ? 7.0f : (FingerIndex == 1 ? 11.0f : 52.0f);
			}
			for (int32 Seg = 0; Seg < 3; ++Seg)
			{
				SetLocalCurl(Chain[Seg], CurlAxis, Base * SegScale[Seg]);
			}
		}
		if (bCigGrip && Rig.Fingers[Side].Num() >= 2)
		{
			// Spread the V around the palm normal.
			SetLocalCurl(Rig.Fingers[Side][0][0], PalmNormal, -5.0f);
			SetLocalCurl(Rig.Fingers[Side][1][0], PalmNormal, 5.0f);
		}
		if (Rig.Thumb[Side].Num() >= 2)
		{
			const FVector ThumbDir = (Rig.RefPos(Rig.Thumb[Side].Last()) - Rig.RefPos(Rig.Thumb[Side][0])).GetSafeNormal();
			const FVector ThumbAxis = FVector::CrossProduct(ThumbDir, PalmNormal).GetSafeNormal();
			SetLocalCurl(Rig.Thumb[Side][FMath::Min(1, Rig.Thumb[Side].Num() - 1)], ThumbAxis, ThumbDeg);
			SetLocalCurl(Rig.Thumb[Side].Last(), ThumbAxis, ThumbDeg * 0.7f);
		}
	}

	FVector MouthTarget(const FCineBodyRig& Rig, const FCineBodyPoseKey& Key)
	{
		TArray<FTransform> Local, CS;
		SolvePose(Rig, Key, Local, CS);
		FVector EyeMid;
		if (Rig.Has(Rig.Eye[0]) && Rig.Has(Rig.Eye[1]))
		{
			EyeMid = (CS[Rig.Index(Rig.Eye[0])].GetLocation() + CS[Rig.Index(Rig.Eye[1])].GetLocation()) * 0.5f;
		}
		else
		{
			EyeMid = CS[Rig.Index(Rig.Head)].GetLocation() + Rig.Up * 8.0f * Rig.Scale + Rig.Fwd * 6.0f * Rig.Scale;
		}
		const FQuat HeadRot = CS[Rig.Index(Rig.Head)].GetRotation() * Rig.RefCS[Rig.Index(Rig.Head)].GetRotation().Inverse();
		const FVector HeadFwd = HeadRot.RotateVector(Rig.Fwd);
		const FVector HeadUp = HeadRot.RotateVector(Rig.Up);
		return EyeMid - HeadUp * 5.5f * Rig.Scale + HeadFwd * 1.5f * Rig.Scale;
	}

	TSet<FName> CollectAnimBones(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim)
	{
		TSet<FName> Bones;
		for (const FCineBodyPoseKey& Key : Anim.Keys)
		{
			for (const TPair<FName, FQuat>& Pair : Key.Deltas)
			{
				Bones.Add(Pair.Key);
			}
			for (const TPair<FName, FQuat>& Pair : Key.LocalDeltas)
			{
				Bones.Add(Pair.Key);
			}
		}
		Bones.Add(Rig.Hips);
		Bones.Add(Rig.Spine);
		Bones.Add(Rig.Chest);
		Bones.Add(Rig.Head);
		if (!Rig.Shoulder[0].IsNone()) { Bones.Add(Rig.Shoulder[0]); }
		if (!Rig.Shoulder[1].IsNone()) { Bones.Add(Rig.Shoulder[1]); }
		return Bones;
	}

	UAnimSequence* Bake(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim,
		const FString& PackageFolder, TArray<UPackage*>& OutPackages, FString& OutError)
	{
		const int32 Fps = 30;
		const int32 NumFrames = FMath::Max(2, FMath::RoundToInt32(Anim.Duration * Fps));
		const TSet<FName> Bones = CollectAnimBones(Rig, Anim);

		TMap<FName, TArray<FTransform>> Tracks;
		for (const FName Bone : Bones)
		{
			Tracks.Add(Bone).SetNum(NumFrames);
		}
		FCineBodyPoseKey Pose;
		TArray<FTransform> Local, CS;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			EvaluatePose(Rig, Anim, (float)Frame / Fps, Bones, Pose);
			SolvePose(Rig, Pose, Local, CS);
			for (const FName Bone : Bones)
			{
				Tracks.FindChecked(Bone)[Frame] = Local[Rig.Index(Bone)];
			}
		}

		const FString PackagePath = PackageFolder / Anim.Name;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			OutError = TEXT("Could not create the animation package.");
			return nullptr;
		}
		Package->FullyLoad();

		UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *Anim.Name, RF_Public | RF_Standalone);
		Sequence->SetSkeleton(Rig.Mesh->GetSkeleton());

		IAnimationDataController& Ctrl = Sequence->GetController();
		Ctrl.OpenBracket(FText::FromString(TEXT("CineDirector body performance")));
		Ctrl.InitializeModel();
		Ctrl.SetFrameRate(FFrameRate(Fps, 1));
		Ctrl.SetNumberOfFrames(FFrameNumber(NumFrames - 1));

		for (const TPair<FName, TArray<FTransform>>& Track : Tracks)
		{
			TArray<FVector3f> PosKeys;
			TArray<FQuat4f> RotKeys;
			TArray<FVector3f> ScaleKeys;
			PosKeys.Reserve(NumFrames);
			RotKeys.Reserve(NumFrames);
			ScaleKeys.Reserve(NumFrames);
			for (const FTransform& T : Track.Value)
			{
				PosKeys.Add((FVector3f)T.GetLocation());
				RotKeys.Add((FQuat4f)T.GetRotation());
				ScaleKeys.Add((FVector3f)T.GetScale3D());
			}
			Ctrl.AddBoneCurve(Track.Key);
			Ctrl.SetBoneTrackKeys(Track.Key, PosKeys, RotKeys, ScaleKeys);
		}

		Ctrl.NotifyPopulated();
		Ctrl.CloseBracket();

		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Sequence);
		OutPackages.Add(Package);

		UE_LOG(LogCineDirectorBody, Display, TEXT("Baked body anim '%s': %d tracks, %d frames (%.1fs)"),
			*Anim.Name, Tracks.Num(), NumFrames, Anim.Duration);
		return Sequence;
	}

	void WritePreviewSheet(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim, const FString& OutDir)
	{
		const int32 Cell = 320;
		const int32 Cols = 4;
		const int32 Rows = 2;
		const int32 W = Cell * Cols;
		const int32 H = Cell * Rows;
		TArray<FColor> Canvas;
		Canvas.Init(FColor(24, 24, 28), W * H);

		const TSet<FName> Bones = CollectAnimBones(Rig, Anim);
		const float MaxZ = Rig.RefPos(Rig.Head).Z + 30.0f * Rig.Scale;
		const float Extent = MaxZ * 0.62f;

		FCineBodyPoseKey Pose;
		TArray<FTransform> Local, CS;
		for (int32 Col = 0; Col < Cols; ++Col)
		{
			const float Time = FMath::Fmod(Anim.Duration * ((float)Col / Cols + 0.12f), Anim.Duration);
			EvaluatePose(Rig, Anim, Time, Bones, Pose);
			SolvePose(Rig, Pose, Local, CS);

			for (int32 Row = 0; Row < Rows; ++Row)
			{
				const bool bFront = Row == 0;
				const int32 OriginX = Col * Cell;
				const int32 OriginY = Row * Cell;
				auto Project = [&](const FVector& P) -> FVector2D
				{
					const float U = bFront ? FVector::DotProduct(P, Rig.Right) : FVector::DotProduct(P, Rig.Fwd);
					const float V = P.Z;
					return FVector2D(
						OriginX + Cell * 0.5f + (U / Extent) * Cell * 0.42f,
						OriginY + Cell * 0.92f - (V / MaxZ) * Cell * 0.84f);
				};

				DrawLine(Canvas, W, H, FVector2D(OriginX + 8, OriginY + Cell * 0.92f),
					FVector2D(OriginX + Cell - 8, OriginY + Cell * 0.92f), FColor(70, 70, 75));

				for (int32 b = 0; b < Rig.RefSkel->GetNum(); ++b)
				{
					const int32 Parent = Rig.RefSkel->GetParentIndex(b);
					if (Parent == INDEX_NONE)
					{
						continue;
					}
					const FString Name = Rig.RefSkel->GetBoneName(b).ToString();
					FColor Color = FColor(225, 225, 225);
					if (Name.Contains(TEXT("Right")) || Name.EndsWith(TEXT("_R")))
					{
						Color = FColor(255, 120, 90);
					}
					else if (Name.Contains(TEXT("Left")) || Name.EndsWith(TEXT("_L")))
					{
						Color = FColor(90, 190, 255);
					}
					DrawLine(Canvas, W, H, Project(CS[Parent].GetLocation()), Project(CS[b].GetLocation()), Color);
					DrawDot(Canvas, W, H, Project(CS[b].GetLocation()), Color);
				}

				DrawDot(Canvas, W, H, Project(MouthTarget(Rig, Pose)), FColor(255, 220, 60));

				for (int32 Side = 0; Side < 2; ++Side)
				{
					const FVector RefPalm = HandPalmNormalRef(Rig, Side);
					if (RefPalm.IsNearlyZero())
					{
						continue;
					}
					const int32 WristIndex = Rig.Index(Rig.Wrist[Side]);
					const FQuat HandRot = CS[WristIndex].GetRotation() * Rig.RefCS[WristIndex].GetRotation().Inverse();
					const FVector P0 = CS[WristIndex].GetLocation();
					DrawLine(Canvas, W, H, Project(P0),
						Project(P0 + HandRot.RotateVector(RefPalm) * 12.0f * Rig.Scale), FColor(255, 80, 255));
				}
			}
		}

		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(W, H, Canvas, Png);
		IFileManager::Get().MakeDirectory(*OutDir, true);
		const FString FilePath = OutDir / FString::Printf(TEXT("%s_%s.png"), *Rig.Mesh->GetName(), *Anim.Name);
		TArray<uint8> Bytes(Png.GetData(), (int32)Png.Num());
		if (FFileHelper::SaveArrayToFile(Bytes, *FilePath))
		{
			UE_LOG(LogCineDirectorBody, Display, TEXT("Preview sheet: %s"), *FilePath);
		}
	}
}
