# AT+RST 复位控制说明书（面向自动化 Agent）

本文件描述 `usb_uart_bridge` 固件内置的复位控制协议。Agent 依此即可通过虚拟串口完成单路复位、多路顺序复位，并动态调整间隔、脉冲宽度和默认电平。

---

## 1. 硬件前提

| 项目 | 值 |
|------|-----|
| 复位输出 1 | **GPIO4** |
| 复位输出 2 | **GPIO5** |
| 复位输出 3 | **GPIO6** |
| 复位输出 4 | **GPIO7** |
| 复位输出 5 | **GPIO8** |
| 复位输出 6 | **GPIO9** |
| 逻辑电平 | 3.3V |
| 默认电平 | 低（可改，每脚独立） |
| 共地 | 必须与目标设备共地 |

脚号与 GPIO 的对应关系固定为 `脚号 N → GPIO(N+3)`，即 1→GPIO4、2→GPIO5、3→GPIO6、4→GPIO7、5→GPIO8、6→GPIO9。

六个复位脚默认工作在**高阻空闲模式**：平时输出关闭（输入保留用于电平采样）、上下拉关闭，仅在复位脉冲期间临时切换为推挽输出，脉冲结束后立即回到高阻。这样不会持续驱动目标 MCU 的复位脚。

**高阻模式下仍可随时读取引脚电平**（`AT+RSTLVL`），读到的是目标板上的真实电平，因为输入缓冲器保持使能且不向外部输出电流。

复位动作 = 切为输出并驱动与默认电平相反的电平，保持该脚自身的 `PULSE` 毫秒，然后回到高阻。

> 高阻空闲时，目标复位脚的电平由目标板自身决定；本设备不参与。若目标板复位脚悬空，请自行加上/下拉。

可用 `AT+RSTMODE=0` 切换为"推挽空闲"模式（平时持续输出默认电平），恢复到旧版行为。

> ⚠️ 3.3V 推挽直连时，若目标复位脚已被上拉到 3.3V 且为强推挽，注意短路电流；必要时串 1kΩ 限流。


---

## 2. 通信端口

设备枚举为 USB 复合设备，出现 **2 个虚拟 COM 口**：

| 端口 | 桥接 UART | TX / RX | 是否支持 AT |
|------|-----------|---------|-------------|
| CDC0（第一个 COM） | UART1 | GPIO17 / GPIO18 | 是 |
| CDC1（第二个 COM） | UART2 | GPIO15 / GPIO16 | 是 |

要点：

- **两个口都能解析 AT**，协议完全一致。
- **指令从哪个口进入，响应就从哪个口返回。**
- 若只做复位控制，**任选一个 COM 即可**，无需区分 CDC0/CDC1（两者对 AT 的行为完全相同）。
- 非 AT 数据仍按原样透传到该口对应的 UART，不影响桥接功能。
- 串口参数（波特率等）与 AT 解析无关，任意波特率均可。建议 115200 8N1。
- 打开端口时会置 DTR，AT 响应不依赖 DTR 状态。

---

## 3. 帧格式

### 3.1 发送

```
<命令正文><终止符>
```

- 终止符：使用**单个** `\r`(0x0D) 或 `\n`(0x0A)。**推荐用 `\r`。**
- ⚠️ **不要用 `\r\n`**：命令解析完成后，第二个终止符字节会被当作普通数据透传到该口的 UART，即向下游泄漏一个 `0x0A`。详见 §8 第 2 条。
- 命令**区分大小写**，除 `dl` 外全部为大写。
- 单行最长 **95 字节**（含终止符）。

### 3.2 接收

响应固定为：

```
<响应正文>\r\n
```

即**始终以 `\r\n` 结尾**。读取时应按行解析。

---

## 4. 命令总表

