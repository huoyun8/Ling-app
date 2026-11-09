SnapCore / SnapOCR
==================

Windows 10/11 轻量工具：单文件分发、免安装、不联网、仅简体中文。同一份源码按
Cargo feature `ocr` 出两个产物，两者可同机安装、同时运行，配置互不覆盖。

一、功能
--------
· 截图标注：框选（窗口/控件/像素吸附）、矩形/椭圆/箭头/标号/直线/文本/马赛克
  （含高斯模糊）/橡皮擦、填充与取色、撤销重做、贴屏、保存 PNG、复制到剪贴板。
· 录屏：MP4 / GIF；MP4 可录系统声音与麦克风，上限 10 分钟；GIF 上限 2 分钟。
· 文字识别（仅 SnapOCR）：框选取字，四档排版（自动合并/每框一行/合并成一段/
  原始顺序），支持查找、复制、清空与实时字数。
· 二维码：文本实时生成、框选或上传图片识别、LOGO 合成、复制与下载 PNG。
· 文件校验：拖入或选择文件，算 MD5 / SHA-256 / CRC32，支持批量、进度、停止与保存。
· 文件改名：五组规则（转换/前后缀/扩展名/序号/删除）、表格就地编辑、过滤、
  两阶段改名、整体回滚与撤销。
· 定时关机：倒计时或指定钟点（可每天续排），暂停保留进度，最后 10 秒置顶提示。
· 设置：截图/视频保存目录、开机自启、截图时隐藏、关闭到托盘、单实例、
  三行快捷键录入（互为排他）、压缩包防多开与一键关闭。

启动后默认只挂托盘，双击托盘图标显示主界面；`--auto-start` / `--enter=tray`
只挂托盘，`--enter=shot` 直接进框选，`--enter=pin` 框选后贴屏，
`--auto-quit=true` 完成后退出。主界面任意状态按 Esc 收进托盘。

二、结构
--------
  Cargo.toml          单一包，`ocr` 特性控制是否内嵌 OCR 引擎与资产
  build.rs            按 feature 注入品牌名、压缩 OCR 资产、编译图标资源
  build.ps1           双版本一键构建
  src/
    main.rs           入口、崩溃日志、单实例、托盘、主窗口与消息分派
    prelude.rs        外部导入的唯一入口
    ui.rs             颜色/尺寸/字号/DPI 常量与页面布局
    modules/
      capture.rs      截屏与 DIB 复用          selection.rs  框选遮罩与放大镜
      editor/         标注编辑器（paint / shapes / text / canvas / options）
      toolbar.rs tooltip.rs pin.rs record/ finalize.rs icons.rs
      qr.rs           二维码生成/识别/LOGO 合成
      ocr.rs          文字识别引擎（仅 ocr 特性）
      verify.rs filehash.rs   文件校验与摘要
      rename.rs rn_pinyin.rs  文件改名与拼音转换
      shutdown.rs settings.rs config.rs  定时关机、设置、配置读写
    recvideo.rs       MP4（Media Foundation）编码
    smart_erase.rs    智能消除
    ocr_layout.rs     OCR 结果排版
  assets/             logo.ico + OCR 进程 exe + 模型/字典（不带 ocr 特性时不参与构建）
  dist/               SnapCore.exe / SnapOCR.exe

三、构建
--------
在工程根目录执行：
  .\build.ps1                 两个版本都出
  .\build.ps1 -Core           只出 SnapCore
  .\build.ps1 -Ocr            只出 SnapOCR
  .\build.ps1 -Clean          清理后重建

等价的 cargo 命令：
  cargo build --release                                   → SnapCore（约 1.45MB）
  cargo build --release --features ocr --target-dir target-ocr
                                                          → SnapOCR（约 33.1MB）

在 Linux / macOS 上交叉编译 Windows 产物：
  rustup target add x86_64-pc-windows-gnu
  sudo apt install gcc-mingw-w64-x86-64   # macOS: brew install mingw-w64
  ./build.sh            # 等价 build.ps1，产物在 dist/SnapCore.exe、dist/SnapOCR.exe
  ./build.sh core       # 只出 SnapCore
  ./build.sh ocr        # 只出 SnapOCR
  ./build.sh clean      # 清理

  `.cargo/config.toml` 已配好 GNU 工具链链接器；build.rs 在找不到 rc.exe 时
  会自动改用 windres 编译图标，交叉产物同样带 exe 图标。

品牌名由 build.rs 注入，窗口类名、单实例互斥体、注册表项与 %APPDATA% 目录
均随品牌派生。OCR 资产压缩后内嵌 exe，首次识别释放到
%LOCALAPPDATA%\<品牌>_ocr 复用；把 DirectML.dll 放到该目录可自动切 GPU，
不放则走 CPU 后端。

四、已知限制与技术债
--------------------
· exe 动态链接 VC 运行库（VCRUNTIME140.dll + UCRT）。若要真正的零运行库依赖，
  可在 `.cargo/config.toml` 给 `x86_64-pc-windows-msvc` 加
  `-C target-feature=+crt-static` 后重新构建（体积略增）。
· SnapOCR 首次识别会把 ocr-process.exe 与模型释放到本地目录并启动子进程，
  属于"单文件分发、多进程运行"。
· 识别流程使用手动框选；智能选区（悬停预览 / 边缘吸附 / Tab 与滚轮循环）
  只在标注截图流程中启用。
· 主窗口锁定尺寸，不支持最大化。
· 二维码识别用 rqrr（1~4 倍放大 + Otsu 预处理）；`reference/qr_rxing.rs.txt`
  是备用解码实现，不参与编译。
· 当前源码中没有 TODO / FIXME 标注的未修复缺陷；交互层的已知回归点以
  "必须 / 不能 / 否则 / 会崩" 等注释形式留在对应代码处。
