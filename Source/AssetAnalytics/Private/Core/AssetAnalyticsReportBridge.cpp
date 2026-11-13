// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Core/AssetAnalyticsReportBridge.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IContentBrowserSingleton.h"
#include "IHttpRouter.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace AssetAnalytics
{
namespace Core
{
	static constexpr uint32 ReportBridgePort = 19842;
	static const TCHAR* ReportBridgeRoute = TEXT("/asset-analytics/select");
	static const TCHAR* ReportPageRoutePath = TEXT("/asset-analytics/report/page");

	/** Returns the local URL that opens the Mesh Analytics report page. */
	FString GetMeshAnalyticsReportUrl()
	{
		return FString::Printf(TEXT("http://127.0.0.1:%u%s?type=mesh"), ReportBridgePort, ReportPageRoutePath);
	}

	/** Returns the local URL that opens the Material Analytics report page. */
	FString GetMaterialAnalyticsReportUrl()
	{
		return FString::Printf(TEXT("http://127.0.0.1:%u%s?type=material"), ReportBridgePort, ReportPageRoutePath);
	}

	/** Returns the full path to RelativePath inside the plugin folder. */
	static FString GetPluginReportPath(const TCHAR* RelativePath)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssetAnalytics"));
		return Plugin.IsValid()
			? FPaths::Combine(Plugin->GetBaseDir(), RelativePath)
			: FString();
	}

	/** Creates a plain-text response with Message, ResponseCode, and local-browser headers. */
	static TUniquePtr<FHttpServerResponse> MakeReportBridgeResponse(
		const FString& Message,
		const EHttpServerResponseCodes ResponseCode)
	{
		TUniquePtr<FHttpServerResponse> Response =
			FHttpServerResponse::Create(Message, TEXT("text/plain"));
		Response->Code = ResponseCode;
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, OPTIONS") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Private-Network"), { TEXT("true") });
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		return Response;
	}

	/**
	 * Loads FilePath and sends it back with ContentType.
	 * UE 4.27 requests carry no peer address; the 4.27 listener only binds to 127.0.0.1, so every request is local.
	 */
	static bool HandleReportFileRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete,
		const FString& FilePath,
		const FString& ContentType)
	{
		TArray<uint8> FileContents;
		if (!FFileHelper::LoadFileToArray(FileContents, *FilePath))
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("The requested report file was not found."),
				EHttpServerResponseCodes::NotFound));
			return true;
		}

		TUniquePtr<FHttpServerResponse> Response =
			FHttpServerResponse::Create(MoveTemp(FileContents), ContentType);
		Response->Code = EHttpServerResponseCodes::Ok;
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		OnComplete(MoveTemp(Response));
		return true;
	}

	/** Reads an asset link from Request and selects that asset in the Content Browser. Requests are local, as for HandleReportFileRequest. */
	static bool HandleReportBridgeRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_OPTIONS)
		{
			OnComplete(MakeReportBridgeResponse(FString(), EHttpServerResponseCodes::NoContent));
			return true;
		}

		const FString* PackagePath = Request.QueryParams.Find(TEXT("packagePath"));
		const FString* AssetName = Request.QueryParams.Find(TEXT("assetName"));
		if (PackagePath == nullptr || AssetName == nullptr ||
			!PackagePath->StartsWith(TEXT("/")) || AssetName->IsEmpty() ||
			AssetName->Contains(TEXT("/")) || AssetName->Contains(TEXT(".")))
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("The request does not contain a valid asset path."),
				EHttpServerResponseCodes::BadRequest));
			return true;
		}

		FString NormalizedPackagePath = *PackagePath;
		NormalizedPackagePath.RemoveFromEnd(TEXT("/"));
		const FString ObjectPath = FString::Printf(
			TEXT("%s/%s.%s"),
			*NormalizedPackagePath,
			**AssetName,
			**AssetName);

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		const FAssetData AssetData =
			AssetRegistryModule.Get().GetAssetByObjectPath(FName(*ObjectPath));
		if (!AssetData.IsValid())
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("The requested asset was not found in this project."),
				EHttpServerResponseCodes::NotFound));
			return true;
		}

		TArray<FAssetData> AssetsToSync;
		AssetsToSync.Add(AssetData);
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);

		OnComplete(MakeReportBridgeResponse(TEXT("Asset selected."), EHttpServerResponseCodes::Ok));
		return true;
	}

	/** Starts the local report server and registers all report routes. */
	void FAssetAnalyticsReportBridge::Start()
	{
		// UE 4.27 only logs an error when the port is busy and still returns a router.
		ReportHttpRouter = FHttpServerModule::Get().GetHttpRouter(ReportBridgePort);
		if (!ReportHttpRouter.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("Asset Analytics report bridge could not bind to port %u."), ReportBridgePort);
			return;
		}

		SelectAssetRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportBridgeRoute),
			EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_OPTIONS,
			FHttpRequestHandler(&HandleReportBridgeRequest));

		// One page shows every report type; its ?type= picks the type and the JSON route it loads.
		// The page loads the other files by these names relative to its own route.
		struct FReportFileRoute
		{
			const TCHAR* RoutePath;
			FString FilePath;
			const TCHAR* ContentType;
		};
		const TCHAR* HtmlType = TEXT("text/html; charset=utf-8");
		const TCHAR* ScriptType = TEXT("application/javascript; charset=utf-8");
		const TCHAR* JsonType = TEXT("application/json; charset=utf-8");
		const FString SavedReportDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetAnalytics"));
		const FReportFileRoute FileRoutes[] =
		{
			{ ReportPageRoutePath, GetPluginReportPath(TEXT("Report/index.html")), HtmlType },
			{ TEXT("/asset-analytics/report/styles"), GetPluginReportPath(TEXT("Report/core/styles.css")), TEXT("text/css; charset=utf-8") },
			{ TEXT("/asset-analytics/report/plotly"), GetPluginReportPath(TEXT("Report/core/plotly-basic-3.7.0.min.js")), ScriptType },
			{ TEXT("/asset-analytics/report/script"), GetPluginReportPath(TEXT("Report/core/report.js")), ScriptType },
			{ TEXT("/asset-analytics/report/mesh-script"), GetPluginReportPath(TEXT("Report/core/mesh.js")), ScriptType },
			{ TEXT("/asset-analytics/report/material-script"), GetPluginReportPath(TEXT("Report/core/material.js")), ScriptType },
			{ TEXT("/asset-analytics/report/mesh-data"), FPaths::Combine(SavedReportDir, TEXT("MeshAnalytics.json")), JsonType },
			{ TEXT("/asset-analytics/report/material-data"), FPaths::Combine(SavedReportDir, TEXT("MaterialAnalytics.json")), JsonType },
		};
		for (const FReportFileRoute& FileRoute : FileRoutes)
		{
			ReportFileRoutes.Add(ReportHttpRouter->BindRoute(
				FHttpPath(FileRoute.RoutePath),
				EHttpServerRequestVerbs::VERB_GET,
				FHttpRequestHandler(
					[FilePath = FileRoute.FilePath, ContentType = FString(FileRoute.ContentType)](
						const FHttpServerRequest& Request,
						const FHttpResultCallback& OnComplete)
					{
						return HandleReportFileRequest(Request, OnComplete, FilePath, ContentType);
					})));
		}
		const bool bFileRoutesValid = !ReportFileRoutes.ContainsByPredicate(
			[](const TSharedPtr<const FHttpRouteHandleInternal>& Route) { return !Route.IsValid(); });

		if (!SelectAssetRoute.IsValid() || !bFileRoutesValid)
		{
			UE_LOG(LogTemp, Warning, TEXT("One or more Asset Analytics report routes could not be registered."));
			Stop();
			return;
		}

		FHttpServerModule::Get().StartAllListeners();
	}

	/** Removes the report routes without stopping other HTTP listeners. */
	void FAssetAnalyticsReportBridge::Stop()
	{
		if (ReportHttpRouter.IsValid() && SelectAssetRoute.IsValid()) ReportHttpRouter->UnbindRoute(SelectAssetRoute);
		for (const TSharedPtr<const FHttpRouteHandleInternal>& Route : ReportFileRoutes)
		{
			if (ReportHttpRouter.IsValid() && Route.IsValid()) ReportHttpRouter->UnbindRoute(Route);
		}
		ReportFileRoutes.Reset();
		SelectAssetRoute.Reset();
		ReportHttpRouter.Reset();
	}
}
}
