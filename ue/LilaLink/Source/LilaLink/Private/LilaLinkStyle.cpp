#include "LilaLinkStyle.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

TSharedPtr<FSlateStyleSet> FLilaLinkStyle::StyleSet;

FName FLilaLinkStyle::GetStyleSetName()
{
	static const FName StyleName(TEXT("LilaLinkStyle"));
	return StyleName;
}

void FLilaLinkStyle::Register()
{
	if (StyleSet.IsValid())
	{
		return;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LilaLink"));
	if (!Plugin.IsValid())
	{
		return;
	}

	StyleSet = MakeShared<FSlateStyleSet>(GetStyleSetName());
	StyleSet->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	StyleSet->Set(GetMenuIconName(), new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("Icon64"), TEXT(".png")), FVector2D(16.f, 16.f)));
	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

void FLilaLinkStyle::Unregister()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
}
