SnapOCR —— C 版（C++20 + 直接调 Win32，单文件免安装）
==============================================================

解压后只有一个目录 SnapOCR\，一份源码出两个产品，只差一个编译宏：

  SnapOCR.exe   默认产物（./build.sh）。带 OCR 引擎，33MB（PP-OCRv6 引擎与模型
                已内嵌，首次识别时释放到 %LOCALAPPDATA%\SnapOCR\ocr）。
                页签：文字识别（首页）/ 二维码 / 文件校验 / 文件改名 / 定时关机 / 设置

  SnapCore.exe  SNAPCORE_WITH_OCR=0 ./build.sh。不带 OCR 引擎，1.6MB。
                页签：二维码 / 文件校验 / 文件改名 / 定时关机 / 设置（没有「文字识别」）

这份源码是完整的，两个产品都从它编出（Core 就是同一份代码关掉 OCR 宏）。
包内两个 exe 均已用本机 MSVC 14.44 编译并随包提供，解压即可运行；要自己重编
就是 ./build.sh，无需任何工程文件或额外下载。


一、直接运行
------------
双击 exe 只是挂个托盘图标（老习惯，故意不弹窗）。再双击一次 exe，已有实例
会把主界面亮出来 —— 这是原设计的启动方式，不是没反应。

默认热键：F1 截图 / F2 识别 / F10 显示或收起主界面。
两个版本默认热键相同，同时运行时后启动的那个注册不上，请在「设置」里改成
不同组合键（配置各存各的，互不影响）。

两版可以同机安装、同时运行：窗口类名（SnapCore_Main / SnapOCR_Main）、
单实例标识、配置目录（%APPDATA%\SnapCore 与 %APPDATA%\SnapOCR）、
注册表启动项、任务栏 AppUserModelID 全部按品牌分开。


二、重新编译
------------
需要 VS2022 BuildTools（MSVC 14.44）+ Windows SDK 10.0.22621。
build.sh 直接驱动 cl.exe / link.exe / rc.exe，不走 msbuild（本机安全策略会拦）。

  cd SnapOCR
  ./build.sh                       # 产物 build/bin/SnapOCR.exe（内嵌模型，约 33MB）

  SNAPCORE_WITH_OCR=0 ./build.sh   # 产物 build/bin/SnapCore.exe（纯工具版，1.6MB）

不带环境变量时按**目录名**判定：目录名以 SnapOCR 开头就走带引擎那版，把目录
改名（不以 SnapOCR 开头）即默认出纯工具版。两套中间产物分放在
build/obj-SnapCore 与 build/obj-SnapOCR，一个目录里来回切产品不会串 obj。

  ./build.sh clean                 # 全量重建（约 5~6 分钟）
  SNAPCORE_OCR_EMBED=0 ./build.sh  # 带引擎但不把模型打进 exe（开发用，
                                   # 运行时要往 exe 旁边放 ocr\ 目录）

注：脚本的调试信息开关是 /Z7（写进 obj），不依赖 PDB 服务 —— 被安全策略拦住
mspdbsrv 的机器上也不会再报 C1090。


三、这一版的变化（v4：修 bug + 代码精简）
---------------
功能口径不变，只修 bug、删死代码。全部改动均与上一版逐处 1:1 行为比对，
每文件 /W4 单独编译零告警（SnapOCR、SnapCore 两侧），最终全量构建通过 ——
包内两个 exe 就是本次构建的产物。

修掉的主要 bug：

  重做顺序反向 —— 画 1~5 再逐步撤销（按 5→4→3→2→1 收，正确），重做却按
  5→4→3→2→1 恢复（与撤销顺序同向），现改为按 1→2→3→4→5 镜像恢复；
  带参数启动残留僵尸进程 —— --auto-quit / --enter 下主窗创建失败被无视，
  留下看不见的后台进程，现失败即退出；
  截图取像时序 —— GetDIBits 在位图仍选入内存 DC 时调用，违反 MSDN 前提，
  部分驱动下会取到乱行像素，已按要求先解除选中再取；
  OCR 线程 —— 迟到的识别完成消息会去 join 新一轮线程并把旧结果填进新页面；
  识别线程启动失败无任何提示；空结果框在控件 0×0 时会挂上收不掉的滚动条；
  悬空指针 —— 删除标注时宿主上的悬停 / 编辑 / 拖拽指针未全部断开；
  录屏选项不落盘 —— MP4/GIF、系统声、麦克风的选择此前每次截图都打回默认，
  现与其它工具一样写入配置。

代码精简：自写代码 31,525 → 31,137 行（净 -388）。删掉的约 900 行全是永不执行
的死分支（贴图窗里到不了的编辑器路径、失效的清理链、只写不读的状态等）与
重复实现；为修 bug 新增的约 500 行都在关键路径上。

版本号按品牌分开：SnapCore 是 1.0（纯工具版，图标换成 SnapCore.ico），
SnapOCR 是 2.0（带识别引擎，仍用原 logo.ico）。

四、目录说明
------------
build.sh                构建脚本（cl / link / rc 直驱）
README.txt              本文件
ReadMe.md               完整功能说明（每一页每个按钮）
版本规范.md             命名规则、版本叠加、改名边界、版本记录
交接单.md               开发交接记录（构建口径 / 实现要点 / 限制 / 缺陷结论）
LICENSE
Src/                    全部源码
  Brand.h               品牌宏（一处定义全程序生效）
  App / Setting / Tray / Tip / Lang / History / Util / main
  Win/                  WinQr（主界面）、WinQrVerify、VfBox、WinCap、CutMask、
                        WinPin、WinHost、CapVideo、cgif、WinSettingShortcut、ZipKill、
                        WinRename（文件改名页）、RnList（改名页的自绘列表，
                        RnPinyinData.h 是它的拼音数据）
  Hash/                 MD5 / SHA256 / CRC32 / 版本号 / 修改时间
  Ocr/                  OcrEngine（与子进程通信）、OcrLayout（排版四档）
                        —— SnapCore 版不编译这两个
  Qr/                   quirc 解码 + libqrencode 编码
  Shape/ Tool/ Ui/      标注元素、工具条、自绘 UI 框架
  Res/                  Resource.rc（图标 / 语言包 / 版本信息 / OCR 资源）
  Res/ocr/              引擎本体 snow-ocr-process.exe + PP-OCRv6 det/rec 模型 + 字典
                        —— 只在 SnapOCR 目录里
Lang/zh-CN.json         编进 exe 的中文语言包
tools/                  ocrcmp（模型 LZMS 压缩打包）、layout_test（排版离线自测）
