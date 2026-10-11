#include "UiFocusSessionRecorder.h"

#include "UiFocusDebuggerModule.h"

#include "Containers/StringConv.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "GenericPlatform/GenericPlatformTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogUiFocusRecorder, Log, All);

namespace
{
	/** JSON 字符串转义：只处理 ASCII 特殊字符，非 ASCII 原样通过（落盘时统一转 UTF-8）。 */
	FString EscapeJson(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 8);
		for (const TCHAR Ch : In)
		{
			switch (Ch)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (Ch < 0x20)
				{
					Out.AppendChar(TEXT('\\'));
					Out += FString::Printf(TEXT("u%04x"), (int32)Ch);
				}
				else
				{
					Out += Ch;
				}
			}
		}
		return Out;
	}

	FString RectToJson(const FSlateRect& R)
	{
		return FString::Printf(TEXT("[%.1f,%.1f,%.1f,%.1f]"), R.Left, R.Top, R.Right, R.Bottom);
	}

	FString WidgetContextToJson(const FUiFocusWidgetContext& C)
	{
		return FString::Printf(TEXT("{\"focusable\":%d,\"vis\":\"%s\",\"enabled\":%d,\"rect\":%s}"),
			C.bFocusable ? 1 : 0, *EscapeJson(C.Vis), C.bEnabled ? 1 : 0, *RectToJson(C.Rect));
	}

	/**
	 * 工程侧帧启发式分类：先排除工具自身的帧（捕获/入队/写盘链是固定管道噪声，含
	 * "UiFocusDebugger" 模块/路径标记）；再按工程目录/工程名匹配工程侧帧。
	 * 工程目录运行时推导（PDB 符号串里是构建机的路径，正/反斜杠两种形式都试），
	 * 引擎帧是 Build 目录路径或无路径，天然不命中。尽力而为，原文始终在 stack 里。
	 */
	bool IsProjectFrame(const FString& InFrame)
	{
		if (InFrame.Contains(TEXT("UiFocusDebugger")))
		{
			return false;
		}
		static const FString ProjectDirFwd = FPaths::GetPath(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		static const FString ProjectDirBwd = ProjectDirFwd.Replace(TEXT("/"), TEXT("\\"));
		static const FString ProjectToken = FApp::GetProjectName();
		return InFrame.Contains(ProjectDirFwd)
			|| InFrame.Contains(ProjectDirBwd)
			|| InFrame.Contains(ProjectToken);
	}
}