| 命令 | 作用 | 响应 |
|------|------|------|
| `AT` | 连通性测试 | `OK` |
| `AT+RST=1` | 仅复位 GPIO4 | `OK` |
| `AT+RST=2` | 仅复位 GPIO5 | `OK` |
| `AT+RST=3` | 仅复位 GPIO6 | `OK` |
| `AT+RST=4` | 仅复位 GPIO7 | `OK` |
| `AT+RST=5` | 仅复位 GPIO8 | `OK` |
| `AT+RST=6` | 仅复位 GPIO9 | `OK` |
| `AT+RST=12` | 先 GPIO4，间隔后 GPIO5 | `OK` |
| `AT+RST=BOTH` | 同 `AT+RST=12` | `OK` |
| `AT+RST=21` | 先 GPIO5，间隔后 GPIO4 | `OK` |
| `AT+RST=123` | 依次 GPIO4 → GPIO5 → GPIO6 | `OK` |
| `AT+RST=ALL` | 依次 GPIO4 → … → GPIO9（全部 6 路） | `OK` |
| `AT+RST=<任意排列>` | 数字 1..6 的任意不重复排列，如 `456`、`31` | `OK` |
| `AT+RSTCFG?` | 查询当前参数 | `+RSTCFG:...` |
| `AT+RSTCFG=<间隔>,<脉冲>,<默认电平>` | 设置参数（对六脚统一生效） | `OK` |
| `AT+RSTCFG=<间隔>,<脚号>,<脉冲>,<默认电平>` | 只设置指定脚（脚号 1..6） | `OK` |
| `AT+RSTMODE?` | 查询空闲模式 | `+RSTMODE:HIGHZ` 或 `+RSTMODE:DRIVE` |
| `AT+RSTMODE=1` | 空闲高阻（默认，平时不驱动） | `OK` |
| `AT+RSTMODE=0` | 空闲推挽（平时持续输出默认电平） | `OK` |
| `AT+RSTLVL` 或 `AT+RSTLVL?` | 读取全部六脚当前电平 | `+RSTLVL:P1=0,P2=0,...` |
| `AT+RSTLVL=<脚号>` | 读取单个脚电平（脚号 1..6） | `+RSTLVL:P3=1` |
| `AT+DL` 或 `dl` | 进入 USB 下载模式 | **无响应**（设备复位） |

> 复位顺序串的数字不能重复，如 `AT+RST=11` 视为未识别命令（会被透传到 UART）。


---

## 5. 参数语义

### 5.1 `AT+RSTCFG=` 参数

**三参数形式（统一设置所有复位脚）**

| 位置 | 名称 | 单位 | 合法范围 | 出厂默认 |
|------|------|------|----------|----------|
| 1 | 间隔 | ms | 0 – 60000 | 100 |
| 2 | 脉冲宽度 | ms | 1 – 60000 | 100 |
| 3 | 默认电平 | 0/1 | 0 或 1 | 0 |

**四参数形式（只设置单个复位脚）**

| 位置 | 名称 | 单位 | 合法范围 | 出厂默认 |
|------|------|------|----------|----------|
| 1 | 间隔 | ms | 0 – 60000 | 100 |
| 2 | 脚号 | 1..6 | 1=GPIO4，2=GPIO5，3=GPIO6，4=GPIO7，5=GPIO8，6=GPIO9 | — |
| 3 | 脉冲宽度 | ms | 1 – 60000 | 100 |
| 4 | 默认电平 | 0/1 | 0 或 1 | 0 |

- **间隔**：多路复位时，**前一个脉冲结束**到**后一个脉冲开始**之间的等待时间。单路复位时该参数不起作用。**六路共享同一个间隔**。
- **脉冲宽度**：复位有效电平的持续时间。**每个脚独立**。
- **默认电平**：引脚空闲时输出的电平。设置后**立即**把对应脚驱动到该电平。**每个脚独立**。
- 四参数形式同样会更新全局间隔（间隔只有一个）。


### 5.2 时序

设默认电平 = 0、脉冲 = 100ms、间隔 = 100ms。

**单路复位 `AT+RST=1`**

```
GPIO4  ─┐          ┌──── 默认低
        └── 100ms ──┘
t=0     : 拉高（复位有效）
t=100   : 拉低（回默认）
t≈100   : 返回 "OK\r\n"
```

**双路复位 `AT+RST=12`**

```
GPIO4  ─┐          ┌──────────────────── 默认低
        └── 100ms ──┘
                   │← 间隔 100ms →│
GPIO5  ────────────────────────────┐          ┌── 默认低
                                   └── 100ms ──┘
t=0     : GPIO4 拉高
t=100   : GPIO4 拉低
t=200   : GPIO5 拉高
t=300   : GPIO5 拉低
t≈300   : 返回 "OK\r\n"
```

