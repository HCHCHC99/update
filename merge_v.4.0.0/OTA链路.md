# OTA 链路完全讲解（Bootloader + APP）

> 面向初学者：把 boot 和 APP 两侧的 OTA 处理，从 CAN 总线数据帧一路讲到写 Flash、跳转、复位补响应，逐层拆开。
> 涉及的工程：
> - Bootloader：`D:\ota_ddl3.3_v.3.1\merge_v.2.0.0\boot`
> - 四驱 APP1/APP2：`D:\260706_NL\project\app1|app2`

---

## 1. 一张图看懂整体分层

```
TBOX（上位机/刷写工具）
   │  CAN 总线：250kbps，扩展帧
   ▼
┌─────────────────────────────────────────────────────────────┐
│ ① CAN 硬件层          HC32F460 CAN 外设 + 驱动               │
│ ② CAN 适配层          Adapter_Can（boot）/ adapter_can（APP）│
│ ③ ISO-TP 传输层       isotp_transport（SF/FF/CF/FC 分包）    │
│ ④ UDS 服务层          uds_diagnostic（SID 分发、会话、安全）  │
│ ⑤ OTA 下载执行层      flash_download（擦写 Flash、状态机）   │
│ ⑥ 应用封装层          uds_ota（初始化、轮询、延迟复位）       │
│ ⑦ 业务层（boot）      Bootloader_App（启动序列、槽切换、UDS主循环）│
│ ⑧ 记录层              rmu（复位原因/故障计数，状态扇区）      │
└─────────────────────────────────────────────────────────────┘
```

每一层的职责一句话版：

| 层 | 干什么 | 类比 |
| --- | --- | --- |
| ① CAN 硬件层 | 收发 CAN 帧、进接收缓冲 | 网卡 |
| ② CAN 适配层 | 把硬件帧转成通用结构体，按 CAN ID 分发给回调 | 驱动/分发器 |
| ③ ISO-TP 层 | 把一条 UDS 消息拆成多个 CAN 帧发，或把多个帧拼成一条消息 | 分包/组包 |
| ④ UDS 服务层 | 认 SID（0x34/0x36/0x37…），调对应的处理函数，组装响应 | 业务接口分发 |
| ⑤ 下载执行层 | 真正擦 Flash、写 Flash、算 CRC、维护下载状态机 | 执行器 |
| ⑥ 应用封装层 | 把上面几层串起来初始化、按 1ms 轮询、处理延迟复位 | 胶水 |
| ⑦ Boot 业务层 | 上电先干嘛、要不要进 UDS、跳哪个 APP、烧完做什么 | 主流程 |
| ⑧ RMU 记录层 | 记住每次复位原因、APP 故障次数（坏块判断依据） | 黑匣子 |

---

## 2. 分层详解：每一层由哪个文件负责

### ① CAN 硬件层

- 位置：`drivers/hc32_ll_driver/inc|src/hc32_ll_can.*`（boot 和 APP 都有）
- 作用：初始化 CAN 控制器（250kbps、扩展帧、PB14=RX/PB15=TX）、收发报文、RX 中断。
- 你不直接碰它，它被适配层调用。

### ② CAN 适配层

- **boot**：`boot/projects/ev_hc32f460_lqfp100_v2/Adp/Adapter_Can.c/.h`
- **APP**：`D:\260706_NL\project\app1\app\can\uds\adapter_can.c/.h`（四驱 APP 也有独立一份）

关键接口：
```c
CanIf_Init();                          // 初始化 CAN
CanIf_Poll();                          // 主循环里调用：把硬件 RX 缓冲里的帧取出来分发
CanIf_Send(&msg);                      // 发送一帧
CanIf_RegisterRxFilter(&entry);        // 注册“匹配某个 CAN ID 就调用回调”的过滤项
CanIf_SetDefaultRxCallback(&cb);       // 没匹配到任何过滤项时的兜底回调
```

