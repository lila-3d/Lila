#include "LilaComponentExporter.h"
#include "LilaLinkComponents.h"
#include "LilaLinkLog.h"

#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
/**
 * EXAMPLE: the smallest custom component exporter. In Lila, set a component's Unreal export to custom with class
 * LilaExample; each send logs the component's attributes and tags a placed actor "LilaExample:<component>". Copy it into
 * your own editor module, give it a name of yours, and register it there.
 */
class FLilaExampleExporter : public ILilaComponentExporter
{
public:
	virtual FName GetName() const override { return TEXT("LilaExample"); }

	virtual bool Apply(AActor* Actor, UObject* Asset, const FString& ComponentName, const TSharedRef<FJsonObject>& Attrs, TArray<FString>& OutWarnings) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaExampleExporter::Apply);

		const FString Target = Actor ? Actor->GetActorLabel() : Asset ? Asset->GetPathName() : FString(TEXT("nothing"));
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Attr : Attrs->Values)
		{
			FString Text;
			if (!Attr.Value.IsValid() || !Attr.Value->TryGetString(Text))
			{
				Text = Attr.Value.IsValid() && Attr.Value->Type == EJson::Number ? FString::SanitizeFloat(Attr.Value->AsNumber()) : FString(TEXT("(value)"));
			}
			UE_LOG(LogLilaLink, Log, TEXT("LilaExample: %s %s.%s = %s"), *Target, *ComponentName, *Attr.Key, *Text);
		}
		if (Actor)
		{
			Actor->Modify();
			Actor->Tags.AddUnique(FName(*(TEXT("LilaExample:") + ComponentName)));
		}
		return true;
	}
};
} // namespace

void LilaLink::RegisterExampleExporter()
{
	LilaLink::RegisterComponentExporter(MakeShared<FLilaExampleExporter>());
}

void LilaLink::UnregisterExampleExporter()
{
	LilaLink::UnregisterComponentExporter(TEXT("LilaExample"));
}
