// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CineDirectorAutoRetargetTypes.h"

class UIKRigDefinition;
class USkeletalMesh;

struct FCineDirectorIKRigGenerationResult
{
	bool bSuccess = false;
	UIKRigDefinition* IKRig = nullptr;
	FText Error;
	FString AssetPath;
};

/** Creates safely named, saved IK Rig and profile assets from verified analysis. */
class FCineDirectorIKRigGenerator
{
public:
	static FCineDirectorIKRigGenerationResult GenerateIKRig(
		USkeletalMesh* SkeletalMesh,
		const FCineDirectorSkeletonAnalysis& Analysis,
		const FString& RequestedOutputPath);

	static UCineDirectorRetargetProfile* SaveProfile(
		const FCineDirectorSkeletonAnalysis& Analysis,
		const FString& RequestedOutputPath,
		FText& OutError);

private:
	static FString ResolveOutputPath(USkeletalMesh* SkeletalMesh, const FString& RequestedOutputPath, FText& OutError);
	static bool SaveAsset(UObject* Asset, FText& OutError);
};
