#pragma once

#include "CoreMinimal.h"

struct FUiFocusEvent;
struct FUiFocusEventContext;
class IFileHandle;

/** 会话启动信息（模块侧汇总后传入；记录器只写文件，不触碰引擎状态）。 */
struct FUiFocusSessionStartInfo
{
	FString Reason;              // overlay | cvar
	bool bOverlayOn = false;
	bool bStackCapture = false;  // 会话生效的 stackcapture 终值
	bool bAutoStack = false;     // 是否由会话自动开启（结束后还原）
	bool bActivationTracked = false; // CommonUI 桥存在（激活事件可入日志）
};

/**
 * 会话记录器（飞行记录仪）：开启期间所有事件逐条追加 JSONL，每行写后 Flush——
 * 编辑器崩溃后文件仍完整可用（HTTP 面板方案随进程消失，此处刻意文件化）。
 * 消费者=agent（Docs/FocusPlaybook.md）；记录端保持最小，摘要/统计由分析侧脚本现算。
 * 仅 game thread 使用。
 */
class FUiFocusSessionRecorder
{
public:
	static constexpr int32 SchemaVersion = 1;

	bool Start(const FUiFocusSessionStartInfo& InInfo);
	void Stop();
	bool IsActive() const { return FileHandle != nullptr; }

	/** 在环形缓冲过滤（自环/链式折叠）之前调用——日志为原始流，保真链式跳变。 */
	void WriteEvent(const FUiFocusEvent& InEvent, const FUiFocusEventContext& InContext);

	/** 会话内插入标记行（uifocus.mark）：分析时对准复现时刻。仅在会话激活时有效。 */
	void WriteMark(const FString& InText);

	const FString& GetFilePath() const { return FilePath; }

private:
	void WriteLine(const FString& InLine);

	IFileHandle* FileHandle = nullptr;
	FString FilePath;
	int64 NextSeq = 1;
	int32 EventCount = 0;
};