总耗时 = `脉冲1 + 间隔 + 脉冲2`。

**三路复位 `AT+RST=123`**

```
GPIO4  ─┐          ┌─── 默认低
        └─ p1 ─────┘
                   │← 间隔 →│
GPIO5  ───────────────────┐          ┌── 默认低
                          └─ p2 ─────┘
                                     │← 间隔 →│
GPIO6  ──────────────────────────────────────┐          ┌── 默认低
                                             └─ p3 ─────┘
t≈p1+p2+p3+2×间隔 : 返回 "OK\r\n"
```

**六路复位 `AT+RST=ALL`（=123456）**

按 `AT+RST` 串中的顺序逐个输出脉冲，相邻脉冲之间插入 `间隔`：

```
GPIO4  ─┐     ┌──
        └─p1──┘  │←间隔→│
GPIO5  ────────────────┐     ┌──
                       └─p2──┘  │←间隔→│
GPIO6  ───────────────────────────────┐ ...
GPIO7  ─────────────────────────────── ...
GPIO8  ─────────────────────────────── ...
GPIO9  ─────────────────────────────── ...
t≈Σp + 5×间隔 : 返回 "OK\r\n"
```

总耗时 = `p1 + p2 + ... + pN + (N-1)×间隔`。若各脚脉冲宽度相同（统一配置），则 `N×脉冲 + (N-1)×间隔`。

> 若默认电平设为 1，则上述波形极性反转：复位表现为**低电平脉冲**。

---

## 6. 响应格式

| 场景 | 响应 |
|------|------|
| 通用成功 | `OK\r\n` |
| 参数查询 | `+RSTCFG:INTERVAL=100,IDLE=HIGHZ,PIN1=4,PULSE1=100,DEFAULT1=0,...,PIN6=9,PULSE6=100,DEFAULT6=0\r\n` |
| 复位命令队列已满 | `ERROR\r\n` |

`AT+RSTCFG?` 字段说明：

| 字段 | 含义 |
|------|------|
| `INTERVAL` | 当前间隔（ms，全局共享） |
| `IDLE` | 空闲模式：`HIGHZ`（高阻，默认）或 `DRIVE`（持续输出默认电平） |
| `PIN1` … `PIN6` | 复位脚编号（固定 4、5、6、7、8、9） |
| `PULSE1` … `PULSE6` | 各脚当前脉冲宽度（ms） |
| `DEFAULT1` … `DEFAULT6` | 各脚当前默认电平 |

### 电平读取响应

| 命令 | 响应 |
|------|------|
| `AT+RSTLVL` / `AT+RSTLVL?` | `+RSTLVL:P1=0,P2=0,P3=1,P4=0,P5=0,P6=0\r\n` |
| `AT+RSTLVL=3` | `+RSTLVL:P3=1\r\n` |

- `P<n>` 为脚号（1..6），值为 `0`（低）或 `1`（高）。
- **`HIGHZ` 空闲模式**：读到的是目标板真实电平，可用于判断目标 MCU 复位脚状态/是否掉电。
- **`DRIVE` 空闲模式**：读到的是本设备自己驱动的默认电平，参考价值有限。
- 复位脉冲进行中读取可能读到脉冲电平（正在驱动）。

> 解析方式：字段串不保证顺序对外稳定，建议按 `KEY=VALUE` 逐项解析，不要依赖位置。
> 六路全量响应约 200 字节，单行建议缓冲区 ≥ 256 字节。

---

## 7. 超时建议

| 命令 | 建议等待 | 说明 |
|------|----------|------|
| `AT` | ≥ 500 ms | 即时响应 |
| `AT+RSTCFG?` | ≥ 500 ms | 即时响应 |
| `AT+RSTCFG=` | ≥ 500 ms | 即时响应 |
| `AT+RST=1` / `=2` / `=3` | ≥ `脉冲` + 500 ms | 响应在复位完成后才发出 |
| `AT+RST=12` / `=21` / `BOTH` | ≥ `p1+p2+间隔` + 500 ms | 同上 |
| `AT+RST=<N 路排列>` | ≥ `Σp + (N-1)×间隔` + 500 ms | 同上 |

