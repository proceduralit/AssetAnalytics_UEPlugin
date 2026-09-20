// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetAnalytics.h"
#include "Containers/Ticker.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Mesh/MeshAnalytics.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

static const FName MeshAnalyticsReportTabName(TEXT("AssetAnalytics.MeshAnalyticsReport"));

#define LOCTEXT_NAMESPACE "FAssetAnalyticsModule"

/** Cleans up services declared in the private implementation. */
FAssetAnalyticsModule::~FAssetAnalyticsModule() = default;

/** Starts the report service and adds Mesh Analytics to Developer Tools. */
void FAssetAnalyticsModule::StartupModule()
{
	ReportBridge = MakeUnique<AssetAnalytics::Core::FAssetAnalyticsReportBridge>();
	ReportBridge->Start();

	const IWorkspaceMenuStructure& MenuStructure = WorkspaceMenu::GetMenuStructure();
	TSharedRef<FWorkspaceItem> ReportMenuGroup = MenuStructure.GetDeveloperToolsDebugCategory();
	if (const TSharedPtr<FWorkspaceItem> DeveloperToolsGroup = ReportMenuGroup->GetParent())
	{
		AssetAnalyticsMenuGroup = DeveloperToolsGroup->AddGroup(
			LOCTEXT("AssetAnalyticsMenuSection", "Asset Analytics"),
			FSlateIcon(),
			true);
		ReportMenuGroup = AssetAnalyticsMenuGroup.ToSharedRef();
	}

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		MeshAnalyticsReportTabName,
		FOnSpawnTab::CreateRaw(this, &FAssetAnalyticsModule::OnSpawnMeshAnalyticsReportTab))
		.SetDisplayName(LOCTEXT("GenerateMeshAnalyticsReport", "Generate Mesh Analytics Report"))
		.SetTooltipText(LOCTEXT("GenerateMeshAnalyticsReportTooltip", "Gather mesh analytics, save the CSV, and open the browser report."))
		.SetGroup(ReportMenuGroup);
}

/** Stops the current report and removes everything added during startup. */
void FAssetAnalyticsModule::ShutdownModule()
{
	AssetAnalytics::Mesh::StopMeshAnalyticsReport();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MeshAnalyticsReportTabName);
	if (AssetAnalyticsMenuGroup.IsValid())
	{
		if (const TSharedPtr<FWorkspaceItem> DeveloperToolsGroup = AssetAnalyticsMenuGroup->GetParent())
		{
			DeveloperToolsGroup->RemoveItem(AssetAnalyticsMenuGroup.ToSharedRef());
		}
		AssetAnalyticsMenuGroup.Reset();
	}

	if (ReportBridge)
	{
		ReportBridge->Stop();
		ReportBridge.Reset();
	}
}

/**
 * Starts the report and closes its temporary Developer Tools tab on the next tick.
 * Returns: The temporary tab required by Unreal's tab spawner.
 */
TSharedRef<SDockTab> FAssetAnalyticsModule::OnSpawnMeshAnalyticsReportTab(const FSpawnTabArgs& SpawnTabArgs)
{
	AssetAnalytics::Mesh::RunMeshAnalyticsReport();

	TSharedRef<SDockTab> CommandTab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
	const TWeakPtr<SDockTab> WeakCommandTab = CommandTab;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakCommandTab](float)
		{
			if (const TSharedPtr<SDockTab> PinnedCommandTab = WeakCommandTab.Pin()) PinnedCommandTab->RequestCloseTab();
			return false;
		}));
	return CommandTab;
}
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAssetAnalyticsModule, AssetAnalytics)
