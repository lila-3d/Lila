#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"
#include "LilaLinkServer.h"
#include "LilaLinkSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogLilaLink);

namespace
{
constexpr int32 PortSearchCount = 8;

FString PortFilePath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("LilaLink") / TEXT("port.json"));
}
} // namespace

class FLilaLinkModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkModule::StartupModule);

		// Cooks and other commandlets never tick the core ticker, so requests would wait forever.
		if (IsRunningCommandlet())
		{
			return;
		}
		ULilaLinkSettings* Settings = GetMutableDefault<ULilaLinkSettings>();
		SettingsHandle = Settings->OnSettingChanged().AddRaw(this, &FLilaLinkModule::OnSettingsChanged);
		Restart();
	}

	virtual void ShutdownModule() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkModule::ShutdownModule);

		if (UObjectInitialized() && SettingsHandle.IsValid())
		{
			GetMutableDefault<ULilaLinkSettings>()->OnSettingChanged().Remove(SettingsHandle);
		}
		StopServer();
	}

private:
	void OnSettingsChanged(UObject*, FPropertyChangedEvent&)
	{
		const ULilaLinkSettings* Settings = GetDefault<ULilaLinkSettings>();
		const bool bRunning = Server.IsValid();
		if (bRunning != Settings->bEnabled || (bRunning && RequestedPort != Settings->Port))
		{
			Restart();
		}
	}

	void Restart()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkModule::Restart);

		StopServer();
		const ULilaLinkSettings* Settings = GetDefault<ULilaLinkSettings>();
		if (!Settings->bEnabled)
		{
			UE_LOG(LogLilaLink, Log, TEXT("Disabled in Editor Preferences"));
			return;
		}
		RequestedPort = Settings->Port;
		Server = MakeUnique<FLilaLinkServer>(LilaLink::MakeMethods());
		if (!Server->Start(RequestedPort, PortSearchCount))
		{
			Server.Reset();
			return;
		}
		WritePortFile(Server->GetPort());
	}

	void StopServer()
	{
		if (Server)
		{
			Server->Shutdown();
			Server.Reset();
			IFileManager::Get().Delete(*PortFilePath(), false, true, true);
		}
	}

	static void WritePortFile(int32 Port)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkModule::WritePortFile);

		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("port"), Port);
		Json->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
		Json->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
		Json->SetStringField(TEXT("project"), FApp::GetProjectName());
		Json->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));

		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Json, Writer);
		if (!FFileHelper::SaveStringToFile(Text, *PortFilePath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogLilaLink, Warning, TEXT("Could not write %s"), *PortFilePath());
		}
	}

	TUniquePtr<FLilaLinkServer> Server;
	FDelegateHandle SettingsHandle;
	int32 RequestedPort = 0;
};

IMPLEMENT_MODULE(FLilaLinkModule, LilaLink)
