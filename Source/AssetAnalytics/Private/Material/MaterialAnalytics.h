// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

namespace AssetAnalytics
{
namespace Material
{
	/** Gathers master material data and saves it as a CSV report. */
	void RunMaterialAnalyticsReport();

	/** Stops the current Material Analytics report, if there is one. */
	void StopMaterialAnalyticsReport();
}
}
