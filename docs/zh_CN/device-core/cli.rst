设备管理命令行
========================================

:link_to_translation:`en:[English]`

独立 npm 包 ``@esp-vision/device-cli`` 安装 ``esp-vision-device`` 命令。CLI 使用公开的 ``@esp-vision/device-core`` API，支持设备发现与管理、Python 执行、设备文件、图像采集、批处理计划和限时事件监视。需要 Node.js 22 或更高版本。

安装与宿主机要求
----------------------------------------

.. code-block:: console

   npm install -g @esp-vision/device-cli
   esp-vision-device --help
   esp-vision-device inspect

串口访问需要安装带 ``pyserial`` 的 Python。Windows 默认使用 ``python``，macOS/Linux 默认使用 ``python3``；可通过 ``--python`` 选择其它解释器。固件烧录还需要在该解释器中安装 ``esptool>=5.3,<6``。局域网发现使用 CLI 内置的 multicast-DNS 客户端。

发现与检查
----------------------------------------

不指定目标时，``inspect`` 枚举系统串口和局域网公告，不打开串口或接管设备会话。USB 描述符仅用于候选分类，不代表已确认固件身份。默认显示 USB 串口候选；``--all-ports`` 会显示其它系统已枚举的串口。``--interface`` 接受逗号分隔的 ``all``、``serial``、``usb``、``usj``、``uart`` 和 ``tcp``。显式 ``--port`` 不受发现筛选限制，也支持 Linux ``/dev/serial/by-id/...`` 别名。

.. code-block:: console

   esp-vision-device inspect --json
   esp-vision-device inspect --interface serial --all-ports
   esp-vision-device inspect --interface usj,usb --probe
   esp-vision-device inspect --select 1 --fields identity,capabilities,runtime
   esp-vision-device inspect --port /dev/ttyACM0 --role usj
   esp-vision-device inspect --tcp esp-vision-board.local:3232 --fields identity,network,storage

``--probe`` 查询发现到的候选设备，``--select`` 连接本次枚举结果中选定的设备。两者都使用被动 EV-MUX 获取，不会向正在运行的应用发送 Ctrl-C。``--probe-timeout`` 每个候选设备默认 5000 毫秒，最大 60000 毫秒。发现收集默认 3000 毫秒，局域网收集上限为 10000 毫秒。指定目标时默认查询 identity、capabilities 和 runtime；``--fields`` 还接受 ``network`` 和 ``storage``。指定目标的 inspect、configure、固件检查、capture、watch 和原生 ``files write`` 使用被动连接。Python 执行、依赖 REPL 的 ``files list/read/remove/mkdir``、control、固件更新和所有批处理计划使用常规 REPL 连接探测，可能发送 Ctrl-C。显式串口目标可用 ``--role`` 指定 ``usj``、``usb``、``uart`` 或 ``unknown``，并且必须配合 ``--port``；默认角色为 ``unknown``。

Runtime 中的 ``scriptRunning`` 和 ``run`` 表示当前 SDK 会话跟踪的工作。新建的 CLI 进程无法通过这些字段判断先前启动的脱离连接程序是否仍在设备上运行。``vmReady`` 表示虚拟机已就绪，不表示虚拟机空闲。

网络配置、控制与固件
----------------------------------------

.. code-block:: console

   esp-vision-device configure --port COM3 --ssid MyWiFi
   esp-vision-device configure --port COM3 --ssid MyWiFi --password-stdin
   esp-vision-device configure --port COM3 --network-enabled false
   esp-vision-device configure --port COM3 --forget-wifi
   esp-vision-device control --tcp 192.168.1.100 --action stop
   esp-vision-device control --port COM3 --action reset --mode soft
   esp-vision-device control --port COM3 --action bootloader
   esp-vision-device firmware check --port COM3 --version 2026.10.15
   esp-vision-device firmware check --board ESP32_P4X_EYE --json
   esp-vision-device firmware update --port COM3 --version 2026.10.15

配置 Wi-Fi 时会隐藏密码提示输入；非交互环境可用 ``--password-stdin`` 从标准输入读取。没有命令行密码参数，配置结果也不会包含凭据。reset 必须指定 ``--mode soft`` 或 ``--mode hard``；收到确认不代表设备已重新连接。

固件检查默认使用官方版本源；``--catalog`` 可指定本地目录文件或 HTTPS URL。离线检查需要 ``--board``，也可用 ``--current-version`` 指定当前版本。更新必须指定本地 ``--port`` 和明确的 ``--version``；内置串口烧录器使用 ``esptool>=5.3,<6``。TCP 不是 OTA。默认 ``--mode update`` 只写应用并保留现有分区布局和用户数据。``--mode install`` 需要 ``--board``，会写入完整镜像并替换写入范围内的数据；空白设备应使用可进入 ROM 下载模式的 USJ/UART 串口。``--image`` 提供本地固件字节，但不会绕过版本完整性校验。

Python 执行
----------------------------------------

