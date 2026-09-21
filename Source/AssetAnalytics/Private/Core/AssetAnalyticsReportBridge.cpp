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
#include "IPAddress.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

namespace AssetAnalytics
{
namespace Core
{
	static constexpr uint32 ReportBridgePort = 19842;
	static const TCHAR* ReportBridgeRoute = TEXT("/asset-analytics/select");
	static const TCHAR* ReportPageRoutePath = TEXT("/asset-analytics/report/page");
	static const TCHAR* ReportStylesRoutePath = TEXT("/asset-analytics/report/styles");
	static const TCHAR* ReportScriptRoutePath = TEXT("/asset-analytics/report/script");
	static const TCHAR* ReportPlotlyRoutePath = TEXT("/asset-analytics/report/plotly");
	static const TCHAR* ReportCsvRoutePath = TEXT("/asset-analytics/report/data");

	/** Returns the local URL that opens the report page. */
	FString GetAssetAnalyticsReportUrl()
	{
		return FString::Printf(TEXT("http://127.0.0.1:%u%s"), ReportBridgePort, ReportPageRoutePath);
	}

	/** Returns true for local requests, or all requests when the engine does not expose peer addresses. */
	template <typename RequestType>
	static auto IsLoopbackRequest(const RequestType& Request, int)
		-> decltype(Request.PeerAddress, bool())
	{
		const FString PeerAddress = Request.PeerAddress.IsValid()
			? Request.PeerAddress->ToString(false)
			: FString();
		return PeerAddress == TEXT("::1") || PeerAddress.StartsWith(TEXT("127."));
	}

