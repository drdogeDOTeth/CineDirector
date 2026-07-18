// Copyright Roundtree. All Rights Reserved.

#include "CineDirectorSkeletonAnalyzer.h"

#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMesh.h"

namespace CineDirectorAutoRetarget
{
	static TArray<FString> GetAliases(const FString& Role)
	{
		if (Role == TEXT("Pelvis")) return { TEXT("pelvis"), TEXT("hips"), TEXT("hip"), TEXT("root") };
		if (Role == TEXT("Spine")) return { TEXT("spine"), TEXT("spine01"), TEXT("spine1"), TEXT("back") };
		if (Role == TEXT("Chest")) return { TEXT("chest"), TEXT("spine02"), TEXT("spine2"), TEXT("upperchest") };
		if (Role == TEXT("Neck")) return { TEXT("neck") };
		if (Role == TEXT("Head")) return { TEXT("head") };
		if (Role == TEXT("LeftUpperArm")) return { TEXT("upperarml"), TEXT("arm_l"), TEXT("leftarm"), TEXT("shoulderl"), TEXT("claviclel") };
		if (Role == TEXT("RightUpperArm")) return { TEXT("upperarmr"), TEXT("arm_r"), TEXT("rightarm"), TEXT("shoulderr"), TEXT("clavicler") };
		if (Role == TEXT("LeftHand")) return { TEXT("handl"), TEXT("lefthand"), TEXT("wristl") };
		if (Role == TEXT("RightHand")) return { TEXT("handr"), TEXT("righthand"), TEXT("wristr") };
		if (Role == TEXT("LeftUpperLeg")) return { TEXT("thighl"), TEXT("upperlegl"), TEXT("leg_l"), TEXT("leftleg") };
		if (Role == TEXT("RightUpperLeg")) return { TEXT("thighr"), TEXT("upperlegr"), TEXT("leg_r"), TEXT("rightleg") };
		if (Role == TEXT("LeftFoot")) return { TEXT("footl"), TEXT("leftfoot"), TEXT("anklel") };
		if (Role == TEXT("RightFoot")) return { TEXT("footr"), TEXT("rightfoot"), TEXT("ankler") };
		if (Role == TEXT("FrontLeft")) return { TEXT("frontleft"), TEXT("front_l"), TEXT("forelegl"), TEXT("forel"), TEXT("frontlegl") };
		if (Role == TEXT("FrontRight")) return { TEXT("frontright"), TEXT("front_r"), TEXT("forelegr"), TEXT("forer"), TEXT("frontlegr") };
		if (Role == TEXT("BackLeft")) return { TEXT("backleft"), TEXT("hindlegl"), TEXT("rearlegl"), TEXT("back_l") };
		if (Role == TEXT("BackRight")) return { TEXT("backright"), TEXT("hindlegr"), TEXT("rearlegr"), TEXT("back_r") };
		if (Role == TEXT("Tail")) return { TEXT("tail"), TEXT("tail01"), TEXT("tail1") };
		if (Role == TEXT("LeftThumb")) return { TEXT("thumbl"), TEXT("leftthumb") };
		if (Role == TEXT("RightThumb")) return { TEXT("thumbr"), TEXT("rightthumb") };
		if (Role == TEXT("LeftIndex")) return { TEXT("indexl"), TEXT("leftindex") };
		if (Role == TEXT("RightIndex")) return { TEXT("indexr"), TEXT("rightindex") };
		if (Role == TEXT("LeftMiddle")) return { TEXT("middlel"), TEXT("leftmiddle") };
		if (Role == TEXT("RightMiddle")) return { TEXT("middler"), TEXT("rightmiddle") };
		if (Role == TEXT("LeftRing")) return { TEXT("ringl"), TEXT("leftring") };
		if (Role == TEXT("RightRing")) return { TEXT("ringr"), TEXT("rightring") };
		if (Role == TEXT("LeftPinky")) return { TEXT("pinkyl"), TEXT("littlel"), TEXT("leftpinky") };
		if (Role == TEXT("RightPinky")) return { TEXT("pinkyr"), TEXT("littler"), TEXT("rightpinky") };
		return {};
	}

