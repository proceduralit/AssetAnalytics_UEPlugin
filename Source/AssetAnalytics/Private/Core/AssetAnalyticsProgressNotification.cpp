// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Core/AssetAnalyticsProgressNotification.h"
#include "Framework/Notifications/NotificationManager.h"

#define LOCTEXT_NAMESPACE "AssetAnalyticsProgressNotification"

namespace AssetAnalytics
{
namespace Core
{
	/**
	 * Shows a pending notification that stays open until Finish or Dismiss is called.
	 * Input: InitialText is shown first; CancelTooltip and OnCancel set up the Cancel button.
	 */
	void FReportProgressNotification::Show(const FText& InitialText, const FText& CancelTooltip, FSimpleDelegate OnCancel)
	{
		FNotificationInfo NotificationInfo(InitialText);
		NotificationInfo.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("CancelReport", "Cancel"),
			CancelTooltip,
			MoveTemp(OnCancel)));
		NotificationInfo.WidthOverride = 320.0f;
		NotificationInfo.bFireAndForget = false;
		NotificationInfo.FadeInDuration = 0.1f;
		NotificationInfo.FadeOutDuration = 0.5f;
		NotificationInfo.ExpireDuration = 3.0f;
		NotificationInfo.bUseLargeFont = false;
		NotificationInfo.bUseThrobber = false;
		NotificationInfo.bUseSuccessFailIcons = false;
		NotificationItem = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
		if (NotificationItem.IsValid()) NotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
	}

	/** Replaces the text of the visible notification, if there is one. */
	void FReportProgressNotification::SetText(const FText& Text) const
	{
		if (NotificationItem.IsValid()) NotificationItem->SetText(Text);
	}

	/**
	 * Shows the final result and lets the notification fade out on its own.
	 * Input: ResultText and State describe how the report ended.
	 */
	void FReportProgressNotification::Finish(const FText& ResultText, const SNotificationItem::ECompletionState State)
	{
		if (NotificationItem.IsValid())
		{
			NotificationItem->SetText(ResultText);
			NotificationItem->SetCompletionState(State);
			NotificationItem->ExpireAndFadeout();
		}
		NotificationItem.Reset();
	}

	/** Fades the notification out without a result, used when the editor shuts down. */
	void FReportProgressNotification::Dismiss()
	{
		if (NotificationItem.IsValid())
		{
			NotificationItem->SetCompletionState(SNotificationItem::CS_None);
			NotificationItem->Fadeout();
		}
		NotificationItem.Reset();
	}
}
}

#undef LOCTEXT_NAMESPACE
