// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "AssetAnalytics.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Framework/Commands/UIAction.h"
#include "Mesh/MeshAnalytics.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FAssetAnalyticsModule"

/** Cleans up services declared in the private implementation. */
FAssetAnalyticsModule::~FAssetAnalyticsModule() = default;

/** Starts the report service and registers the Mesh Analytics menu action. */
void FAssetAnalyticsModule::StartupModule()
{
	ReportBridge = MakeUnique<AssetAnalytics::Core::FAssetAnalyticsReportBridge>();
	ReportBridge->Start();

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAssetAnalyticsModule::RegisterMenus));
}

/** Stops the current report and removes everything added during startup. */
void FAssetAnalyticsModule::ShutdownModule()
{
	AssetAnalytics::Mesh::StopMeshAnalyticsReport();
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (ReportBridge)
	{
		ReportBridge->Stop();
		ReportBridge.Reset();
	}
}

/** Adds a direct Mesh Analytics action to the editor Window menu. */
void FAssetAnalyticsModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.Window");
	FToolMenuSection& Section = WindowMenu->AddSection(
		"AssetAnalytics",
		LOCTEXT("AssetAnalyticsMenuSection", "Asset Analytics"));
	Section.AddEntry(FToolMenuEntry::InitMenuEntry(
		"GenerateMeshAnalyticsReport",
		LOCTEXT("GenerateMeshAnalyticsReport", "Generate Mesh Analytics Report"),
		LOCTEXT("GenerateMeshAnalyticsReportTooltip", "Gather mesh analytics, save the CSV, and open the browser report."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&AssetAnalytics::Mesh::RunMeshAnalyticsReport))));
}
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAssetAnalyticsModule, AssetAnalytics)
