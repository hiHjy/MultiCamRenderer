# 本地 AI Agent 运行逻辑

这份文档描述的是 RV1126B 上已经跑通的实验链路，以及准备接入 MultiCamRenderer 的正式边界。

## 一句话

RKLLM 只负责生成文字；Agent 负责把文字限制成允许的工具调用；真正操作摄像头的是 C++ 工具执行器，不是模型。

```text
用户文字
  -> LocalAgent
  -> AgentModel::generate()             第一次：模型选择工具
  -> 严格解析和参数校验
  -> AgentToolExecutor                  C++ 查询/控制真实设备
  -> AgentModel::generate()             第二次：根据真实结果生成一句回答
  -> 用户看到的回答
```

## 当前实验中的进程关系

```text
smolagents (Python，只做通用 Agent 循环)
  |
  | Unix socket: REQUEST <base64 prompt>
  v
rkllm-agent-server (C++)
  |
  | rkllm_init() 一次，常驻一个 LLMHandle
  v
librkllmrt.so -> NPU -> Qwen3-1.7B
```

Python 不加载模型权重，也不做 NPU 推理。模型权重由 C++ 进程通过 `rkllm_init()` 加载一次；每次请求只调用 `rkllm_run()`。Unix socket 用 base64 行协议，是为了不让终端回显、PTY 换行和 `assistant>` 提示符污染模型回复边界。

## 一次“打开夜视”的完整过程

1. 用户说：`请打开摄像头 0 的夜视模式。`
2. `LocalAgent` 给模型一张工具白名单和严格 JSON 格式。
3. Qwen 回复：
   ```json
   {"type":"tool_call","name":"set_night_mode","arguments":{"camera_id":0,"enabled":true}}
   ```
4. `LocalAgent` 只接受白名单工具、非负整数 `camera_id`、布尔值 `enabled` 和固定字段。额外字段、错误类型、未知工具都会拒绝，不会操作硬件。
5. `AgentToolExecutor::setNightMode()` 调用未来的 IPC/ISP 控制接口，返回真实 JSON，例如：
   ```json
   {"camera_id":0,"night_mode":true,"applied":true}
   ```
6. `LocalAgent` 再把“用户原请求 + 真实 JSON”交给模型，只让它生成一句自然语言回答。
7. 模型可以说“摄像头 0 的夜视已开启”，但它的这句话不再决定设备状态；真实状态只认第 5 步的 C++ 返回值。

## 工程中的文件

- `include/ai/AgentModel.hpp`：模型抽象。后续 `RkllmAgentModel` 可以实现它。
- `include/ai/AgentToolExecutor.hpp`：受控工具抽象。后续实现内部持有或访问 `AppRuntime`、`CamManager`、`IspController`。
- `include/ai/LocalAgent.hpp` 和 `src/ai/LocalAgent.cpp`：两次模型调用、严格解析、执行工具、最终总结的核心流程。
- `demo/ai_agent_flow_demo.cpp`：不依赖板子、不加载模型的教学 demo，用假模型和假工具走完相同流程。
- `src/ai/RkllmAgentServer.cpp`：已验证的 RKLLM Unix socket 推理服务。它只处理
  `prompt -> model reply`，不直接获得硬件控制权限；通过
  `-DMCR_BUILD_RKLLM_AGENT_SERVER=ON -DMCR_RKLLM_API_ROOT=<librkllm_api>` 显式构建。

## 为什么不能让模型直接执行命令

模型可能选错工具、填写错误参数、生成多余字段，甚至把不存在的状态说得很像真的。`LocalAgent` 的职责就是把模型限制在“建议”层；`AgentToolExecutor` 才是设备权限边界。
