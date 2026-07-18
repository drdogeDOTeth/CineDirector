// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CineDirectorAutoRetargetTypes.h"

class USkeletalMesh;

/** Pure skeleton analysis used by the editor UI and automation tests. */
class FCineDirectorSkeletonAnalyzer
{
public:
	static bool Analyze(USkeletalMesh* SkeletalMesh, FCineDirectorSkeletonAnalysis& OutAnalysis, FText& OutError);

	/** Test-friendly entry point; positions are component-space reference-pose locations. */
	static FCineDirectorSkeletonAnalysis AnalyzeBoneData(
		const TArray<FName>& BoneNames,
		const TArray<int32>& ParentIndices,
		const TArray<FVector>& ComponentPositions);

	static FString MakeSkeletonSignature(const TArray<FName>& BoneNames, const TArray<int32>& ParentIndices);

	static bool IsValidDescendantPath(const TArray<int32>& ParentIndices, int32 StartIndex, int32 EndIndex);

private:
	struct FBone
	{
		FName Name;
		int32 ParentIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		int32 Depth = 0;
	};

	struct FSnapshot
	{
		TArray<FBone> Bones;
		TMap<FName, int32> IndexByName;
	};

	static FSnapshot MakeSnapshot(const TArray<FName>& BoneNames, const TArray<int32>& ParentIndices, const TArray<FVector>& Positions);
	static FString NormalizeBoneName(FName BoneName);
	static int32 FindBestBone(const FSnapshot& Snapshot, const FString& SemanticRole, float& OutConfidence, FString& OutEvidence);
	static int32 FindDeepestDescendant(const FSnapshot& Snapshot, int32 StartIndex);
	static bool IsDescendant(const FSnapshot& Snapshot, int32 StartIndex, int32 EndIndex);
	static FCineDirectorChainMapping MakeMapping(const FSnapshot& Snapshot, FName ChainName, const FString& StartRole, const FString& EndRole = FString());
	static ECineDirectorRigType Classify(const FSnapshot& Snapshot, float& OutConfidence, TArray<FString>& OutWarnings);
};