	static bool IsLeftRole(const FString& Role) { return Role.StartsWith(TEXT("Left")) || Role == TEXT("FrontLeft") || Role == TEXT("BackLeft"); }
	static bool IsRightRole(const FString& Role) { return Role.StartsWith(TEXT("Right")) || Role == TEXT("FrontRight") || Role == TEXT("BackRight"); }
	static bool HasLeftMarker(const FString& Name) { return Name.Contains(TEXT("left")) || Name.EndsWith(TEXT("l")); }
	static bool HasRightMarker(const FString& Name) { return Name.Contains(TEXT("right")) || Name.EndsWith(TEXT("r")); }
}

bool FCineDirectorSkeletonAnalyzer::Analyze(USkeletalMesh* SkeletalMesh, FCineDirectorSkeletonAnalysis& OutAnalysis, FText& OutError)
{
	if (!SkeletalMesh)
	{
		OutError = FText::FromString(TEXT("Select a Skeletal Mesh in the Content Browser first."));
		return false;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const int32 Count = RefSkeleton.GetNum();
	if (Count == 0)
	{
		OutError = FText::FromString(TEXT("The selected Skeletal Mesh has no reference skeleton."));
		return false;
	}

	TArray<FName> Names;
	TArray<int32> Parents;
	TArray<FVector> ComponentPositions;
	Names.Reserve(Count);
	Parents.Reserve(Count);
	ComponentPositions.SetNum(Count);

	const TArray<FTransform>& LocalPose = RefSkeleton.GetRefBonePose();
	TArray<FTransform> ComponentPose;
	ComponentPose.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Names.Add(RefSkeleton.GetBoneName(Index));
		Parents.Add(RefSkeleton.GetRawParentIndex(Index));
		const int32 Parent = Parents[Index];
		ComponentPose[Index] = Parent != INDEX_NONE ? LocalPose[Index] * ComponentPose[Parent] : LocalPose[Index];
		ComponentPositions[Index] = ComponentPose[Index].GetLocation();
	}

	OutAnalysis = AnalyzeBoneData(Names, Parents, ComponentPositions);
	OutAnalysis.SkeletalMesh = SkeletalMesh;
	return true;
}

FCineDirectorSkeletonAnalysis FCineDirectorSkeletonAnalyzer::AnalyzeBoneData(
	const TArray<FName>& BoneNames,
	const TArray<int32>& ParentIndices,
	const TArray<FVector>& ComponentPositions)
{
	FCineDirectorSkeletonAnalysis Result;
	const FSnapshot Snapshot = MakeSnapshot(BoneNames, ParentIndices, ComponentPositions);
	Result.SkeletonSignature = MakeSkeletonSignature(BoneNames, ParentIndices);
	Result.RigType = Classify(Snapshot, Result.OverallConfidence, Result.Warnings);

	auto Add = [&Result, &Snapshot](const TCHAR* ChainName, const TCHAR* Start, const TCHAR* End = TEXT(""))
	{
		Result.ChainMappings.Add(MakeMapping(Snapshot, FName(ChainName), Start, End));
	};

	if (Result.RigType == ECineDirectorRigType::Humanoid)
	{
		Add(TEXT("Pelvis"), TEXT("Pelvis"), TEXT("SELF"));
		Add(TEXT("Spine"), TEXT("Spine"), TEXT("Chest"));
		Add(TEXT("Neck"), TEXT("Neck"), TEXT("SELF"));
		Add(TEXT("Head"), TEXT("Head"), TEXT("SELF"));
		Add(TEXT("LeftArm"), TEXT("LeftUpperArm"), TEXT("LeftHand"));
		Add(TEXT("RightArm"), TEXT("RightUpperArm"), TEXT("RightHand"));
		Add(TEXT("LeftLeg"), TEXT("LeftUpperLeg"), TEXT("LeftFoot"));
		Add(TEXT("RightLeg"), TEXT("RightUpperLeg"), TEXT("RightFoot"));
		Add(TEXT("LeftThumb"), TEXT("LeftThumb"));
		Add(TEXT("LeftIndex"), TEXT("LeftIndex"));
		Add(TEXT("LeftMiddle"), TEXT("LeftMiddle"));
		Add(TEXT("LeftRing"), TEXT("LeftRing"));
		Add(TEXT("LeftPinky"), TEXT("LeftPinky"));
		Add(TEXT("RightThumb"), TEXT("RightThumb"));
		Add(TEXT("RightIndex"), TEXT("RightIndex"));
		Add(TEXT("RightMiddle"), TEXT("RightMiddle"));
		Add(TEXT("RightRing"), TEXT("RightRing"));
		Add(TEXT("RightPinky"), TEXT("RightPinky"));
	}
	else if (Result.RigType == ECineDirectorRigType::Quadruped)
	{
		Add(TEXT("Pelvis"), TEXT("Pelvis"), TEXT("SELF"));
		Add(TEXT("Spine"), TEXT("Spine"), TEXT("Chest"));
		Add(TEXT("Neck"), TEXT("Neck"), TEXT("SELF"));
		Add(TEXT("Head"), TEXT("Head"), TEXT("SELF"));
		Add(TEXT("FrontLeft"), TEXT("FrontLeft"));
		Add(TEXT("FrontRight"), TEXT("FrontRight"));
		Add(TEXT("BackLeft"), TEXT("BackLeft"));
		Add(TEXT("BackRight"), TEXT("BackRight"));
		Add(TEXT("Tail"), TEXT("Tail"));
	}

	for (const FCineDirectorChainMapping& Mapping : Result.ChainMappings)
	{
		if (Mapping.ChainName == TEXT("Pelvis") && Mapping.bValidHierarchy)
		{
			Result.RetargetRoot = Mapping.StartBone;
			break;
		}
	}
	if (Result.RetargetRoot.IsNone())
	{
		Result.Warnings.Add(TEXT("Missing pelvis/root candidate; choose a retarget pelvis before generating."));
	}
	return Result;
}