**关键点**：复位类命令的 `OK` 是在**整个复位序列执行完毕之后**才发送的，不是收到命令就立即回。Agent 不能假设即时应答。

---

## 8. 边界与陷阱（务必阅读）

1. **未识别的命令不会返回 `ERROR`，而是被原样透传到对应 UART。**
   例如 `AT+RST=7`、`AT+RST=11`、`AT+FOO`、参数超范围的 `AT+RSTCFG=99999,50,0` 都不会回 `ERROR`，而是把整行写进 UART。
   → Agent 必须**自行校验参数范围**，不能依赖固件的 `ERROR` 来判断语法错误。

2. **用 `\r\n` 结尾会向下游 UART 泄漏一个 `0x0A`。**
   命令被识别并处理后，解析状态立即复位；紧随其后的第二个终止符字节不再属于命令，会被当作普通数据透传。
   例如发送 `AT\r\n`：`AT\r` 被正确解析并返回 `OK`，但剩余的 `\n` 会被写入 UART。
   → **务必只用单个 `\r`（或单个 `\n`）作为终止符**，不要用 CRLF。

3. **`ERROR` 只在一种情况下出现**：复位命令队列（深度 4）已满时。即连续快速发送超过 4 条复位命令。

4. **配置不持久化**。重启后恢复为 `间隔=100, 各脚脉冲=100, 各脚默认电平=0`。需要特定参数时，每次上电后重新设置。

5. **以 `A`(0x41) 或 `d`(0x64) 开头的普通数据会被缓冲**，直到遇到换行符或累积到 95 字节才转发到 UART。数据不会丢失，但会有延迟和重新分块。若下游是二进制协议，尽量避免以这两个字节开头，或改用另一路 CDC 传输。

6. **`AT+DL` 无响应且设备会复位**，随后以 USB 下载模式重新枚举（PID 变化，COM 号可能改变）。恢复正常运行需要**断开再插入 USB**。

7. **复位命令串行执行**。若队列中有多条命令，后续命令会排队，`OK` 也会依次延迟返回。

8. **修改默认电平会立即改变引脚状态**（仅 `DRIVE` 空闲模式；`HIGHZ` 模式下不改变引脚状态）。建议在目标设备上电前完成配置。
    - 三参数形式会同时改变**六个脚**，对另外五台设备也是一次边沿。
    - 只改一台设备时请用四参数形式（`AT+RSTCFG=<间隔>,<脚号>,<脉冲>,<电平>`）。

9. **同一端口可能同时有桥接数据回传**。若该 UART 下游设备正在发数据，读响应时不能盲目取"第一行"，应按预期格式匹配（`OK` / `+RSTCFG:` / `ERROR`），否则会把下游数据误判为 AT 响应。

10. **`AT+RST` 的顺序串不允许重复数字**。`11`、`121` 等整行会被透传到 UART，不返回 `OK`。

11. **顺序串最多 6 个数字**（`AT+RST` 行长限制 95 字节内）。`AT+RST=654321` 为六路逆序，合法。

12. **`ALL` 的语义已变更为六路**。旧固件中 `ALL` 等价于 `123`；本版本 `ALL` = `123456`。若需只复位前三路，请显式写 `AT+RST=123`。

13. **默认空闲为高阻，不复位时本设备完全不驱动目标复位脚**。目标复位脚的电平完全由其自身电路决定。若目标复位脚悬空，需自行加 10kΩ 上下拉，否则可能出现不确定电平。
    若需要旧版"持续输出默认电平"的行为，发送 `AT+RSTMODE=0`（不持久化，重启恢复高阻）。

---

## 9. 完整操作示例

### 9.1 会话流程（默认参数下单路复位）

```
主机 →  AT\r
设备 ←  OK\r\n

主机 →  AT+RST=1\r
（等待 ≥ 600ms）
设备 ←  OK\r\n
```

### 9.2 自定义参数后双路复位

