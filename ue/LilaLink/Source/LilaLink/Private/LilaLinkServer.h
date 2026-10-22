#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

#include <atomic>

class FRunnableThread;
class FSocket;

/** One connection. Lines are queued from any thread and written by the server thread. */
struct FLilaLinkClient
{
	FSocket* Socket = nullptr;
	TArray<uint8> Inbox;

	FCriticalSection OutboxLock;
	TArray<uint8> Outbox;

	void QueueLine(const FString& Line);
};

/**
 * One request on its way through the game thread. Reply once; Progress any number of times before that, which tells
 * Lila the editor is alive and resets its idle timeout. Dropping the last reference unanswered replies with an error.
 */
class FLilaLinkCall
{
public:
	FLilaLinkCall(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& InClient, TSharedPtr<FJsonValue> InId, FString InMethod);
	~FLilaLinkCall();

	/** Pass a null Result with an Error to fail. */
	void Reply(const TSharedPtr<FJsonValue>& Result, const FString& Error = FString());
	/** Sends {"method":"progress","params":{"id","stage","elapsed"}}. */
	void Progress(const FString& Stage);

	bool HasReplied() const { return bReplied; }
	double GetElapsedSeconds() const;
	const FString& GetMethod() const { return Method; }
	/** "importAsset #3", for logs. */
	FString Describe() const;

private:
	TWeakPtr<FLilaLinkClient, ESPMode::ThreadSafe> Client;
	TSharedPtr<FJsonValue> Id;
	FString Method;
	double StartSeconds = 0.0;
	std::atomic<bool> bReplied = false;
};

using FLilaLinkCallRef = TSharedRef<FLilaLinkCall, ESPMode::ThreadSafe>;

/** A JSON-RPC method. Runs on the game thread from the core ticker; replies through Call now or later. */
using FLilaLinkMethod = TFunction<void(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)>;

/** A method that answers before returning: sets OutError and returns null to fail. */
using FLilaLinkSyncMethod = TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonObject>& Params, FString& OutError)>;

/**
 * Localhost TCP server speaking newline-delimited JSON-RPC 2.0. Sockets are serviced on a background thread that never
 * waits on the game thread. Requests are handed to the game thread through a queue drained by a core ticker, at the
 * top of the frame and outside any task graph task, so methods may run work that pumps game-thread tasks itself.
 */
class FLilaLinkServer : public FRunnable
{
public:
	explicit FLilaLinkServer(TMap<FString, FLilaLinkMethod> InMethods);
	virtual ~FLilaLinkServer() override;

	/** Binds 127.0.0.1 on the first free port in [FirstPort, FirstPort + PortsToTry). Game thread. */
	bool Start(int32 FirstPort, int32 PortsToTry);
	/** Game thread. */
	void Shutdown();

	int32 GetPort() const { return Port; }

	virtual uint32 Run() override;
	virtual void Stop() override { bStopping = true; }

private:
	struct FQueuedRequest
	{
		TSharedPtr<FLilaLinkCall, ESPMode::ThreadSafe> Call;
		TSharedPtr<FJsonObject> Params;
		double QueuedSeconds = 0.0;
	};

	/** False when the connection is gone. */
	bool ServiceClient(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& Client, bool& bOutActive);
	void HandleLine(const TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>& Client, const FString& Line);
	/** Server thread: warns when requests sit in the queue because the game thread stopped ticking. */
	void WatchGameThread();

	bool TickGameThread(float DeltaTime);
	void ReportLongCalls(double Now);

	TMap<FString, FLilaLinkMethod> Methods;
	FSocket* Listener = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bStopping = false;
	int32 Port = 0;
	TArray<TSharedPtr<FLilaLinkClient, ESPMode::ThreadSafe>> Clients;

	TQueue<FQueuedRequest, EQueueMode::Spsc> Requests;
	std::atomic<int32> QueuedCount = 0;
	std::atomic<double> LastGameTickSeconds = 0.0;
	double NextStallWarningSeconds = 0.0;

	FTSTicker::FDelegateHandle TickHandle;
	/** Calls whose method returned without replying. Game thread. */
	TArray<TWeakPtr<FLilaLinkCall, ESPMode::ThreadSafe>> InFlight;
	double NextLongCallReportSeconds = 0.0;
};

namespace LilaLink
{
/** Builds a JSON-RPC response line (no trailing newline). Pass a null Result together with an error message. */
FString MakeResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result, int32 ErrorCode, const FString& ErrorMessage);

/** Adapts a method that answers before returning. */
FLilaLinkMethod Sync(FLilaLinkSyncMethod Fn);
} // namespace LilaLink