FString FCineDirectorSkeletonAnalyzer::MakeSkeletonSignature(const TArray<FName>& BoneNames, const TArray<int32>& ParentIndices)
{
	uint32 Hash = 0;
	for (int32 Index = 0; Index < BoneNames.Num(); ++Index)
	{
		Hash = HashCombineFast(Hash, GetTypeHash(NormalizeBoneName(BoneNames[Index])));
		Hash = HashCombineFast(Hash, GetTypeHash(ParentIndices.IsValidIndex(Index) ? ParentIndices[Index] : INDEX_NONE));
	}
	return FString::Printf(TEXT("CineDirector_%d_%08X"), BoneNames.Num(), Hash);
}

bool FCineDirectorSkeletonAnalyzer::IsValidDescendantPath(const TArray<int32>& ParentIndices, int32 StartIndex, int32 EndIndex)
{
	if (StartIndex == INDEX_NONE || EndIndex == INDEX_NONE)
	{
		return false;
	}
	for (int32 Current = EndIndex; Current != INDEX_NONE; Current = ParentIndices.IsValidIndex(Current) ? ParentIndices[Current] : INDEX_NONE)
	{
		if (Current == StartIndex)
		{
			return true;
		}
	}
	return false;
}

FCineDirectorSkeletonAnalyzer::FSnapshot FCineDirectorSkeletonAnalyzer::MakeSnapshot(
	const TArray<FName>& BoneNames, const TArray<int32>& ParentIndices, const TArray<FVector>& Positions)
{
	FSnapshot Snapshot;
	for (int32 Index = 0; Index < BoneNames.Num(); ++Index)
	{
		FBone Bone;
		Bone.Name = BoneNames[Index];
		Bone.ParentIndex = ParentIndices.IsValidIndex(Index) ? ParentIndices[Index] : INDEX_NONE;
		Bone.Position = Positions.IsValidIndex(Index) ? Positions[Index] : FVector::ZeroVector;
		Bone.Depth = Bone.ParentIndex != INDEX_NONE && Snapshot.Bones.IsValidIndex(Bone.ParentIndex) ? Snapshot.Bones[Bone.ParentIndex].Depth + 1 : 0;
		Snapshot.IndexByName.Add(Bone.Name, Index);
		Snapshot.Bones.Add(Bone);
	}
	return Snapshot;
}

FString FCineDirectorSkeletonAnalyzer::NormalizeBoneName(FName BoneName)
{
	FString Result = BoneName.ToString().ToLower();
	Result.ReplaceInline(TEXT("mixamorig:"), TEXT(""));
	Result.ReplaceInline(TEXT("mixamorig"), TEXT(""));
	Result.ReplaceInline(TEXT("armature"), TEXT(""));
	Result.ReplaceInline(TEXT("skeleton"), TEXT(""));
	Result.ReplaceInline(TEXT("_"), TEXT(""));
	Result.ReplaceInline(TEXT("-"), TEXT(""));
	Result.ReplaceInline(TEXT("."), TEXT(""));
	return Result;
}

