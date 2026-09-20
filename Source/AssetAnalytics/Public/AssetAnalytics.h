// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

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
	/** Adds the direct Mesh Analytics action to the editor Window menu. */
	void RegisterMenus();

	/** Handles requests coming from the browser report. */
	TUniquePtr<AssetAnalytics::Core::FAssetAnalyticsReportBridge> ReportBridge;
};