bool FUiFocusSessionRecorder::Start(const FUiFocusSessionStartInfo& InInfo)
{
	if (IsActive())
	{
		return true;
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("UiFocusDebugger") / TEXT("sessions");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/true);

	// 同秒重开会话：文件名加序号防覆盖
	const FString Base = FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H-%M-%S"));
	FString Candidate = Dir / (Base + TEXT(".jsonl"));
	for (int32 Suffix = 2; IFileManager::Get().FileExists(*Candidate); ++Suffix)
	{
		Candidate = Dir / FString::Printf(TEXT("%s_%d.jsonl"), *Base, Suffix);
	}

	FileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Candidate, /*bAppend*/false, /*bAllowRead*/false);
	if (!FileHandle)
	{
		UE_LOG(LogUiFocusRecorder, Error, TEXT("会话文件创建失败：%s"), *Candidate);
		return false;
	}
	FilePath = Candidate;
	NextSeq = 1;
	EventCount = 0;

	// latest 指针：分析侧不必猜哪个文件最新
	FFileHelper::SaveStringToFile(
		FPaths::ConvertRelativePathToFull(Candidate) + TEXT("\r\n"),
		*(Dir / TEXT("latest.txt")));

	// 会话元数据（自描述：schema+字段说明随文件走，agent 拿到文件即会读，不依赖外部文档）
	FString ViewportJson = TEXT("null");
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize(0, 0);
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		ViewportJson = FString::Printf(TEXT("[%.0f,%.0f]"), ViewportSize.X, ViewportSize.Y);
	}
	const FString MapName = GWorld ? GWorld->GetMapName() : FString(TEXT("(unknown)"));
	const float DpiScale = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetApplicationScale() : 1.0f;

	const TCHAR* FieldsDoc = TEXT("{")
		TEXT("\"seq\":\"会话内事件序号(递增)\",")
		TEXT("\"t\":\"事件时间,毫秒(FPlatformTime开机起算,只做差值比较)\",")
		TEXT("\"kind\":\"FocusChanging=焦点变化 | Navigation=导航决策未产生焦点变化 | ActivationChanged=CommonUI页面激活变化\",")
		TEXT("\"cause\":\"Mouse|Navigation|SetDirectly|Cleared|OtherWidgetLostFocus|WindowActivate;激活事件=Activated|Deactivated\",")
		TEXT("\"old/new\":\"name=Slate显示名(UMG实例名优先);owner=所属控件名(列表行/Activatable页面边界判定);path=新焦点根到自身链(仅new,点分隔)\",")
		TEXT("\"nav\":\"dir=方向;genesis=Keyboard|Controller|User;method=选取方式(InferredExplicit=引擎无广播,几何推断);rule=生效规则;handler=生效规则所在控件;dest/destOwner=目的地;chain=链上配置 名字|规则|是否生效 (内→外)\",")
		TEXT("\"ctx\":\"事件时刻控件状态:focusable/vis/enabled/rect(绝对坐标[l,t,r,b]);old=旧焦点,new=新焦点,dest=导航目的地\",")
		TEXT("\"stack\":\"调用堆栈(符号化,stackcapture开启时才有);pstack=其中疑似工程侧帧\",")
		TEXT("\"note\":\"本流为环形缓冲过滤(自环/链式折叠)前的原始流:一次按键可见页面→容器→目标的链式跳变,与叠加层显示的折叠视图不同\"")
		TEXT("}");

	const FString Header = FString::Printf(TEXT(
		"{\"type\":\"header\",\"schema\":%d,\"tool\":\"UiFocusDebugger\","
		"\"startWall\":\"%s\",\"startWallUtc\":\"%s\","
		"\"project\":\"%s\",\"engine\":\"%s\",\"config\":\"%s\",\"map\":\"%s\","
		"\"viewport\":%s,\"dpiScale\":%.3f,"
		"\"reason\":\"%s\",\"overlayOn\":%s,\"stackCapture\":%s,\"autoStack\":%s,\"activationTracked\":%s,"
		"\"eventLayoutVersion\":%u,"
		"\"playbook\":\"UiFocusDebugger/Docs/FocusPlaybook.md\","
		"\"fields\":%s}"),
		SchemaVersion,
		*EscapeJson(FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"))),
		*EscapeJson(FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%SZ"))),
		*EscapeJson(FApp::GetProjectName()),
		*EscapeJson(FEngineVersion::Current().ToString()),
		*EscapeJson(LexToString(FApp::GetBuildConfiguration())),
		*EscapeJson(MapName),
		*ViewportJson,
		DpiScale,
		*EscapeJson(InInfo.Reason),
		InInfo.bOverlayOn ? TEXT("true") : TEXT("false"),
		InInfo.bStackCapture ? TEXT("true") : TEXT("false"),
		InInfo.bAutoStack ? TEXT("true") : TEXT("false"),
		InInfo.bActivationTracked ? TEXT("true") : TEXT("false"),
		FUiFocusDebuggerModule::EventLayoutVersion,
		FieldsDoc);

	WriteLine(Header);
	UE_LOG(LogUiFocusRecorder, Log, TEXT("会话开始(自动堆栈=%d,原因=%s)：%s"),
		InInfo.bAutoStack ? 1 : 0, *InInfo.Reason, *FilePath);
	return true;
}

void FUiFocusSessionRecorder::Stop()
{
	if (!FileHandle)
	{
		return;
	}
	WriteLine(FString::Printf(TEXT("{\"type\":\"footer\",\"endWall\":\"%s\",\"events\":%d}"),
		*EscapeJson(FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"))), EventCount));
	delete FileHandle; // IFileHandle 析构即关闭
	FileHandle = nullptr;
	UE_LOG(LogUiFocusRecorder, Log, TEXT("会话结束：%s（%d 事件）"), *FilePath, EventCount);
}

void FUiFocusSessionRecorder::WriteMark(const FString& InText)
{
	if (!FileHandle)
	{
		return;
	}
	WriteLine(FString::Printf(TEXT("{\"type\":\"mark\",\"seq\":%lld,\"t\":%.3f,\"text\":\"%s\"}"),
		static_cast<long long>(NextSeq++), FPlatformTime::Seconds() * 1000.0, *EscapeJson(InText)));
	// mark 不计入事件数（footer 的 events 只统计 event 行）
}

void FUiFocusSessionRecorder::WriteLine(const FString& InLine)
{
	if (!FileHandle)
	{
		return;
	}
	FString Line(InLine);
	Line += TEXT('\n');
	// UTF-8 无 BOM（JSONL 惯例）；每行 Flush——崩溃后文件仍完整
	const FTCHARToUTF8 Utf8(*Line);
	FileHandle->Write(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	FileHandle->Flush();
}

void FUiFocusSessionRecorder::WriteEvent(const FUiFocusEvent& InEvent, const FUiFocusEventContext& InContext)
{
	if (!FileHandle)
	{
		return;
	}

	TArray<FString> Parts;
	Parts.Reserve(12);
	Parts.Add(TEXT("\"type\":\"event\""));
	Parts.Add(FString::Printf(TEXT("\"seq\":%lld"), static_cast<long long>(NextSeq++)));
	Parts.Add(FString::Printf(TEXT("\"t\":%.3f"), InEvent.TimeMs));
	if (!InEvent.Type.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("\"kind\":\"%s\""), *EscapeJson(InEvent.Type)));
	}
	if (InEvent.UserIndex != 0)
	{
		Parts.Add(FString::Printf(TEXT("\"user\":%d"), InEvent.UserIndex));
	}
	if (!InEvent.Cause.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("\"cause\":\"%s\""), *EscapeJson(InEvent.Cause)));
	}

	const auto SideJson = [](const FString& Name, const FString& Owner, const FString& Path) -> FString
	{
		TArray<FString> Fields;
		if (!Name.IsEmpty())
		{
			Fields.Add(FString::Printf(TEXT("\"name\":\"%s\""), *EscapeJson(Name)));
		}
		if (!Owner.IsEmpty())
		{
			Fields.Add(FString::Printf(TEXT("\"owner\":\"%s\""), *EscapeJson(Owner)));
		}
		if (!Path.IsEmpty())
		{
			Fields.Add(FString::Printf(TEXT("\"path\":\"%s\""), *EscapeJson(Path)));
		}
		return Fields.Num() > 0 ? TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}") : FString();
	};
	const FString OldJson = SideJson(InEvent.OldName, InEvent.OldOwnerName, FString());
	if (!OldJson.IsEmpty())
	{
		Parts.Add(TEXT("\"old\":") + OldJson);
	}
	const FString NewJson = SideJson(InEvent.NewName, InEvent.NewOwnerName, InEvent.NewPath);
	if (!NewJson.IsEmpty())
	{
		Parts.Add(TEXT("\"new\":") + NewJson);
	}

	if (!InEvent.NavDirection.IsEmpty())
	{
		TArray<FString> Nav;
		Nav.Add(FString::Printf(TEXT("\"dir\":\"%s\""), *EscapeJson(InEvent.NavDirection)));
		if (!InEvent.NavGenesis.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"genesis\":\"%s\""), *EscapeJson(InEvent.NavGenesis)));
		}
		if (!InEvent.NavMethod.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"method\":\"%s\""), *EscapeJson(InEvent.NavMethod)));
		}
		if (!InEvent.NavRule.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"rule\":\"%s\""), *EscapeJson(InEvent.NavRule)));
		}
		if (!InEvent.NavHandlerName.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"handler\":\"%s\""), *EscapeJson(InEvent.NavHandlerName)));
		}
		if (!InEvent.NavDestName.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"dest\":\"%s\""), *EscapeJson(InEvent.NavDestName)));
		}
		if (!InEvent.NavDestOwnerName.IsEmpty())
		{
			Nav.Add(FString::Printf(TEXT("\"destOwner\":\"%s\""), *EscapeJson(InEvent.NavDestOwnerName)));
		}
		if (InEvent.NavRuleChain.Num() > 0)
		{
			TArray<FString> Chain;
			Chain.Reserve(InEvent.NavRuleChain.Num());
			for (const FString& Row : InEvent.NavRuleChain)
			{
				Chain.Add(TEXT("\"") + EscapeJson(Row) + TEXT("\""));
			}
			Nav.Add(TEXT("\"chain\":[") + FString::Join(Chain, TEXT(",")) + TEXT("]"));
		}
		Parts.Add(TEXT("\"nav\":{") + FString::Join(Nav, TEXT(",")) + TEXT("}"));
	}

	{
		TArray<FString> Ctx;
		if (InContext.Old.bValid)
		{
			Ctx.Add(TEXT("\"old\":") + WidgetContextToJson(InContext.Old));
		}
		if (InContext.New.bValid)
		{
			Ctx.Add(TEXT("\"new\":") + WidgetContextToJson(InContext.New));
		}
		if (InContext.Dest.bValid)
		{
			Ctx.Add(TEXT("\"dest\":") + WidgetContextToJson(InContext.Dest));
		}
		if (Ctx.Num() > 0)
		{
			Parts.Add(TEXT("\"ctx\":{") + FString::Join(Ctx, TEXT(",")) + TEXT("}"));
		}
	}

	if (InEvent.Stack.Num() > 0)
	{
		TArray<FString> Frames;
		TArray<FString> ProjectFrames;
		Frames.Reserve(InEvent.Stack.Num());
		for (const FString& Frame : InEvent.Stack)
		{
			Frames.Add(TEXT("\"") + EscapeJson(Frame) + TEXT("\""));
			if (IsProjectFrame(Frame))
			{
				ProjectFrames.Add(TEXT("\"") + EscapeJson(Frame) + TEXT("\""));
			}
		}
		Parts.Add(TEXT("\"stack\":[") + FString::Join(Frames, TEXT(",")) + TEXT("]"));
		if (ProjectFrames.Num() > 0)
		{
			Parts.Add(TEXT("\"pstack\":[") + FString::Join(ProjectFrames, TEXT(",")) + TEXT("]"));
		}
	}

	WriteLine(TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}"));
	++EventCount;
}
