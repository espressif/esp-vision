设备管理命令行
========================================

:link_to_translation:`en:[English]`

npm 包安装 ``esp-vision-device`` 管理工具，共有 inspect、configure、control 和 firmware（check/update）四个命令，调用与 IDE 相同的公开 createDevice API。不再单独设置 discover/info/status/capabilities，也不扩展成 Python 终端或文件管理器。

安装与系统支持
----------------------------------------

.. code-block:: console

   npm install -g @esp-vision/device-core@0.1.0
   esp-vision-device --help
   esp-vision-device inspect

安装命令在首次 npm 发布后可用，使用 Node.js 22。Windows、Linux、macOS 使用同一命令解析和 Python/pyserial 系统端口枚举。Windows 默认 python，其它系统默认 python3，可通过 --python 选择安装了 pyserial 的解释器，不依赖 Linux 专有设备路径扫描。局域网发现使用可选 multicast-dns，正常 npm 安装会带上，省略 optional dependencies 时需自行安装；缺少发现依赖时警告，其它成功来源仍可使用。

.. code-block:: console

   python -m pip install pyserial
   esp-vision-device inspect --python python

枚举与查询
----------------------------------------

不指定目标时，inspect 只枚举系统串口和 DNS-SD 公告，不打开串口、不接管设备会话。有线设备的开发板/固件版本保持未知，不能把 USB 产品字符串当作已确认板名。USB VID/PID、产品和序列号仅用于候选分类：保留 Espressif USJ、ESP-VISION USB-OTG 候选、USB 转 UART；普通 UART 通过 --all-ports 显示。ROM/未烧录候选不会被丢弃，无响应也不能断言未烧录。没有暴露系统串口的 USB 设备无法被当前串口后端枚举；无串口端点的 USB/ROM 模式需要另外的宿主 USB 适配器。

筛选先依据系统枚举，再检查各平台名称和 USB 描述符；不通过拼接文件名猜测设备，也不设置仅允许 Espressif VID/PID 的白名单。Windows 接受 COM1 及更高编号（包括 COM10+ 和扩展设备路径形式）；Linux 的 USB 串口通常为 /dev/ttyACM*、/dev/ttyUSB* 或 /dev/ttyXRUSB*；macOS 使用 /dev/cu.*、/dev/tty.*，同一端口两者都存在时优先 cu.*。USB 描述符也可保留自定义驱动名称。开发板身份或固件未知不会导致 USB 候选被过滤。

默认只显示 USB 串口候选。--all-ports 额外显示系统枚举的原生 UART、蓝牙和其它串口：Linux 的 ttyS*、ttyAMA*、ttyTHS*、rfcomm* 都是合法端口，不是非法名称。Windows 仅从 COM 名称无法区分 USB、原生或虚拟串口；缺少 USB 描述符时需 --all-ports 才显示。macOS 蓝牙/调试串口也需 --all-ports。显式 --port 绕过发现筛选，也支持 Linux 的 /dev/serial/by-id/... 别名；两种枚举模式都不会通过打开串口来验证存在性。

规则依据 `pySerial 枚举说明 <https://pyserial.readthedocs.io/en/latest/tools.html#serial.tools.list_ports.comports>`_、`Microsoft 串口句柄说明 <https://learn.microsoft.com/en-us/windows/win32/devio/communications-resource-handles>`_、`Linux 串口设备命名 <https://www.kernel.org/doc/html/latest/admin-guide/devices.html>`_ 和 `Apple 串口设备访问说明 <https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/WorkingWSerial/WWSerial_SerialDevs/SerialDevices.html>`_。命令行 inspect 与 JavaScript 查询方法 device.inspect() 保持一致。

.. code-block:: console

   esp-vision-device inspect --json
   esp-vision-device inspect --interface serial
   esp-vision-device inspect --interface serial --all-ports
   esp-vision-device inspect --interface usj,usb --probe
   esp-vision-device inspect --interface tcp
   esp-vision-device inspect --select 1 --fields identity,capabilities

结果显示本次编号。已主动识别设备排在局域网公告、未探测串口和探测失败端口前面；确认同一 deviceId 后分配相同分组号，保留各接口地址。未识别、占用或无权限端口保留并显示错误码，探测失败不证明固件缺失。USB 序列号与固件 deviceId 分别显示。--select 使用该次调用的同一枚举快照，随后连接所选编号；脚本应使用明确地址。局域网公告也不保证设备此刻可连接。

--probe 显式建立会话，可能打断 Python。每个端点默认探测 5000 毫秒，通过 --probe-timeout 修改；--interface 支持 all/serial/usb/usj/uart/tcp，逗号分隔，serial 包括通过枚举策略筛选的全部有线端口。默认局域网收集 3000 毫秒，上限 10000 毫秒。不同系统的端口路径直接使用，不擅自改写。

.. code-block:: console

   esp-vision-device inspect --port COM3
   esp-vision-device inspect --port /dev/ttyACM0 --role usj
   esp-vision-device inspect --port /dev/cu.usbmodem1101 --role usb
   esp-vision-device inspect --tcp 192.168.1.100:3232
   esp-vision-device inspect --tcp esp-vision-board.local:3232 --fields identity,capabilities,runtime,network,storage --json

指定目标时统一执行连接、查询、释放。默认字段为 identity/capabilities/runtime，需要时加 network/storage；无目标的 --fields 必须配合 --probe 或 --select。明确串口地址时 --role 可省略，默认 unknown；从枚举结果选择时使用描述符候选角色，最终身份仍以固件协商为准。TCP 主机名依赖系统 DNS/mDNS 解析，无法解析 .local 时用发现的 IP。TCP 支持方括号 IPv6 地址，当前 DNS-SD 公告提供 IPv4 端点。不能将这些查询描述成无干扰协议探测。