int32 FCineDirectorSkeletonAnalyzer::FindBestBone(const FSnapshot& Snapshot, const FString& SemanticRole, float& OutConfidence, FString& OutEvidence)
{
	OutConfidence = 0.0f;
	OutEvidence.Reset();
	const TArray<FString> Aliases = CineDirectorAutoRetarget::GetAliases(SemanticRole);
	int32 BestIndex = INDEX_NONE;

	for (int32 Index = 0; Index < Snapshot.Bones.Num(); ++Index)
	{
		const FBone& Bone = Snapshot.Bones[Index];
		const FString Normalized = NormalizeBoneName(Bone.Name);
		float Score = 0.0f;
		for (const FString& Alias : Aliases)
		{
			const FString CleanAlias = NormalizeBoneName(FName(Alias));
			if (Normalized == CleanAlias) Score = FMath::Max(Score, 0.80f);
			else if (Normalized.StartsWith(CleanAlias) || Normalized.EndsWith(CleanAlias)) Score = FMath::Max(Score, 0.68f);
			else if (Normalized.Contains(CleanAlias)) Score = FMath::Max(Score, 0.55f);
		}

		if (CineDirectorAutoRetarget::IsLeftRole(SemanticRole))
		{
			Score += CineDirectorAutoRetarget::HasLeftMarker(Normalized) ? 0.15f : (CineDirectorAutoRetarget::HasRightMarker(Normalized) ? -0.40f : 0.0f);
		}
		if (CineDirectorAutoRetarget::IsRightRole(SemanticRole))
		{
			Score += CineDirectorAutoRetarget::HasRightMarker(Normalized) ? 0.15f : (CineDirectorAutoRetarget::HasLeftMarker(Normalized) ? -0.40f : 0.0f);
		}

		// Depth, descendants, and reference-pose position make aliases less brittle on imported rigs.
		if (SemanticRole.Contains(TEXT("Finger")) || SemanticRole.Contains(TEXT("Thumb")) || SemanticRole.Contains(TEXT("Index")) || SemanticRole.Contains(TEXT("Middle")) || SemanticRole.Contains(TEXT("Ring")) || SemanticRole.Contains(TEXT("Pinky")))
		{
			Score += Bone.Depth >= 4 ? 0.05f : 0.0f;
		}
		if (SemanticRole == TEXT("Head") || SemanticRole == TEXT("Neck"))
		{
			Score += Bone.Position.Z > 0.0f ? 0.04f : 0.0f;
		}
		if (Score > OutConfidence)
		{
			OutConfidence = FMath::Clamp(Score, 0.0f, 1.0f);
			BestIndex = Index;
			OutEvidence = FString::Printf(TEXT("alias/hierarchy score %.0f%%"), OutConfidence * 100.0f);
		}
	}
	return BestIndex;
}

int32 FCineDirectorSkeletonAnalyzer::FindDeepestDescendant(const FSnapshot& Snapshot, int32 StartIndex)
{
	int32 Best = StartIndex;
	for (int32 Index = 0; Index < Snapshot.Bones.Num(); ++Index)
	{
		if (IsDescendant(Snapshot, StartIndex, Index) && Snapshot.Bones[Index].Depth > Snapshot.Bones[Best].Depth)
		{
			Best = Index;
		}
	}
	return Best;
}

bool FCineDirectorSkeletonAnalyzer::IsDescendant(const FSnapshot& Snapshot, int32 StartIndex, int32 EndIndex)
{
	for (int32 Current = EndIndex; Current != INDEX_NONE; Current = Snapshot.Bones.IsValidIndex(Current) ? Snapshot.Bones[Current].ParentIndex : INDEX_NONE)
	{
		if (Current == StartIndex) return true;
	}
	return false;
}

