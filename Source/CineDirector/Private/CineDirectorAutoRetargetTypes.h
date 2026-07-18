// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CineDirectorAutoRetargetTypes.generated.h"

class USkeletalMesh;

UENUM(BlueprintType)
enum class ECineDirectorRigType : uint8
{
	Unknown,
	Humanoid,
	Quadruped,
};

USTRUCT(BlueprintType)
struct FCineDirectorBoneCandidate
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	FName Bone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	float Confidence = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	FString Evidence;
};

USTRUCT(BlueprintType)
struct FCineDirectorChainMapping
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FName ChainName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FName StartBone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FName EndBone;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	float Confidence = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	bool bValidHierarchy = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	FString Warning;
};

USTRUCT(BlueprintType)
struct FCineDirectorSkeletonAnalysis
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	TSoftObjectPtr<USkeletalMesh> SkeletalMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	ECineDirectorRigType RigType = ECineDirectorRigType::Unknown;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	float OverallConfidence = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FName RetargetRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	FString SkeletonSignature;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	TArray<FCineDirectorChainMapping> ChainMappings;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Auto Retarget")
	TArray<FString> Warnings;
};

/** Reusable, user-verified mapping for a skeleton family. */
UCLASS(BlueprintType)
class CINEDIRECTOR_API UCineDirectorRetargetProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	ECineDirectorRigType RigType = ECineDirectorRigType::Unknown;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FName RetargetRoot;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	FString SkeletonSignature;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	TArray<FCineDirectorChainMapping> ChainMappings;

	/** Explicit user changes, retained for auditability and future automatic reuse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Auto Retarget")
	TMap<FName, FName> UserCorrections;
};