命令行与 SDK 命名对应
----------------------------------------

命令名与 SDK 方法统一为 inspect、configure、control、firmware check/update。命令行选项使用连字符，JavaScript 使用驼峰或嵌套对象；这是语法转换，不是另一套功能。下表为突出对应关系省略连接选择与清理。

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - 命令行
     - SDK
   * - ``inspect --fields identity,network``
     - ``device.inspect({ fields: ["identity", "network"] })``
   * - ``configure --network-enabled true``
     - ``device.configure({ network: { enabled: true } })``
   * - ``configure --ssid NAME``
     - ``device.configure({ network: { credentials: { ssid, password } } })``
   * - ``configure --forget-wifi``
     - ``device.configure({ network: { credentials: null } })``
   * - ``control --action reset --mode soft``
     - ``device.control({ action: "reset", mode: "soft" })``
   * - ``firmware check --version VERSION``
     - ``device.firmware.check({ version })``
   * - ``firmware update --version VERSION``
     - ``device.firmware.check({ version })`` → ``device.firmware.update({ release, openLoader })``

CLI inspect 未指定目标时执行主机侧设备枚举，组合系统串口与局域网发现，不会对未连接设备调用 device.inspect()。SDK 的 device.discover() 通过发现适配器列出局域网公告，device.inspect() 查询已建立的会话。--port/--tcp 选择宿主传输；--probe/--select/--all-ports/--json 属于 CLI 的发现或展示参数，不是额外的 SDK API。--ssid 通过提示/stdin 获取密码；配置结果有意只输出 ok，避免泄露凭据。--image 将本地字节映射为 firmware.update 的 data，--version 先通过 firmware.check 选择 release。

CLI 显式目标的 --timeout 默认为 30000 毫秒，传给 connect/inspect/configure/control 时覆盖 SDK 方法默认值，不是整条命令的总时限。固件检查/更新的下载与烧写使用取消信号，不受此 timeout 控制。直接调用 SDK 且省略 timeout 时，配置默认 45000 毫秒，控制默认 15000 毫秒。

配置与控制
----------------------------------------

.. code-block:: console

   esp-vision-device configure --port COM3 --ssid MyWiFi
   esp-vision-device configure --port /dev/ttyACM0 --ssid MyWiFi --password-stdin
   esp-vision-device configure --port /dev/ttyACM0 --network-enabled false
   esp-vision-device configure --port /dev/ttyACM0 --forget-wifi
   esp-vision-device control --tcp 192.168.1.100 --action stop
   esp-vision-device control --port COM3 --action reset --mode soft
   esp-vision-device control --port COM3 --action reset --mode hard
   esp-vision-device control --port COM3 --action bootloader

配置 SSID 时交互隐藏输入密码；--password-stdin 从标准输入读取，去掉末尾一个换行，不接受命令行密码参数。配置结果仅输出 ok:true，不回显凭据；空密码代表开放网络。Wi-Fi 配置可能断开 TCP，建议有线配网。reset 必须显式给出 soft/hard，复位/下载 ACK 不代表设备已重新上线。枚举不会复位或写入设备。

固件管理
----------------------------------------

.. code-block:: console

   esp-vision-device firmware check --port COM3
   esp-vision-device firmware check --port COM3 --version 2026.10.15
   esp-vision-device firmware check --board ESP32_P4X_EYE --json
   esp-vision-device firmware update --port COM3 --version 2026.10.15

check 默认读取 https://dl.espressif.com/AE/esp-vision/config.toml，再获取 firmware/<board>/<version>.json 中的版本更新信息，无需用户准备 JSON。列出全部目录版本；旧版本缺少合格更新元数据时仍显示 compatible:false、reason UPDATE_METADATA_UNAVAILABLE，不能直接通过应用分区更新接口写入。--version 精确选择合格版本。--catalog 仅作高级自定义源覆盖，不是必填项。离线检查只需 --board，--current-version 可省略，默认 unknown。

update 需要本地 --port 和明确 --version。内置串口烧写器在内部调用独立安装的 Python esptool>=5.3,<6，一次安装依赖即可：python -m pip install "esptool>=5.3,<6"。不再接受 --loader，也不要求用户编写 JS。--python 只是选择解释器，不是配置适配器。显式 --board 可用于不接管 v4 会话的恢复操作，但仍校验真实芯片、安全状态和分区。TCP 不等于 OTA。镜像下载、仅写 application、完整回读和复位处理均复用 Core 更新 API；--image 仅作为高级本地镜像覆盖。

固件发布流水线根据实际构建产物自动生成 application bin、SHA-256、真实分区大小/表哈希及镜像头中的芯片修订范围，随原有整片镜像增量发布，不改变官网原有烧写方式。本次实现时 dl 上六个历史版本缺少这些元数据，因此可列举但没有可直接安全更新的官方版本；新固件发布后自动生效，不猜测兼容性或模拟更新成功。

输出与取消
----------------------------------------

--json 向 stdout 输出一个结果对象，默认枚举按 Unicode 显示宽度补空格对齐，窄终端使用带编号的设备信息块，宽度足够时显示列对齐表格，其它操作输出格式化 JSON。部分枚举失败通过 stderr 警告，JSON 模式放在 warnings 数组；全部来源失败则命令失败，成功但空的来源允许返回空列表。错误向 stderr 输出有限的 code/message 对象并以非零状态退出，不输出设备原始 traceback 或配置数据。Ctrl-C/SIGTERM 取消发现和 Core 操作并清理资源；开始写固件后遵循 Core 策略完成校验。表格转义设备数据中的终端控制字符。当前真机覆盖 Linux，Windows/macOS 的参数/描述符行为有自动化测试，不代表已完成两系统真机验收。