```
主机 →  AT+RSTCFG=200,50,0\r
设备 ←  OK\r\n

主机 →  AT+RSTCFG?\r
设备 ←  +RSTCFG:INTERVAL=200,PIN1=4,PULSE1=50,DEFAULT1=0,PIN2=5,PULSE2=50,DEFAULT2=0,PIN3=6,PULSE3=50,DEFAULT3=0,PIN4=7,PULSE4=50,DEFAULT4=0,PIN5=8,PULSE5=50,DEFAULT5=0,PIN6=9,PULSE6=50,DEFAULT6=0\r\n

主机 →  AT+RST=12\r
（等待 ≥ 50+50+200+500 = 800ms）
设备 ←  OK\r\n
```

时序：GPIO4 高 50ms → 等待 200ms → GPIO5 高 50ms → 返回 `OK`。

### 9.3 低电平有效目标（复位脚高电平为空闲）

```
主机 →  AT+RSTCFG=100,100,1\r
设备 ←  OK\r\n
主机 →  AT+RSTMODE=0\r        ; 需持续输出默认电平时才需要
设备 ←  OK\r\n
```

之后六路复位都表现为输出 **100ms 低电平**，`DRIVE` 模式空闲时保持高电平。默认的高阻模式下空闲时引脚不驱动。

### 9.4 单独给一台设备配不同参数并复位

```
主机 →  AT+RSTCFG=100,6,500,0\r      ; 间隔100ms、6号脚(GPIO9)、脉冲500ms、空闲低
设备 ←  OK\r\n

主机 →  AT+RST=6\r
（等待 ≥ 500+500 = 1000ms）
设备 ←  OK\r\n
```

其余五路的脉冲宽度与默认电平保持不变。

### 9.5 三路 / 六路依次复位

```
主机 →  AT+RST=123\r        ; 前三路
（等待 ≥ 3×脉冲 + 2×间隔 + 500ms）
设备 ←  OK\r\n

主机 →  AT+RST=ALL\r        ; 全部六路（=123456）
（等待 ≥ 6×脉冲 + 5×间隔 + 500ms）
设备 ←  OK\r\n
```

> 顺序可任意，如 `AT+RST=654321` 表示从 GPIO9 逆序复位到 GPIO4。

---

## 10. Agent 参考实现

