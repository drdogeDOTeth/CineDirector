// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class UPackage;
class USkeletalMesh;
struct FReferenceSkeleton;

/** Semantic joint roles — humanization dials key off these, not bone names. */
enum class ECineBodyJoint : uint8
{
	Hips, Spine, Chest, Neck, Head, Clavicle, UpperArm, Forearm, Wrist,
	Thigh, Shin, Foot, Toe, Finger, Eye, Other
};

/**
 * A skeletal mesh resolved onto semantic body joints. Supports the VRM-style
 * void rigs (Hips / Left-arm / IndexFinger1_L) and Blender-Mixamo rigs
 * (mixamorig_*). Facing is derived from the rig itself (toes, else eyes).
 */
struct FCineBodyRig
{
	USkeletalMesh* Mesh = nullptr;
	const FReferenceSkeleton* RefSkel = nullptr;
	TArray<FTransform> RefLocal; // reference local transforms
	TArray<FTransform> RefCS;    // reference component-space transforms

	FVector Fwd = FVector::ForwardVector;
	FVector Right = FVector::RightVector;
	FVector Up = FVector::UpVector;
	float Scale = 1.0f;     // ~1 at human size; scales cm offsets
	float ThighLen = 40.0f; // |knee - hip|
	FString SchemeName;

	// Semantic bones; side arrays are [0]=left, [1]=right. NAME_None = absent.
	FName Hips, Spine, Chest, Neck, Head;
	FName Eye[2];
	FName Shoulder[2], Arm[2], Elbow[2], Wrist[2];
	FName Leg[2], Knee[2], Ankle[2], Toe[2];
	/** Finger chains per hand, index→pinky order; each chain is segment bones. */
	TArray<TArray<FName>> Fingers[2];
	TArray<FName> Thumb[2];

	TMap<FName, ECineBodyJoint> Roles;

	int32 Index(const FName Bone) const { return RefSkel->FindBoneIndex(Bone); }
	bool Has(const FName Bone) const { return !Bone.IsNone() && Index(Bone) != INDEX_NONE; }
	FVector RefPos(const FName Bone) const { return RefCS[Index(Bone)].GetLocation(); }
	ECineBodyJoint RoleOf(const FName Bone) const
	{
		const ECineBodyJoint* Role = Roles.Find(Bone);
		return Role ? *Role : ECineBodyJoint::Other;
	}

	/** Resolve Mesh onto a known scheme. False + OutError if unsupported. */
	static bool Analyze(USkeletalMesh* InMesh, FCineBodyRig& Out, FString& OutError);
};

/**
 * One keyframe: world-space rotation deltas on reference component-space
 * orientations (unkeyed bones follow their parent rigidly; identity pins to
 * reference), plus parent-relative LocalDeltas used for finger curls.
 */
struct FCineBodyPoseKey
{
	float Time = 0.0f;
	TMap<FName, FQuat> Deltas;
	TMap<FName, FQuat> LocalDeltas;
	FVector HipsOffset = FVector::ZeroVector;

	FCineBodyPoseKey& Set(const FName Bone, const FQuat& Delta)
	{
		Deltas.Add(Bone, Delta);
		return *this;
	}
	FCineBodyPoseKey& Add(const FName Bone, const FQuat& Delta)
	{
		FQuat* Existing = Deltas.Find(Bone);
		Deltas.Add(Bone, Existing ? (Delta * *Existing).GetNormalized() : Delta);
		return *this;
	}
	FCineBodyPoseKey& SetLocal(const FName Bone, const FQuat& Delta)
	{
		LocalDeltas.Add(Bone, Delta);
		return *this;
	}
};

struct FCineBodyAnimDef
{
	FString Name;
	float Duration = 12.0f;
	float LagMul = 1.0f;       // scales per-joint follow-through lag
	float OvershootMul = 1.0f; // scales per-joint overshoot
	TArray<FCineBodyPoseKey> Keys; // sorted by Time; loops back to Keys[0]
	TFunction<void(float Time, FCineBodyPoseKey& InOut)> Procedural;
};

/** Pose/IK/bake mechanics shared by the author and the preview writer. */
namespace CineBodyRigOps
{
	FQuat RotAbout(const FVector& Axis, float Degrees);

	/** Reference-pose palm normal (geometric, thumb-disambiguated). Zero if no fingers. */
	FVector HandPalmNormalRef(const FCineBodyRig& Rig, int32 Side);

	/** Interpolated pose at Time with per-joint lag/overshoot and layers applied. */
	void EvaluatePose(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim, float Time,
		const TSet<FName>& AllBones, FCineBodyPoseKey& Out);

	/** Component-space FK solve of one pose. */
	void SolvePose(const FCineBodyRig& Rig, const FCineBodyPoseKey& Pose,
		TArray<FTransform>& OutLocal, TArray<FTransform>& OutCS);

	/**
	 * Two-bone arm IK writing upper/forearm/wrist deltas into Key. Palm twist
	 * about the forearm axis when PalmTowards is non-zero; WristDroopDeg breaks
	 * the hand off the forearm line (positive = toward gravity).
	 */
	void SolveArmIK(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, int32 Side,
		const FVector& Target, const FVector& PoleDir, float WristDroopDeg = 14.0f,
		const FVector& PalmTowards = FVector::ZeroVector);

	/** Finger curls (parent-relative). bCigGrip: index/middle V-hold, rest curled. */
	void CurlFingers(const FCineBodyRig& Rig, FCineBodyPoseKey& Key, int32 Side,
		float CurlDeg, float ThumbDeg, bool bCigGrip);

	/** Mouth position for the current key's pose (eyes if present, else head offset). */
	FVector MouthTarget(const FCineBodyRig& Rig, const FCineBodyPoseKey& Key);

	/** Every bone any key or the standard procedural layers touch. */
	TSet<FName> CollectAnimBones(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim);

	/** Bake to a bone-track UAnimSequence asset under PackageFolder. */
	UAnimSequence* Bake(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim,
		const FString& PackageFolder, TArray<UPackage*>& OutPackages, FString& OutError);

	/** Stick-figure contact sheet (front+side x 4 times, palm markers) to OutDir. */
	void WritePreviewSheet(const FCineBodyRig& Rig, const FCineBodyAnimDef& Anim, const FString& OutDir);
}
