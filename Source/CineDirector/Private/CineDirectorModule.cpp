// Copyright Roundtree. All Rights Reserved.

#include "CoreMinimal.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
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
	}

	virtual void ShutdownModule() override
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CineDirectorTabName);
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
};

IMPLEMENT_MODULE(FCineDirectorModule, CineDirector)

#undef LOCTEXT_NAMESPACE
