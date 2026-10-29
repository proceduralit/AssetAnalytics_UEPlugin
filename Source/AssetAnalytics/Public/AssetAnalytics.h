// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

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
	/** Defined in the .cpp, where the report bridge type is complete. */
	virtual ~FAssetAnalyticsModule() override;

	/** Starts the report bridge and adds the report menu entries. */
	virtual void StartupModule() override;

	/** Stops any running report, removes the menu entries and stops the report bridge. */
	virtual void ShutdownModule() override;

private:
	/** Adds the Mesh and Material Analytics actions to the editor Tools > Audit submenu. */
	void RegisterMenus();

	/** Handles requests coming from the browser report. */
	TUniquePtr<AssetAnalytics::Core::FAssetAnalyticsReportBridge> ReportBridge;
};
