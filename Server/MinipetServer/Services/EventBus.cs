using System;
using System.Collections.Generic;

namespace MinipetServer.Services;

public enum EventType
{
    UserDragStart,
    UserDragEnd,
    ResourceLoaded,
    SettingsChanged,
    ConfigChanged,
    PetChanged,
    OpenSettings,
    AppExit,
    // adapter 发布的 Agent 状态变化（经 PublishOnUIThread marshal 到 UI 线程）
    AgentStateChanged,
    // AgentStateAggregator 计算后的桌宠反映态（已在 UI 线程，直接 Publish）
    PetReflectStateChanged,
    // ── V0.1.3 多桌宠命令（payload: Models.PetCommandEvent；由 PetManager 订阅执行）──
    AddPet,
    RemovePet,
    SetPrimaryPet,
    // 设置在设置中心点「应用到桌宠」：payload Models.PetPaperdollEvent，只有目标宠物窗口执行换装
    PaperdollApplied,
    // 设置在设置中心点「确定应用」（Mob/Npc）：payload Models.PetEntityAppliedEvent，只有目标宠物窗口切实体
    EntityApplied,
    // 换装完成回执：payload string petId（发起方设置窗口据此解除「合成中」按钮态）
    PaperdollAppliedDone
}

public class EventBus
{
    private readonly Dictionary<EventType, List<Action<object?>>> _subscribers = new();
    private readonly object _lock = new();

    public void Subscribe(EventType eventType, Action<object?> callback)
    {
        try
        {
            lock (_lock)
            {
                if (!_subscribers.ContainsKey(eventType))
                    _subscribers[eventType] = new List<Action<object?>>();
                _subscribers[eventType].Add(callback);
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[EventBus] Subscribe: {ex.Message}");
        }
    }

    public void Unsubscribe(EventType eventType, Action<object?> callback)
    {
        try
        {
            lock (_lock)
            {
                if (_subscribers.TryGetValue(eventType, out var list))
                    list.Remove(callback);
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[EventBus] Unsubscribe: {ex.Message}");
        }
    }

    public void Publish(EventType eventType, object? data = null)
    {
        List<Action<object?>> handlers;
        lock (_lock)
        {
            if (!_subscribers.TryGetValue(eventType, out var list)) return;
            handlers = new List<Action<object?>>(list);
        }

        foreach (var handler in handlers)
        {
            try
            {
                handler(data);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[EventBus] Publish {eventType}: {ex.Message}");
            }
        }
    }

    /// <summary>
    /// 服务端迁移（M1）：无 UI 线程，PublishOnUIThread 与 Publish 等价（直接同步发布）。
    /// 保留方法以兼容桌面版调用方签名。
    /// </summary>
    public void PublishOnUIThread(EventType eventType, object? data = null)
    {
        Publish(eventType, data);
    }
}