分发逻辑在 `CanIf_DispatchRx()`：
```c
for (每个注册的过滤器) {
    if (匹配) 调用该过滤器的回调;
}
if (没匹配 && 默认回调存在) 调用默认回调;
```

> 一句话：**这一层负责“哪一帧 CAN 数据交给谁”**。UDS 报文注册了 4 个 ID 的过滤器，裸帧指令（0x18FF5858 / 0x18FF5555）走各自的回调/入口。

### ③ ISO-TP 传输层

- 文件：`UDS/isotp_transport.c/.h`（boot 与 APP 各有一份，内容同源）
- 解决什么问题：一帧 CAN 只有 8 字节，一条 UDS 消息可能几百上千字节（比如 0x36 一次传 258 字节）。ISO 15765-2 规定怎么拆：
  - **SF 单帧**：≤7 字节，`0x0N` 开头（N=长度）
  - **FF 首帧**：`0x10 LL`，声明总长
  - **CF 连续帧**：`0x2X`，X=序号，最多 7 字节数据
  - **FC 流控帧**：`0x3X`，告诉对方“继续/等待/溢出”
- 关键函数：
```c
isotp_init(channel);                          // 初始化
isotp_receive_frame(channel, can_id, data, len, out_buf, &out_len);
    // 喂一帧 → 内部组包，组完一条完整消息返回 ISOTP_OK
isotp_send_message(channel, dst_id, data, len);
    // 自动把消息拆成 FF/CF 发送，等待/处理 FC
isotp_ms_update();   // 1ms 定时：超时管理
isotp_tx_process();  // 发送状态机推进（把排队的 CF 发出去）
```

### ④ UDS 服务层（重点：服务分发在这里）

- 文件：`UDS/uds_diagnostic.c/.h`
- **唯一入口**：`uds_receive_handler(channel, can_id, data, len)`——所有完整 UDS 消息都从这里进来。
- 它做 4 件事：
  1. **CAN ID 过滤**：只认物理请求 `0x18DA03F1`、功能请求 `0x18DBFFF0`（可开关）；
  2. **取 SID** = `data[0]`；
  3. **按 SID 分发**（switch）到对应处理函数；
  4. **统一回响应**：处理函数填好 `response_buf/response_len`，这里统一 `uds_send_response()` 发出去；处理函数没填响应（`resp_len=0`）就不回（用于 0x11/0x31 延迟复位场景）。

分发表（`uds_receive_handler` 里的 switch）：

| SID | 服务 | 处理函数 | OTA 中的作用 |
| --- | --- | --- | --- |
| 0x10 | 诊断会话控制 | `uds_handle_diagnostic_session_control` | 进入编程会话（02） |
| 0x11 | ECU 复位 | `uds_handle_ecu_reset` | 烧完复位 |
| 0x14 | 清除 DTC | `uds_handle_clear_dtc` | 辅助 |
| 0x19 | 读 DTC | `uds_handle_read_dtc_info` | 辅助 |
| 0x22 | 读 DID | `uds_handle_read_data_by_id` | 读版本等 |
| 0x27 | 安全访问 | `uds_handle_security_access` | 解锁后才能下载 |
| 0x2E | 写 DID | `uds_handle_write_data_by_id` | 辅助 |
| 0x31 | 例程控制 | `uds_handle_routine_control` | 进 bootloader / 擦除 / CRC |
| 0x34 | 请求下载 | `uds_handle_request_download` | **开始下载** |
| 0x36 | 传输数据 | `uds_handle_transfer_data` | **传固件内容** |
| 0x37 | 请求退出传输 | `uds_handle_request_transfer_exit` | **收尾** |
| 0x3E | 测试仪在线 | `uds_handle_tester_present` | 保活 |

> 注意：**uds_diagnostic 只做“服务分发”，不直接擦 Flash**。0x34/0x36/0x37 处理函数内部调用第 ⑤ 层 `FlashDownload_*` 接口，把结果转成 UDS 响应（74/76/77 或 7F 负响应）。