FCineDirectorChainMapping FCineDirectorSkeletonAnalyzer::MakeMapping(const FSnapshot& Snapshot, FName ChainName, const FString& StartRole, const FString& EndRole)
{
	FCineDirectorChainMapping Mapping;
	Mapping.ChainName = ChainName;

	float StartScore = 0.0f;
	FString StartEvidence;
	int32 Start = FindBestBone(Snapshot, StartRole, StartScore, StartEvidence);
	if (Start == INDEX_NONE || StartScore < 0.45f)
	{
		Mapping.Warning = FString::Printf(TEXT("No confident %s candidate; review before generation."), *StartRole);
		return Mapping;
	}

	int32 End = Start;
	float EndScore = StartScore;
	if (EndRole == TEXT("SELF"))
	{
		End = Start;
	}
	else if (!EndRole.IsEmpty())
	{
		FString EndEvidence;
		End = FindBestBone(Snapshot, EndRole, EndScore, EndEvidence);
		if (End == INDEX_NONE || EndScore < 0.45f)
		{
			End = FindDeepestDescendant(Snapshot, Start);
			EndScore = StartScore * 0.75f;
			Mapping.Warning = FString::Printf(TEXT("No confident %s endpoint; using deepest descendant."), *EndRole);
		}
	}
	else
	{
		End = FindDeepestDescendant(Snapshot, Start);
		EndScore = StartScore * 0.80f;
	}

	if (!IsDescendant(Snapshot, Start, End))
	{
		if (IsDescendant(Snapshot, End, Start))
		{
			Swap(Start, End);
			Mapping.Warning = TEXT("Hierarchy ran opposite the semantic labels; chain direction was reversed.");
		}
		else
		{
			End = Start;
			Mapping.Warning = TEXT("Semantic endpoints are not connected; reduced to a valid single-bone chain.");
		}
	}

	Mapping.StartBone = Snapshot.Bones[Start].Name;
	Mapping.EndBone = Snapshot.Bones[End].Name;
	Mapping.Confidence = (StartScore + EndScore) * 0.5f;
	Mapping.bValidHierarchy = true;
	if (Start == End && Mapping.Warning.IsEmpty())
	{
		Mapping.Warning = TEXT("Single-bone chain.");
	}
	return Mapping;
}

ECineDirectorRigType FCineDirectorSkeletonAnalyzer::Classify(const FSnapshot& Snapshot, float& OutConfidence, TArray<FString>& OutWarnings)
{
	auto ScoreRole = [&Snapshot](const TCHAR* Role)
	{
		float Score = 0.0f; FString Evidence;
		FindBestBone(Snapshot, Role, Score, Evidence);
		return Score;
	};

	const float Tail = ScoreRole(TEXT("Tail"));
	const int32 QuadLegs =
		(ScoreRole(TEXT("FrontLeft")) > 0.50f) + (ScoreRole(TEXT("FrontRight")) > 0.50f) +
		(ScoreRole(TEXT("BackLeft")) > 0.50f) + (ScoreRole(TEXT("BackRight")) > 0.50f);
	const int32 HumanoidLimbs =
		(ScoreRole(TEXT("LeftUpperArm")) > 0.50f) + (ScoreRole(TEXT("RightUpperArm")) > 0.50f) +
		(ScoreRole(TEXT("LeftUpperLeg")) > 0.50f) + (ScoreRole(TEXT("RightUpperLeg")) > 0.50f);
	const int32 Fingers =
		(ScoreRole(TEXT("LeftThumb")) > 0.50f) + (ScoreRole(TEXT("RightThumb")) > 0.50f) +
		(ScoreRole(TEXT("LeftIndex")) > 0.50f) + (ScoreRole(TEXT("RightIndex")) > 0.50f);

	if (Tail > 0.50f && QuadLegs >= 2)
	{
		OutConfidence = FMath::Clamp((Tail + QuadLegs * 0.20f) / 1.4f, 0.0f, 1.0f);
		return ECineDirectorRigType::Quadruped;
	}
	if (HumanoidLimbs >= 3 || (HumanoidLimbs >= 2 && Fingers >= 2))
	{
		OutConfidence = FMath::Clamp((HumanoidLimbs * 0.20f + Fingers * 0.08f) / 0.96f, 0.0f, 1.0f);
		return ECineDirectorRigType::Humanoid;
	}

	OutConfidence = 0.25f;
	OutWarnings.Add(TEXT("Rig type is ambiguous. Choose mappings manually before generation."));
	return ECineDirectorRigType::Unknown;
}