Python 输入必须且只能选择一种：内联 ``--source CODE``、本地 ``--file LOCAL.py``，或设备上的 ``--path DEVICE.py``。默认 ``--completion wait`` 会等待执行结束。正整数 ``--timeout MS`` 超时时会取消等待中的执行。``--max-output-chars`` 限制捕获的输出（默认 16,384，最大 1 MiB）。

.. code-block:: console

   esp-vision-device python --source 'print(42)' --port COM3
   esp-vision-device python --file ./main.py --port COM3 --timeout 30000
   esp-vision-device python --path /main.py --completion start --port COM3

``--completion start`` 仅接受 ``--path``，并在设备 VM 进入 runner 后确认启动。该确认不表示应用已成功初始化或执行完成。CLI 随后释放连接并退出，不会继续监视或恢复脚本。EV-MUX 将 Python 标准输出固定到启动程序的会话。需要实时 Python 输出时使用前台 ``--completion wait``；后续 watch 无法重新连接到脱离连接任务的标准输出。可通过图像采集或应用写入的进度文件检查脱离连接的程序。

前台 Python 执行和批处理中的 Python 步骤会将 ``python.output``、``python.log`` 和 ``python.state`` NDJSON 事件实时写入 stderr。输出受 ``--max-output-chars`` 限制，达到上限时发送一次截断标记。stdout 结果包含元数据，不会重复包含已发送的 Python 文本。

设备文件与图像采集
----------------------------------------

``/models`` 等路径属于设备，Windows 上也应保持原样。无效路径会在连接前被拒绝。Git Bash/MSYS 可能将斜杠开头的参数改写为 Windows 宿主路径；请使用 PowerShell，或仅为该条命令添加 ``MSYS2_ARG_CONV_EXCL="*"`` 前缀，并为本地路径提供经确认的 Windows 格式。仅给 ``/models`` 加引号不能阻止转换。不要从转换后的路径中直接删除盘符，也不要盲目重试设备操作。

.. code-block:: console

   esp-vision-device files list --path / --recursive --port COM3
   esp-vision-device files read /main.py --output ./main.py --port COM3
   esp-vision-device files write /backup.bin --file ./backup.bin --port COM3
   esp-vision-device files remove /old.bin --type file --port COM3
   esp-vision-device files mkdir /data --recursive --port COM3
   esp-vision-device capture camera --output ./frame.jpg --quality 80 --port COM3
   esp-vision-device capture screen --output ./screen.jpg --port COM3

文件读取和 JPEG 采集会将二进制数据写入指定的本地 ``--output``，并返回路径、字节数和元数据。读取和采集默认上限为 16 MiB；``--max-bytes`` 可调低上限。``files write`` 通过二进制文件 RPC 发送本地字节。JSON 中不会嵌入图像或文件内容。

批处理与 watch
----------------------------------------

``batch PLAN.json`` 会在连接前完整校验 ``schemaVersion: 1`` 计划，然后在一个连接中按顺序执行，并在首个失败操作处停止。报告包含已完成操作和失败信息。支持的操作为 ``python``、``files.list``、``files.read``、``files.write``、``files.remove``、``files.mkdir`` 和 ``capture``。Python 步骤会等待执行完成。本地文件路径以计划文件所在目录为基准；设备路径必须是绝对路径。批处理使用常规 REPL 连接探测，因为计划可能执行 Python 或依赖 REPL 的文件操作。

.. code-block:: json

   {
     "schemaVersion": 1,
     "operations": [
       { "op": "python", "source": "print('hello')" },
       { "op": "files.read", "path": "/boot.py", "output": "./boot.py" },
       { "op": "capture", "source": "screen", "output": "./screen.jpg" }
     ]
   }

``watch --duration MS`` 需要 ``--port`` 或 ``--tcp``，时长范围为 1 毫秒至 1 小时。它被动观察当前获取会话可见的输出、日志、状态变化和诊断事件，然后释放连接。watch 不会启动脚本、重新连接或重新接收先前脱离连接任务的 Python 标准输出。SIGINT/SIGTERM 会取消命令并释放所有权。

输出、超时与取消
----------------------------------------

``--json`` 会在 stdout 输出一个 ``schemaVersion: 1`` JSON 信封。状态、Python 输出、日志、固件阶段、取消、错误和耗时以 NDJSON 事件写入 stderr。二进制内容始终写入本地文件。``--timeout`` 接受 1..600000 毫秒，适用于 inspect/configure/control 和设备发现收集；``--probe-timeout`` 设置每个已发现候选设备的探测超时。显式指定 ``--timeout`` 时，也适用于前台 Python 执行、文件读取、图像采集及批处理中的这些操作。它不是连接超时：设备连接固定使用 30000 毫秒。Python ``--completion start`` 不接受 ``--timeout``；watch 使用 ``--duration``；固件操作不使用此选项，但支持信号取消。固件更新开始写入后，Core 会完成回读验证和复位处理。files list/write/remove/mkdir 支持信号取消，但没有单操作超时。SIGINT 返回码为 130，SIGTERM 为 143，参数/计划错误为 2，其它失败为 1，成功为 0。占用或结果不确定的操作不会自动重试。运行 ``esp-vision-device --help --json`` 可查看机器可读的命令和能力参考。