### ⑤ OTA 下载执行层

- 文件：`UDS/flash_download.c/.h`
- 职责：真正的 Flash 操作 + 下载状态机。
- 状态机（`FlashDownloadState_t`）：
```
IDLE → PREPARING → READY → TRANSFERRING → VERIFYING → COMPLETE
                                  ↘ ERROR
```
- 对外接口：
```c
FlashDownload_OnRequestDownload(address, size);  // 0x34：校验地址→擦扇区→READY
FlashDownload_OnTransferData(seq, data, len);    // 0x36：写 Flash（实时写）+ CRC + 序号校验
FlashDownload_OnTransferExit();                  // 0x37：补 CRC、置 COMPLETE
FlashDownload_GetState();                        // 查询状态（Bootloader_UdsMain 用它判断“烧完了”）
FlashDownload_GetProgress(&progress);            // 目标地址/进度
```
- **地址映射在这里**：
```c
mapped_addr = MAP_TBOX_ADDR_TO_FLASH(address);
// TBOX_ADDR_APP1(0x08018000) → APP1_START_ADDR(0x1A000)
// TBOX_ADDR_APP2(0x08004000) → APP2_START_ADDR(0x44000)
// 其它地址原样返回，再由地址范围检查决定是否接受
```
- 擦除时机：0x34 里立即按扇区擦除；写入时机：0x36 每块实时写。

### ⑥ 应用封装层

- 文件：`UDS/uds_ota.c/.h`
- 把 ②③④⑤ 串起来，提供 4 个“大接口”：
```c
UdsOta_Init();                     // 初始化 ISO-TP + 注册 4 个 UDS ID 过滤器 + FlashDownload + uds_init
UdsOta_Poll();                     // 主循环每圈调用：1ms 门控（延迟复位/ISO-TP 超时/UDS 会话计时/TX 推进）+ FlashDownload_Task + CanIf_Poll
UdsOta_Bootloader_Enter();         // boot 进入编程模式（调 Bootloader_UdsMain）
UdsOta_App_CheckPendingAck();      // APP 启动时补发挂起的 0x51/0x71
```
- 关键回调（在 uds_ota.c 里）：`ISOTP_RxCallback()`——被 CanIf 过滤器调用，把帧喂给 `isotp_receive_frame`，组完一条消息后调 `uds_receive_handler`。这就是 ②→③→④ 的桥。

### ⑦ Boot 业务层

- 文件：`Bootloader_App/Bootloader_App.c/.h` + `Bootloader_App/memory_map.h`

**上电主流程 `Boot_StartupSequence()`（boot 的 main 里调用）：**
1. **读上次运行槽 + RMU 处理**：`GetCurrentSlot()` → `Rmu_ProcessPowerUp(当前槽)`（记录本次复位原因/故障计数）；
2. **50ms 强制指令窗口**：注册 `0x18FF5858` 过滤器，收到指令：
   - `0xFF` → 直接进 UDS 编程模式；
   - `0x01/0x02` → 坏块检查后设置下次启动槽，软件复位；
   - 双坏 → 回 `0x03` 并进 UDS 等刷写；
3. **UDS 共享区检查**：读 `UDS_SHARED`，若 `phase==ENTER_BOOTLOADER` → 进 `Bootloader_UdsMain()`（APP 请求进 boot 的通道）；
4. **槽选择**：`InitAppInfo`（从 flash 读两个 APP 的故障计数/固件有效性）→ `UpdateAppState`（fault>=3 或固件无效 = DISABLED）→ `SelectTargetSlot`（当前槽可用优先，否则切另一个，双坏 = 不进 APP）→ `UpdateSlotFlagToFlash`（必要时把新目标写回 APP_RUN_SLOT）→ `Bootloader_JumpToApp(目标地址)`。