	/** Lets older engine versions pass the check because they do not provide the peer address. */
	template <typename RequestType>
	static bool IsLoopbackRequest(const RequestType&, long)
	{
		return true;
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
			FHttpServerResponse::Create(Message, TEXT("text/plain; charset=utf-8"));
		Response->Code = ResponseCode;
		Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, OPTIONS") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type") });
		Response->Headers.Add(TEXT("Access-Control-Allow-Private-Network"), { TEXT("true") });
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		return Response;
	}

	/** Loads FilePath and sends it back with ContentType when Request is local. */
	static bool HandleReportFileRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete,
		const FString& FilePath,
		const FString& ContentType)
	{
		if (!IsLoopbackRequest(Request, 0))
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("Only loopback requests are accepted."),
				EHttpServerResponseCodes::Forbidden));
			return true;
		}

		FString FileContents;
		if (!FFileHelper::LoadFileToString(FileContents, *FilePath))
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("The requested report file was not found."),
				EHttpServerResponseCodes::NotFound));
			return true;
		}

		TUniquePtr<FHttpServerResponse> Response =
			FHttpServerResponse::Create(FileContents, ContentType);
		Response->Code = EHttpServerResponseCodes::Ok;
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		OnComplete(MoveTemp(Response));
		return true;
	}

	/** Serves the Mesh Analytics report HTML from the plugin Report folder. */
	static bool HandleReportPageRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		const FString FilePath = GetPluginReportPath(TEXT("Report/index.html"));
		return HandleReportFileRequest(Request, OnComplete, FilePath, TEXT("text/html; charset=utf-8"));
	}

	/** Serves the Mesh Analytics report stylesheet from the plugin Report folder. */
	static bool HandleReportStylesRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		const FString FilePath = GetPluginReportPath(TEXT("Report/core/styles.css"));
		return HandleReportFileRequest(Request, OnComplete, FilePath, TEXT("text/css; charset=utf-8"));
	}

	/** Serves the Mesh Analytics report application script from the plugin Report folder. */
	static bool HandleReportScriptRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		const FString FilePath = GetPluginReportPath(TEXT("Report/core/app.js"));
		return HandleReportFileRequest(Request, OnComplete, FilePath, TEXT("application/javascript; charset=utf-8"));
	}

	/** Serves the local Plotly script used by the Mesh Analytics report. */
	static bool HandleReportPlotlyRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		const FString FilePath = GetPluginReportPath(TEXT("Report/core/plotly-basic-3.7.0.min.js"));
		return HandleReportFileRequest(Request, OnComplete, FilePath, TEXT("application/javascript; charset=utf-8"));
	}

	/** Serves the most recently generated Mesh Analytics CSV from the project Saved folder. */
	static bool HandleReportCsvRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		const FString FilePath = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AssetAnalytics/MeshAnalytics.csv"));
		return HandleReportFileRequest(Request, OnComplete, FilePath, TEXT("text/csv; charset=utf-8"));
	}

	/** Reads an asset link from Request and selects that asset in the Content Browser. */
	static bool HandleReportBridgeRequest(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete)
	{
		if (!IsLoopbackRequest(Request, 0))
		{
			OnComplete(MakeReportBridgeResponse(
				TEXT("Only loopback requests are accepted."),
				EHttpServerResponseCodes::Forbidden));
			return true;
		}

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
			AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
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

	/** Uses the newer HTTP server router call when the engine supports it. */
	template <typename HttpServerModuleType>
	static auto GetReportHttpRouter(HttpServerModuleType& HttpServerModule, int)
		-> decltype(HttpServerModule.GetHttpRouter(ReportBridgePort, true))
	{
		return HttpServerModule.GetHttpRouter(ReportBridgePort, true);
	}

	/** Falls back to the older router call that only takes a port. */
	template <typename HttpServerModuleType>
	static auto GetReportHttpRouter(HttpServerModuleType& HttpServerModule, long)
		-> decltype(HttpServerModule.GetHttpRouter(ReportBridgePort))
	{
		return HttpServerModule.GetHttpRouter(ReportBridgePort);
	}

	/** Wraps Function in a delegate when the engine uses delegate handlers. */
	template <typename HandlerType, typename FunctionType>
	static auto MakeReportRequestHandler(FunctionType Function, int)
		-> decltype(HandlerType::CreateStatic(Function))
	{
		return HandlerType::CreateStatic(Function);
	}

	/** Wraps Function directly when the engine uses callable handlers. */
	template <typename HandlerType, typename FunctionType>
	static HandlerType MakeReportRequestHandler(FunctionType Function, long)
	{
		return HandlerType(Function);
	}

	/** Starts the local report server and registers all report routes. */
	void FAssetAnalyticsReportBridge::Start()
	{
		FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
		ReportHttpRouter = GetReportHttpRouter(HttpServerModule, 0);
		if (!ReportHttpRouter.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("Asset Analytics report bridge could not bind to port %u."), ReportBridgePort);
			return;
		}

		SelectAssetRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportBridgeRoute),
			EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_OPTIONS,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportBridgeRequest, 0));
		ReportPageRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportPageRoutePath),
			EHttpServerRequestVerbs::VERB_GET,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportPageRequest, 0));
		ReportStylesRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportStylesRoutePath),
			EHttpServerRequestVerbs::VERB_GET,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportStylesRequest, 0));
		ReportScriptRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportScriptRoutePath),
			EHttpServerRequestVerbs::VERB_GET,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportScriptRequest, 0));
		ReportPlotlyRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportPlotlyRoutePath),
			EHttpServerRequestVerbs::VERB_GET,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportPlotlyRequest, 0));
		ReportCsvRoute = ReportHttpRouter->BindRoute(
			FHttpPath(ReportCsvRoutePath),
			EHttpServerRequestVerbs::VERB_GET,
			MakeReportRequestHandler<FHttpRequestHandler>(&HandleReportCsvRequest, 0));
		if (!SelectAssetRoute.IsValid() || !ReportPageRoute.IsValid() ||
			!ReportStylesRoute.IsValid() || !ReportScriptRoute.IsValid() ||
			!ReportPlotlyRoute.IsValid() || !ReportCsvRoute.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("One or more Asset Analytics report routes could not be registered."));
			Stop();
			return;
		}

		HttpServerModule.StartAllListeners();
	}

	/** Removes the report routes without stopping other HTTP listeners. */
	void FAssetAnalyticsReportBridge::Stop()
	{
		if (ReportHttpRouter.IsValid() && SelectAssetRoute.IsValid()) ReportHttpRouter->UnbindRoute(SelectAssetRoute);
		if (ReportHttpRouter.IsValid() && ReportPageRoute.IsValid()) ReportHttpRouter->UnbindRoute(ReportPageRoute);
		if (ReportHttpRouter.IsValid() && ReportStylesRoute.IsValid()) ReportHttpRouter->UnbindRoute(ReportStylesRoute);
		if (ReportHttpRouter.IsValid() && ReportScriptRoute.IsValid()) ReportHttpRouter->UnbindRoute(ReportScriptRoute);
		if (ReportHttpRouter.IsValid() && ReportPlotlyRoute.IsValid()) ReportHttpRouter->UnbindRoute(ReportPlotlyRoute);
		if (ReportHttpRouter.IsValid() && ReportCsvRoute.IsValid()) ReportHttpRouter->UnbindRoute(ReportCsvRoute);
		SelectAssetRoute.Reset();
		ReportPageRoute.Reset();
		ReportStylesRoute.Reset();
		ReportScriptRoute.Reset();
		ReportPlotlyRoute.Reset();
		ReportCsvRoute.Reset();
		ReportHttpRouter.Reset();
	}
}
}
