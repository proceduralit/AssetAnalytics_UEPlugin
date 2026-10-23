// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace AssetAnalytics
{
namespace Core
{
	/** Progress notification with a Cancel button, shared by the report runners. */
	class FReportProgressNotification
	{
	public:
		/** Shows the notification with InitialText; OnCancel runs when Cancel is clicked. */
		void Show(const FText& InitialText, const FText& CancelTooltip, FSimpleDelegate OnCancel);

		/** Replaces the notification text. */
		void SetText(const FText& Text) const;

		/** Shows ResultText with State, then fades the notification out. */
		void Finish(const FText& ResultText, SNotificationItem::ECompletionState State);

		/** Hides the notification immediately, used during shutdown. */
		void Dismiss();

	private:
		/** The visible notification, or null when none is shown. */
		TSharedPtr<SNotificationItem> NotificationItem;
	};
}
}