**编程模式主循环 `Bootloader_UdsMain()`（不返回）：**
```c
while (1) {
    if (500ms 到) SWDT_FeedDog();   // boot 自己也要喂狗
    UdsOta_Poll();                   // 处理 CAN/UDS/ISO-TP/下载
    if (FlashDownload_GetState() == FW_UPDATE_COMPLETE) {
        // 烧录完成：
        // 1. 写 UDS_SHARED（phase=PROGRAMMING_DONE, target_slot, 强制 magic）
        // 2. 整槽清零该 APP 的 RMU 记录（Rmu_ResetSlotRecord）
        // 3. 设置自动跳转槽 = 刚烧的 APP（Boot_SetRunSlotToAddr）
    }
}
```

**UDS 共享区（boot 与 APP 的“留言板”）`stc_uds_shared_t`**（存在 Flash 0x10000）：
```c
magic / phase / target_slot / fw_size / fw_crc / result / pending_sid / reserved
```
- APP 想进 boot：写 `phase=ENTER_BOOTLOADER, pending_sid=0x31` → 延迟复位；
- boot 烧完：写 `phase=PROGRAMMING_DONE`；
- 复位响应：boot 收到 0x11 时写 `pending_sid=0x11` → APP 启动补发 0x51。

### ⑧ RMU 记录层

- 文件：`Utils/rmu.c/.h`
- 状态扇区：APP1=0x16000、APP2=0x18000，每槽一条 `stc_rmu_slot_record_t`（含 fault_count、最近故障原因、故障原因计数等）。
- 职责：上电第一件事读 RSTF0 → 分类（SWDT/WDT/MPU 是故障）→ 故障计数 <3 才 +1 → **仅故障复位/首次初始化才写 Flash**（保护寿命）→ 提供 `Rmu_GetFaultCount()` 给槽选择判断坏块。

---

## 3. APP 侧（四驱控制盒）的 OTA 相关代码

APP 没有“下载执行层”的使用（下载在 boot 里做），但它有**诊断栈**和**进 boot 的入口**：

| 文件（app1 为例） | 职责 |
| --- | --- |
| `app/can/uds/uds_rx_entry.c/.h` | **CAN 入口分发**：裸帧指令先拦（0x18FF5858 强制 OTA、0x18FF5555 停/恢复喂狗），UDS 4 个 ID 交给 ISO-TP 组包再进 uds_receive_handler |
| `app/can/uds/uds_diagnostic.c/.h` | UDS 服务层（与 boot 同源）：0x31 例程（写 UDS_SHARED + 延迟复位进 boot）、0x11 复位、0x10/0x27/0x22/0x2E/0x3E 等 |
| `app/can/uds/uds_ota.c/.h` | 轮询封装：`g_force_ota_cmd` 检测到就软复位进 boot；延迟复位倒计时 |
| `app/can/uds/bootloader_app.c/.h` | **UDS_SHARED 读写 + 补发 ACK**：`App_CheckPendingUdsAck()` 启动时读留言板，`pending_sid==0x11` 补发 0x51，`==0x31` 补发 0x71，然后清留言板 |
| `app/can/uds/isotp_transport.c/.h` | ISO-TP 层（同 boot） |
| `app/can/uds/memory_map.h` | 分区宏（与 boot 保持一致） |
| `app/can/app_can.c` | CAN 接收主函数：`app_can_receive()` 对未知 ID 调 `uds_rx_entry()` |
| `startup/main.c` | 主循环：`SWDT_FeedDog()`（受 `g_swdt_feed_disable` 控制）+ `UdsOta_Poll()`；启动时 `UdsOta_App_CheckPendingAck()` |

---

## 4. 一次完整 OTA 的报文级走查（TBOX 视角）

假设当前在 APP1 运行，TBOX 要刷 APP1 新固件：

