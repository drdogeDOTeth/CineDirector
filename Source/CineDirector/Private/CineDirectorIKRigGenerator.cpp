// Copyright Roundtree. All Rights Reserved.

#include "CineDirectorIKRigGenerator.h"
#include "CineDirectorSkeletonAnalyzer.h"

#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Factories/DataAssetFactory.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"

FCineDirectorIKRigGenerationResult FCineDirectorIKRigGenerator::GenerateIKRig(
	USkeletalMesh* SkeletalMesh,
	const FCineDirectorSkeletonAnalysis& Analysis,
	const FString& RequestedOutputPath)
{
	FCineDirectorIKRigGenerationResult Result;
	if (!SkeletalMesh)
	{
		Result.Error = FText::FromString(TEXT("No Skeletal Mesh is selected."));
		return Result;
	}
	if (Analysis.RigType == ECineDirectorRigType::Unknown)
	{
		Result.Error = FText::FromString(TEXT("Choose a supported rig type and review its mappings before generating."));
		return Result;
	}
	if (Analysis.RetargetRoot.IsNone())
	{
		Result.Error = FText::FromString(TEXT("A retarget pelvis/root is required before generating an IK Rig."));
		return Result;
	}

	FText PathError;
	const FString OutputPath = ResolveOutputPath(SkeletalMesh, RequestedOutputPath, PathError);
	if (OutputPath.IsEmpty())
	{
		Result.Error = PathError;
		return Result;
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	FString UniquePackageName;
	FString UniqueAssetName;
	AssetTools.CreateUniqueAssetName(
		OutputPath / FString::Printf(TEXT("IK_%s"), *SkeletalMesh->GetName()),
		TEXT(""),
		UniquePackageName,
		UniqueAssetName);

	UIKRigDefinition* IKRig = UIKRigDefinitionFactory::CreateNewIKRigAsset(
		FPackageName::GetLongPackagePath(UniquePackageName),
		UniqueAssetName);
	if (!IKRig)
	{
		Result.Error = FText::FromString(TEXT("Unreal could not create the IK Rig asset."));
		return Result;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller || !Controller->SetSkeletalMesh(SkeletalMesh))
	{
		Result.Error = FText::FromString(TEXT("The generated IK Rig could not use the selected Skeletal Mesh."));
		return Result;
	}

	if (!Controller->SetRetargetRoot(Analysis.RetargetRoot))
	{
		Result.Error = FText::FromString(TEXT("The selected retarget pelvis/root is not a valid skeleton bone."));
		return Result;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	if (RefSkeleton.GetNum() > 0)
	{
		Controller->SetRootMotionBone(RefSkeleton.GetBoneName(0));
	}

	TArray<int32> ParentIndices;
ParentIndices.Reserve(RefSkeleton.GetNum());
for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
{
    ParentIndices.Add(RefSkeleton.GetRawParentIndex(BoneIndex));
}

int32 ChainsCreated = 0;
	for (const FCineDirectorChainMapping& Mapping : Analysis.ChainMappings)
	{
		if (!Mapping.bValidHierarchy || Mapping.StartBone.IsNone() || Mapping.EndBone.IsNone())
		{
			continue;
		}

		const int32 StartIndex = RefSkeleton.FindBoneIndex(Mapping.StartBone);
		const int32 EndIndex = RefSkeleton.FindBoneIndex(Mapping.EndBone);
		if (!FCineDirectorSkeletonAnalyzer::IsValidDescendantPath(
			ParentIndices, StartIndex, EndIndex))
		{
			continue;
		}

		if (!Controller->AddRetargetChain(Mapping.ChainName, Mapping.StartBone, Mapping.EndBone, NAME_None).IsNone())
		{
			++ChainsCreated;
		}
	}

	if (ChainsCreated == 0)
	{
		Result.Error = FText::FromString(TEXT("No valid retarget chains were available to create. Review the mappings and try again."));
		return Result;
	}

	IKRig->MarkPackageDirty();
	FText SaveError;
	if (!SaveAsset(IKRig, SaveError))
	{
		Result.Error = SaveError;
		return Result;
	}

	Result.bSuccess = true;
	Result.IKRig = IKRig;
	Result.AssetPath = IKRig->GetPathName();
	return Result;
}

UCineDirectorRetargetProfile* FCineDirectorIKRigGenerator::SaveProfile(
	const FCineDirectorSkeletonAnalysis& Analysis,
	const FString& RequestedOutputPath,
	FText& OutError)
{
	if (Analysis.SkeletonSignature.IsEmpty())
	{
		OutError = FText::FromString(TEXT("Analyze a Skeletal Mesh before saving a profile."));
		return nullptr;
	}

	FString OutputPath = RequestedOutputPath.TrimStartAndEnd();
	if (OutputPath.IsEmpty())
	{
		OutputPath = TEXT("/Game/CineDirector/RetargetProfiles");
	}
	if (!OutputPath.StartsWith(TEXT("/Game/")))
	{
		OutError = FText::FromString(TEXT("Profile output path must be inside /Game/."));
		return nullptr;
	}

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	FString UniquePackageName;
	FString UniqueAssetName;
	AssetTools.CreateUniqueAssetName(OutputPath / TEXT("CineDirectorRetargetProfile"), TEXT(""), UniquePackageName, UniqueAssetName);

	UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
	Factory->DataAssetClass = UCineDirectorRetargetProfile::StaticClass();
	UCineDirectorRetargetProfile* Profile = Cast<UCineDirectorRetargetProfile>(
		AssetTools.CreateAsset(UniqueAssetName, FPackageName::GetLongPackagePath(UniquePackageName), UCineDirectorRetargetProfile::StaticClass(), Factory));
	if (!Profile)
	{
		OutError = FText::FromString(TEXT("Unreal could not create the retarget profile asset."));
		return nullptr;
	}

	Profile->RigType = Analysis.RigType;
	Profile->RetargetRoot = Analysis.RetargetRoot;
	Profile->SkeletonSignature = Analysis.SkeletonSignature;
	Profile->ChainMappings = Analysis.ChainMappings;
	Profile->MarkPackageDirty();
	if (!SaveAsset(Profile, OutError))
	{
		return nullptr;
	}
	return Profile;
}

FString FCineDirectorIKRigGenerator::ResolveOutputPath(USkeletalMesh* SkeletalMesh, const FString& RequestedOutputPath, FText& OutError)
{
	FString OutputPath = RequestedOutputPath.TrimStartAndEnd();
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPackageName::GetLongPackagePath(SkeletalMesh->GetOutermost()->GetName());
	}
	if (!OutputPath.StartsWith(TEXT("/Game/")))
	{
		OutError = FText::FromString(TEXT("Output path must be inside /Game/."));
		return FString();
	}
	return OutputPath;
}

bool FCineDirectorIKRigGenerator::SaveAsset(UObject* Asset, FText& OutError)
{
	if (!Asset)
	{
		OutError = FText::FromString(TEXT("No asset was available to save."));
		return false;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Asset->GetOutermost());
	if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
	{
		OutError = FText::FromString(TEXT("The asset was created but could not be saved. Check source control and the Output Log."));
		return false;
	}
	return true;
}