```python
import serial
import time

CMD_TIMEOUT = 0.5          # 即时类命令
MAX_INTERVAL = 60000
MAX_PULSE = 60000
RESET_PINS = {1: 4, 2: 5, 3: 6, 4: 7, 5: 8, 6: 9}      # 逻辑脚号 -> GPIO


class RstController:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.ser.dtr = True
        time.sleep(0.2)
        self.ser.reset_input_buffer()
        self.interval = 100
        self.pulse = {1: 100, 2: 100, 3: 100}
        self.default_level = {1: 0, 2: 0, 3: 0}

    def _send(self, cmd, timeout, expect=("OK", "+RSTCFG:", "ERROR")):
        """发送命令并按预期前缀匹配响应，跳过桥接透传的无关数据"""
        self.ser.reset_input_buffer()
        self.ser.write(cmd.encode() + b"\r")   # 单个 CR：用 CRLF 会把 \n 泄漏到 UART
        self.ser.flush()
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            buf += self.ser.read(256)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.rstrip(b"\r").decode(errors="replace")
                if text.startswith(expect):
                    return text
        return None

    def ping(self):
        return self._send("AT", CMD_TIMEOUT) == "OK"

    def query(self):
        r = self._send("AT+RSTCFG?", CMD_TIMEOUT)
        if not r or not r.startswith("+RSTCFG:"):
            return None
        out = {}
        for kv in r[len("+RSTCFG:"):].split(","):
            k, _, v = kv.partition("=")
            out[k] = int(v)
        self.interval = out["INTERVAL"]
        for p in RESET_PINS:
            self.pulse[p] = out[f"PULSE{p}"]
            self.default_level[p] = out[f"DEFAULT{p}"]
        return out

    def configure(self, interval_ms, pulse_ms, default_level):
        """统一配置三路"""
        assert 0 <= interval_ms <= MAX_INTERVAL, "间隔越界"
        assert 1 <= pulse_ms <= MAX_PULSE, "脉冲越界"
        assert default_level in (0, 1), "默认电平只能为 0 或 1"
        r = self._send(f"AT+RSTCFG={interval_ms},{pulse_ms},{default_level}", CMD_TIMEOUT)
        if r == "OK":
            self.interval = interval_ms
            for p in RESET_PINS:
                self.pulse[p], self.default_level[p] = pulse_ms, default_level
            return True
        return False

    def configure_pin(self, pin, pulse_ms, default_level, interval_ms=None):
        """只配置单路；interval_ms 省略则沿用当前间隔"""
        assert pin in RESET_PINS, "脚号只能是 1..6"
        interval_ms = self.interval if interval_ms is None else interval_ms
        assert 0 <= interval_ms <= MAX_INTERVAL, "间隔越界"
        assert 1 <= pulse_ms <= MAX_PULSE, "脉冲越界"
        assert default_level in (0, 1), "默认电平只能为 0 或 1"
        r = self._send(f"AT+RSTCFG={interval_ms},{pin},{pulse_ms},{default_level}", CMD_TIMEOUT)
        if r == "OK":
            self.interval = interval_ms
            self.pulse[pin], self.default_level[pin] = pulse_ms, default_level
            return True
        return False

    def reset(self, order):
        """order: '1'..'6' 的组合，如 '3'/'12'/'456'/'123456'/'ALL'/'BOTH'"""
        if order == "BOTH":
            order = "12"
        elif order == "ALL":
            order = "123456"
        order = str(order)
        assert 0 < len(order) <= 6 and len(set(order)) == len(order), "非法复位顺序"
        assert all(c in "123456" for c in order), "非法复位顺序"
        total = sum(self.pulse[int(c)] for c in order) + (len(order) - 1) * self.interval
        r = self._send(f"AT+RST={order}", total / 1000.0 + CMD_TIMEOUT)
        return r == "OK"

    def close(self):
        self.ser.close()


if __name__ == "__main__":
    import sys
    dev = RstController(sys.argv[1])
    print("ping:", dev.ping())
    print("cfg :", dev.query())
    print("cfg set:", dev.configure(200, 50, 0))
    print("pin6 cfg:", dev.configure_pin(6, 500, 0))
    print("pin6 reset:", dev.reset(6))
    print("all reset:", dev.reset("ALL"))
    dev.close()
```

---

## 11. 快速对照表

```
连通测试      AT                            → OK
查参数        AT+RSTCFG?                    → +RSTCFG:INTERVAL=..,IDLE=HIGHZ,PIN1=4,PULSE1=..,DEFAULT1=..,(PIN2..PIN6 同构)
设全路参数    AT+RSTCFG=<间隔>,<脉冲>,<电平>    → OK   （间隔 0-60000，脉冲 1-60000，电平 0/1）
设单路参数    AT+RSTCFG=<间隔>,<脚号>,<脉冲>,<电平> → OK   （脚号 1..6 → GPIO4..GPIO9）
查空闲模式    AT+RSTMODE?                   → +RSTMODE:HIGHZ / +RSTMODE:DRIVE
设空闲模式    AT+RSTMODE=1(高阻) / =0(推挽)   → OK
读全部电平    AT+RSTLVL  /  AT+RSTLVL?       → +RSTLVL:P1=0,P2=0,P3=0,P4=0,P5=0,P6=0
读单脚电平    AT+RSTLVL=3                   → +RSTLVL:P3=1
复位 1..6     AT+RST=<单个数字 1-6>           → OK（等 对应脉冲）
先 1 后 2     AT+RST=12  /  AT+RST=BOTH     → OK（等 p1+p2+间隔）
两路任意      AT+RST=<任意两位不重复>          → OK
三路顺序      AT+RST=123                    → OK（等 p1+p2+p3+2×间隔）
六路全部      AT+RST=ALL   (=123456)        → OK（等 Σp + 5×间隔）
自定义顺序    AT+RST=654321 等不重复排列       → OK
进下载模式    AT+DL  /  dl                  → 无响应，设备复位
```

终止符：发送用**单个 `\r`**（不要 `\r\n`，会把 `\n` 泄漏到 UART）；响应统一以 `\r\n` 结尾；两路 CDC 行为一致。
