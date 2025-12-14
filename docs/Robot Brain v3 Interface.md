# Robot Brain v3 接口使用指南（供硬件端对接）

## 快速要点
- 支持文本/事件 HTTP 请求，以及流式语音 WebSocket。
- 默认音频格式 `mp3_16k_32kbps`，单个音频分片默认 500B（Base64 前原始长度）。
- 建议上行音频使用 16kHz / 16bit / mono PCM 或 WAV；若用 MP3 需服务器安装 `ffmpeg`。
- 关键环境变量：`AZURE_OPENAI_ENDPOINT`、`AZURE_OPENAI_API_KEY`、`AZURE_CHAT_DEPLOYMENT`、`AZURE_EMBEDDING_DEPLOYMENT`、`SPEECH_KEY`、`SPEECH_REGION`、`SPEECH_VOICE`、`SPEECH_LANGUAGE`（参考 `local.env`）。

## 接口概览
- 健康检查：`GET /health`
- 文本对话：`POST /v1/robot`
- 事件触发：`POST /v1/robot/event`
- 语音转文本再回复（单次）：`POST /v1/robot/voice`
- 流式语音对话（推荐给硬件端）：`WS /v3/robot/voice`

## 通用消息格式
- `meta`（服务端下行）
  - `type`: `"meta"`
  - `req`: 请求 ID
  - `rid`: 回复 ID
  - `anim`: 表情动画，基于情绪映射（开心/难过/焦虑/愤怒/平静/疲惫/空虚/孤独 → smile_soft 等）
  - `motion`: 动作指令
  - `af`: 音频格式（如 `mp3_16k_32kbps`）
- `audio`（服务端下行）
  - `type`: `"audio"`
  - `req`, `rid`: 与 meta 对应
  - `seq`: 分片序号（从 1 递增）
  - `is_last`: 是否最后一个分片
  - `chunk`: 音频分片 Base64 字符串
- `asr_text`（仅 WS 流式下行）：`{"type":"asr_text","text":"..."}`

## HTTP 接口
### 1) 文本对话 `POST /v1/robot`
请求 JSON：
```json
{
  "type": "in",
  "u": "你好",
  "req": "r123",          // 可选，未填由服务端生成
  "user_id": "demo",      // 可选，区分记忆
  "chunk_bytes": 500,     // 可选，默认 500
  "mode": "stream",       // "stream" 分片返回；"single" 整包
  "af": "mp3_16k_32kbps"  // 可选，默认值
}
```
响应 JSON：
```json
{
  "req": "r123",
  "rid": "rep_xxx",
  "text": "机器人回复文本",
  "meta": { "...": "..." },
  "audio": [
    { "type": "audio", "seq": 1, "is_last": false, "chunk": "..." },
    { "type": "audio", "seq": 2, "is_last": true,  "chunk": "..." }
  ]
}
```

### 2) 事件触发 `POST /v1/robot/event`
请求 JSON：
```json
{
  "type": "event",
  "event": "idle",        // 如 idle/touch/wake/sleep，自定义事件也可
  "req": "r456",
  "user_id": "demo",
  "chunk_bytes": 500,
  "mode": "stream",
  "af": "mp3_16k_32kbps"
}
```
返回结构与 `/v1/robot` 相同。

### 3) 语音输入单次调用 `POST /v1/robot/voice`
请求 JSON：
```json
{
  "type": "voice",
  "audio_data": "<Base64 of WAV/PCM/MP3>",
  "audio_format": "wav_16k_16bit", // MP3 时可用 mp3_16k_32kbps
  "language": "zh-CN",
  "req": "r789",
  "user_id": "demo",
  "chunk_bytes": 500,
  "mode": "stream",
  "af": "mp3_16k_32kbps"
}
```
返回结构同上，`text` 为识别+生成的最终文本。

## WebSocket 流式语音 `WS /v3/robot/voice`（推荐）
连接地址：`ws://<host>/v3/robot/voice`（https 对应 wss）。

### 上行流程
1. 发送 `start` 文本 JSON（必须先发）：
```json
{
  "type": "start",
  "req": "r001",              // 可选
  "user_id": "demo",          // 可选
  "af": "wav_16k_16bit",      // 默认 mp3_16k_32kbps；含 mp3 字样则按 MP3 缓冲
  "mode": "stream",           // "stream" 分片；"single" 整包
  "chunk_bytes": 500,         // 下行分片大小，未填默认 500B
  "language": "zh-CN"         // ASR 语言
}
```
2. 连续发送音频二进制分片（不要 Base64）。推荐 3~8KB/片，格式 16k/16bit/mono PCM/WAV。MP3 也可，但需要服务端有 `ffmpeg`。
3. 发送 `{"type":"end"}` 文本 JSON 表示音频结束。

### 下行消息顺序
1) `asr_text`：最终识别文本  
2) `meta`：表情/动作/音频格式  
3) `audio`：TTS 分片序列，`is_last=true` 结束

### 设备端处理要点
- 按 `seq` 顺序播放 `audio`，收到 `is_last=true` 即结束一轮。
- `meta.anim` / `meta.motion` 可直接驱动表情与动作；未匹配情绪时会回落到 `neutral`/`idle`。
- 连接关闭代码 1011 表示服务端内部错误，可查看日志；若提示 MP3 解码失败请改用 WAV/PCM 或安装 `ffmpeg`。

## 音频格式支持与建议
- 默认：`mp3_16k_32kbps`（带宽省、延迟低）。
- 其他可选（部分示例）：`mp3_16k_64kbps`、`mp3_24k_48kbps`、`pcm_16k_16bit`、`wav_16k_16bit`、`wav_24k_16bit` 等（见 `tts.py` 中 `SUPPORTED_AUDIO_FORMATS`）。
- 若 `mode="single"`，服务端会合成整包音频后一次返回（更易播放但增加首包等待）。

## 简易 WS 参考代码
```python
# 需安装 websockets
import asyncio, json, base64, websockets

async def run(server, pcm_chunks):
    async with websockets.connect(server.replace("http", "ws") + "/v3/robot/voice") as ws:
        await ws.send(json.dumps({"type":"start","af":"wav_16k_16bit","mode":"stream"}))
        for c in pcm_chunks:
            await ws.send(c)
        await ws.send(json.dumps({"type":"end"}))

        async for m in ws:
            obj = json.loads(m)
            if obj.get("type") == "asr_text":
                print("ASR:", obj["text"])
            elif obj.get("type") == "meta":
                print("META:", obj)
            elif obj.get("type") == "audio":
                data = base64.b64decode(obj["chunk"])
                # 播放 data；is_last=True 表示结束
```

## 常见问题
- 未收到 `asr_text`：检查音频采样率/位深/单声道与时长（建议 ≥3s），或网络稳定性。
- MP3 失败：确认 `ffmpeg` 已安装可执行；否则改用 WAV/PCM 上传。
- 返回 400：`audio_data`/`u`/`event` 为空，或 JSON 字段缺失。
- 延迟优化：使用 PCM/WAV，保持 3~8KB 分片，尽量减少网络抖动；需要极低延迟可选 `mode="stream"`。
