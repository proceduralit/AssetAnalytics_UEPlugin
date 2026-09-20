// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SDockTab;
class FSpawnTabArgs;
class FWorkspaceItem;

namespace AssetAnalytics
{
namespace Core
{
	class FAssetAnalyticsReportBridge;
}
}

/** Adds Asset Analytics tools to the editor. */
class FAssetAnalyticsModule : public IModuleInterface
{
public:
	virtual ~FAssetAnalyticsModule() override;
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** Runs Mesh Analytics from Developer Tools and returns its temporary tab. */
	TSharedRef<SDockTab> OnSpawnMeshAnalyticsReportTab(const FSpawnTabArgs& SpawnTabArgs);

	/** Handles requests coming from the browser report. */
	TUniquePtr<AssetAnalytics::Core::FAssetAnalyticsReportBridge> ReportBridge;

	/** Asset Analytics group under Developer Tools. */
	TSharedPtr<FWorkspaceItem> AssetAnalyticsMenuGroup;
};
