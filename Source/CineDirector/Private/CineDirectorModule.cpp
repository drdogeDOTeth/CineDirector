// Copyright Roundtree. All Rights Reserved.

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CineBodyAuthor.h"
#include "CineBodyGrammar.h"
#include "CineBodyRig.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SCineDirectorPanel.h"
#include "ShotGrammarParser.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "CineDirector"

static const FName CineDirectorTabName("CineDirector");

class FCineDirectorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Provider = MakeShared<FShotGrammarParser>();

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			CineDirectorTabName,
			FOnSpawnTab::CreateRaw(this, &FCineDirectorModule::SpawnCineDirectorTab))
			.SetDisplayName(LOCTEXT("TabTitle", "CineDirector"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Describe a shot in plain language; CineDirector builds the cameras, keys and camera cuts in the open Level Sequence."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetLevelEditorCinematicsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.CineCameraActor"));

		// Headless-friendly: CineDirector.AuthorBody <MeshName> <description...>
		// Bakes the asset + writes a stick-figure preview sheet, no panel needed.
		BodyCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("CineDirector.AuthorBody"),
			TEXT("Authors a prompted body performance for a skeletal mesh by name: CineDirector.AuthorBody DegenGrills sitting smoking nervous"),
			FConsoleCommandWithArgsDelegate::CreateStatic(&FCineDirectorModule::AuthorBodyCommand),
			ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		if (BodyCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(BodyCommand);
			BodyCommand = nullptr;
		}
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CineDirectorTabName);
		}
	}

	static void AuthorBodyCommand(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: CineDirector.AuthorBody <MeshName> <description...>"));
			return;
		}
		const FString MeshName = Args[0];
		FString Description;
		for (int32 i = 1; i < Args.Num(); ++i)
		{
			Description += (i > 1 ? TEXT(" ") : TEXT("")) + Args[i];
		}
		if (Description.IsEmpty())
		{
			Description = TEXT("standing idle");
		}

		const FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FARFilter Filter;
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		TArray<FAssetData> Meshes;
		AssetRegistryModule.Get().GetAssets(Filter, Meshes);
		USkeletalMesh* Mesh = nullptr;
		for (const FAssetData& Asset : Meshes)
		{
			if (Asset.AssetName.ToString().Equals(MeshName, ESearchCase::IgnoreCase)
				&& !Asset.PackagePath.ToString().Contains(TEXT("Backup")))
			{
				Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
				break;
			}
		}
		if (!Mesh)
		{
			UE_LOG(LogTemp, Error, TEXT("CineDirector.AuthorBody: mesh '%s' not found."), *MeshName);
			return;
		}

		FCineBodyRig Rig;
		FString Error;
		if (!FCineBodyRig::Analyze(Mesh, Rig, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("CineDirector.AuthorBody: %s"), *Error);
			return;
		}
		const FCineBodySpec Spec = FCineBodyGrammar::Parse(Description, GetTypeHash(MeshName));
		FCineBodyAnimDef Anim = FCineBodyAuthor::Build(Rig, Spec);
		Anim.Name = FString::Printf(TEXT("%s_%s"), *Mesh->GetName(), *FCineBodyAuthor::MakeSlug(Spec));

		TArray<UPackage*> Packages;
		if (CineBodyRigOps::Bake(Rig, Anim, TEXT("/Game/CineDirector/BodyAnims"), Packages, Error))
		{
			CineBodyRigOps::WritePreviewSheet(Rig, Anim, FPaths::ProjectSavedDir() / TEXT("CineDirectorBody"));
			UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("CineDirector.AuthorBody: %s"), *Error);
		}
	}

private:
	TSharedRef<SDockTab> SpawnCineDirectorTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SCineDirectorPanel)
				.Provider(Provider)
			];
	}

	TSharedPtr<IShotPlanProvider> Provider;
	IConsoleCommand* BodyCommand = nullptr;
};

IMPLEMENT_MODULE(FCineDirectorModule, CineDirector)

#undef LOCTEXT_NAMESPACE