```
① 进 boot（两条路任选）
   路 A（强制指令，裸帧）：TBOX 发 0x18FF5858 / data=0xFF
      → APP：uds_rx_entry 记 g_force_ota_cmd=0xFF → UdsOta_Poll 检测到 → NVIC_SystemReset()
   路 B（诊断服务）：TBOX 发 10 02 → 27 01/02 → 31 01 FE00（进入 boot 例程）
      → APP：uds_diagnostic 处理 0x31 → 写 UDS_SHARED(ENTER_BOOTLOADER) → 延迟复位
   复位后 boot 启动 → Boot_StartupSequence → 看到 ENTER_BOOTLOADER → Bootloader_UdsMain()

② 建立编程会话 + 解锁
   TBOX → ECU：02 10 02            （0x10 诊断会话控制，02=编程）
   ECU  → TBOX：02 50 02
   TBOX → ECU：02 27 01            （0x27 安全访问，请求种子）
   ECU  → TBOX：06 67 01 xx xx xx xx
   TBOX → ECU：06 27 02 xx xx xx xx（发密钥）
   ECU  → TBOX：02 67 02

③ 下载固件
   TBOX → ECU：10 0B 34 00 44 88 01 00 / 21 00 00 01 2A DC 00 00
              （0x34 请求下载：地址 4 字节=0x88010000(APP1 标签)，长度 4 字节=0x00012ADC）
   uds_receive_handler → uds_handle_request_download
     → FlashDownload_OnRequestDownload(0x88010000, 76508)
     → MAP_TBOX_ADDR_TO_FLASH → 0x1A000（APP1）→ 地址校验 → 擦扇区 → READY
   ECU  → TBOX：03 74 40 00        （0x74 肯定，最大块长 0x4000）

   TBOX → ECU：11 02 36 01 ...     （0x36 传输数据，258 字节/块，首帧+连续帧）
   uds_receive_handler → uds_handle_transfer_data
     → FlashDownload_OnTransferData(seq, data, len) → 写 Flash → 76 响应
   ECU  → TBOX：03 76 76 2B        （0x76 肯定 + 块序号）

   …（重复很多块）…

   TBOX → ECU：01 37             （0x37 请求退出传输）
   uds_receive_handler → uds_handle_request_transfer_exit
     → FlashDownload_OnTransferExit() → CRC 收尾 → COMPLETE
   ECU  → TBOX：01 77             （0x77 肯定）

④ boot 烧完收尾（Bootloader_UdsMain 检测到 COMPLETE）
   - 写 UDS_SHARED（phase=PROGRAMMING_DONE, target_slot=APP1, 强制 magic=UDS_SHARED_MAGIC）
   - Rmu_ResetSlotRecord(APP1)（整槽清零 APP1 的 RMU 记录，APP2 不动）
   - Boot_SetRunSlotToAddr(APP1_START_ADDR)（自动跳转槽 = APP1）

⑤ 复位确认
   TBOX → ECU：10 09 31 01 FE01 … / 21 …   （0x31 例程，可选）
   ECU  → TBOX：02 71 01
   TBOX → ECU：02 11 01                     （0x11 ECU 复位）
   boot 收到 0x11（boot 上下文）：不回 51，写 pending_sid=0x11，延迟 100ms 复位
   boot 复位 → Boot_StartupSequence → 正常启动路径 → 跳 APP1

⑥ APP1 启动补发
   APP1 main → UdsOta_App_CheckPendingAck()
     → 读 UDS_SHARED：magic 有效 && pending_sid==0x11
     → 发 04 51 01 00 00 00 00 00（0x51 复位肯定响应）
     → UdsShared_Clear()
   TBOX 收到 51 → 认为复位完成 → 流程结束
```

---

## 5. 每个“收到”和“回复”在代码里的落点

