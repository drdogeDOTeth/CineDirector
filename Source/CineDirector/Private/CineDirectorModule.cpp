// Copyright Roundtree. All Rights Reserved.

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CineBodyAuthor.h"
#include "CineBodyGrammar.h"
#include "CineBodyRig.h"
#include "CineFaceBaker.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "LlmShotPlanProvider.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SCineDirectorPanel.h"
#include "ShotGrammarParser.h"
#include "ShotPlanExecutor.h"
#include "ShotPlanJson.h"
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
		// The router picks the offline parser or a model backend per request, so the
		// Project Settings choice applies without restarting the editor.
		Provider = MakeShared<FShotPlanProviderRouter>();

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

		// Read-only: asks the configured backend for a plan and logs it without
		// spawning cameras, so a backend can be checked without Sequencer open.
		PlanCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("CineDirector.PlanShots"),
			TEXT("Plans shots from a description and logs the result without executing it: CineDirector.PlanShots slow orbit around the hero, 85mm"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FCineDirectorModule::PlanShotsCommand),
			ECVF_Default);

		// Same referencer-checked sweep as the Face panel button, from the console.
		// Editor-only in practice: it needs a fully scanned asset registry, which
		// rules out headless startup, and -ExecCmds cannot reach it either (commandlets
		// ignore ExecCmds, and a -nullrhi editor boot has no UWorld for the exec path).
		PurgeFaceCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("CineDirector.PurgeFaceAnims"),
			TEXT("Deletes face anims under /Game/CineDirector/FaceAnims that no sequence or map references: CineDirector.PurgeFaceAnims"),
			FConsoleCommandWithArgsDelegate::CreateStatic(&FCineDirectorModule::PurgeFaceAnimsCommand),
			ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		if (BodyCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(BodyCommand);
			BodyCommand = nullptr;
		}
		if (PlanCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(PlanCommand);
			PlanCommand = nullptr;
		}
		if (PurgeFaceCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(PurgeFaceCommand);
			PurgeFaceCommand = nullptr;
		}
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CineDirectorTabName);
		}
	}

	/** CineDirector.PlanShots <description...> — plan and log, change nothing. */
	void PlanShotsCommand(const TArray<FString>& Args)
	{
		const FString Description = FString::Join(Args, TEXT(" "));
		if (Description.IsEmpty() || !Provider.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: CineDirector.PlanShots <description...>"));
			return;
		}

		const FCineSceneContext Scene = FShotPlanExecutor::BuildSceneContext();
		UE_LOG(LogTemp, Display, TEXT("CineDirector.PlanShots: asking %s for \"%s\" (%d actors in scene)"),
			*Provider->GetProviderName().ToString(), *Description, Scene.Actors.Num());

		Provider->BuildShotPlanAsync(Description, Scene,
			FCineShotPlanReady::CreateLambda(
				[](bool bSuccess, const FCineShotPlan& Plan, const FText& Error)
				{
					if (!bSuccess)
					{
						UE_LOG(LogTemp, Error, TEXT("CineDirector.PlanShots failed: %s"), *Error.ToString());
						return;
					}
					UE_LOG(LogTemp, Display, TEXT("CineDirector.PlanShots result — %s"),
						*FCineShotPlanJson::DescribePlan(Plan));
				}));
	}

	/** CineDirector.PurgeFaceAnims — delete face anims nothing references. */
	static void PurgeFaceAnimsCommand(const TArray<FString>& Args)
	{
		// A partial registry reports no referencers, which would make a take that IS
		// used by a sequence look orphaned and get deleted. Block on any in-flight
		// scan first. Note this waits for a requested scan, it does not start one.
		FAssetRegistryModule& RegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		RegistryModule.Get().WaitForCompletion();

		FString Message;
		FCineFaceBaker::PurgeUnusedFaceAnims(Message);
		UE_LOG(LogTemp, Display, TEXT("CineDirector.PurgeFaceAnims: %s"), *Message);
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
			if (Packages.Num() > 0)
			{
				UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
			}
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
	IConsoleCommand* PlanCommand = nullptr;
	IConsoleCommand* PurgeFaceCommand = nullptr;
};

IMPLEMENT_MODULE(FCineDirectorModule, CineDirector)

#undef LOCTEXT_NAMESPACE
