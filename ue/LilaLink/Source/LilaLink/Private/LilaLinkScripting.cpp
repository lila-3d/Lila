#include "LilaLinkScripting.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethodRegistry.h"
#include "LilaLinkMethods.h"
#include "LilaLinkServer.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString ULilaLinkScripting::CallMethod(const FString& Method, const FString& ParamsJson, float TimeoutSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkScripting::CallMethod);

	auto Fail = [](const FString& Message)
	{
		return FString::Printf(TEXT("{\"error\":{\"message\":\"%s\"}}"), *Message.ReplaceCharWithEscapedChar());
	};
	TMap<FString, FLilaLinkMethod> Methods = LilaLink::MakeMethods();
	FLilaLinkMethod External;
	FLilaLinkMethod* Fn = Methods.Find(Method);
	if (!Fn && LilaLink::FindExternalMethod(Method, External))
	{
		Fn = &External;
	}
	if (!Fn)
	{
		return Fail(FString::Printf(TEXT("Unknown method %s"), *Method));
	}
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (!ParamsJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
		if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid())
		{
			return Fail(TEXT("params is not a JSON object"));
		}
	}

	// A client without a socket: the reply lands in its outbox.
	TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe> Client = MakeShared<FLilaLinkClient, ESPMode::ThreadSafe>();
	{
		FLilaLinkCallRef Call = MakeShared<FLilaLinkCall, ESPMode::ThreadSafe>(Client, MakeShared<FJsonValueNumber>(1), Method);
		(*Fn)(Params, Call);
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		double Last = FPlatformTime::Seconds();
		while (!Call->HasReplied() && FPlatformTime::Seconds() < Deadline)
		{
			const double Now = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - Last));
			Last = Now;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.01f);
		}
		if (!Call->HasReplied())
		{
			Call->Reply(nullptr, FString::Printf(TEXT("%s did not answer within %.0f s"), *Method, TimeoutSeconds));
		}
	}

	FString Response;
	{
		FScopeLock Lock(&Client->OutboxLock);
		Client->Outbox.Add(0);
		TArray<FString> Lines;
		FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Client->Outbox.GetData()))).ParseIntoArrayLines(Lines);
		// Progress notifications come first; the response is the line with an id.
		for (const FString& Line : Lines)
		{
			TSharedPtr<FJsonObject> Message;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
			if (FJsonSerializer::Deserialize(Reader, Message) && Message.IsValid() && Message->HasField(TEXT("id")))
			{
				Response = Line;
			}
		}
	}
	return Response.IsEmpty() ? Fail(TEXT("no response")) : Response;
}