| 报文 | 收到后谁处理 | 回复在哪生成 |
| --- | --- | --- |
| CAN 帧 | CanIf_Poll（boot）/ app_can_receive（APP）取出 | — |
| UDS 帧组包 | ISOTP_RxCallback / uds_rx_entry → isotp_receive_frame | isotp_send_message |
| SID 分发 | uds_receive_handler（switch） | uds_send_response |
| 0x34 | uds_handle_request_download | 74 / 7F |
| 0x36 | uds_handle_transfer_data | 76 / 7F / 78 挂起 |
| 0x37 | uds_handle_request_transfer_exit | 77 / 7F |
| 0x31 | uds_handle_routine_control | 71 / 7F |
| 0x11 | uds_handle_ecu_reset | boot 上下文不回（靠 APP 补发 51）；APP 上下文回 51 |
| 0x18FF5858 | boot：Boot_ForceCmdRxCallback；APP：uds_rx_entry | 0x18EF5858（Boot_SendForceResp） |
| 0x18FF5555 | APP：uds_rx_entry | 0x18EF5555（BootTest_SendResp） |

---

## 6. 关键 CAN ID 速查

| ID | 方向 | 用途 |
| --- | --- | --- |
| 0x18DA03F1 | TBOX → ECU | 物理寻址请求 |
| 0x18DAF103 | ECU → TBOX | 物理寻址响应 |
| 0x18DBFFF0 | TBOX → ECU | 功能寻址请求（广播） |
| 0x18FF8118 | 双向 | OTA 专用 ID |
| 0x18FF5858 | TBOX → ECU | 强制 OTA 指令（裸帧：0xFF 进 boot / 0x01/0x02 设槽） |
| 0x18EF5858 | ECU → TBOX | 强制指令回帧 |
| 0x18FF5555 | TBOX → ECU | 测试：停止/恢复喂 SWDT（0x01/0x00） |
| 0x18EF5555 | ECU → TBOX | 测试指令回帧 |

---

## 7. 关键数据结构速查

| 结构体 | 文件 | 作用 |
| --- | --- | --- |
| `CanMsg_t` | Adapter_Can.h | 通用 CAN 帧 |
| `CanIf_RxFilterEntry_t` | Adapter_Can.h | 过滤器（ID/Mask/回调） |
| isotp 状态机结构 | isotp_transport.h | ISO-TP 收发状态 |
| `stc_uds_shared_t` | Bootloader_App.h（boot）/ bootloader_app.h（APP） | UDS 留言板 |
| `stc_app_info_t / stc_boot_context_t` | Bootloader_App.h | 槽信息/启动上下文 |
| `stc_rmu_slot_record_t` | rmu.h | 每槽 RMU 记录（fault_count 等） |
| `FlashDownloadProgress_t` | flash_download.h | 下载进度/目标地址 |

---

## 8. 调试手段

- RTT 打印：
  - `[RMU] raw=... cause=... fault=...`：上电复位原因/故障计数
  - `[RMU] cnt : WDT=.. SWDT=.. MPU=..`：故障原因累计
  - `CurSlot: APP1, Target: APP1` / `APP1 state=AVAILABLE, fault=0`：槽选择结果
  - `>>> OnRequestDownload: orig_addr=0x..., mapped_addr=0x...`：34 地址映射
  - `State: 0 -> 3`（FlashDownload 状态机流转）
  - `Sending pending 11 01 ACK (51 01)`：APP 补发 0x51
- Keil Watch：
  - `g_stcRmuLastCauseApp1/2`、`g_stcRmuReasonCountApp1/2`：两槽 RMU 视图
  - `*(unsigned long*)0x16008` / `0x18008`：APP1/APP2 fault_count（flash 原值）
  - `stcCtx`（断在 Boot_StartupSequence 的 SelectTargetSlot 之后）：看槽选择
- 断点推荐：
  - `uds_receive_handler`：看每条 UDS 消息进来
  - `uds_handle_request_download / transfer_data / request_transfer_exit`：看下载三步
  - `FlashDownload_OnTransferExit`：看下载完成
  - `Rmu_ProcessPowerUp`：看每次上电 RMU 处理