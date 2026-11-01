#include "pch.h"
#include <Ui/Ui.h>
#include "Brand.h"
#include "Setting.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinQr.h"
#include "App.h"

namespace {
    std::unique_ptr<Setting> setting;
    constexpr int capShortcutMsgId{ 100 };
    constexpr int qrShortcutMsgId{ 101 };
    constexpr int hideShortcutMsgId{ 102 };
    constexpr int zipKillShortcutMsgId{ 103 };
    // 四组快捷键共用一张"组合池"：同一个组合只能挂在一个功能上
    const std::vector<std::wstring>& allShortcutTypes()
    {
        static const std::vector<std::wstring> types{ L"cap", L"qrcap", L"hide", L"zipkill" };
        return types;
    }
    int msgIdOfType(const std::wstring& type)
    {
        if (type == L"qrcap") return qrShortcutMsgId;
        if (type == L"hide") return hideShortcutMsgId;
        if (type == L"zipkill") return zipKillShortcutMsgId;
        return capShortcutMsgId;
    }
    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart / initShortcutKeys）。
    // 默认组合故意选 F1 / F2 / F10 这类单键：截图、识码、隐藏都是高频动作，
    // 按一次就到位比"三根手指摆好"顺得多，而 F 键本来就不承担输入职能，占着不碍事。
    // zipkill 默认留空：一键关掉所有压缩软件是破坏性动作，得由用户自己在设置里录
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":true,"language":"zh-CN"},"shortcutKey":{"cap":"F1","qrcap":"F2","hide":"F10","zipkill":""}})""" };
    // 启动项所在的 Run 键：setAutoStart 写、autoStartInRegistry 查，两边共用一个字面量
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // 注册表 Run 里现在有没有本程序的启动项（值名 = 品牌名，与 setAutoStart 相同）
    bool autoStartInRegistry()
    {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return false;
        const LONG r = RegQueryValueEx(hKey, SNAP_BRAND_W, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(hKey);
        return r == ERROR_SUCCESS;
    }
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        auto content = ui::Util::readFileText(configPath);
        if (content.empty() || content.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            configObj = JsonObject::Parse(defaultConfig);
            save();
            return;
        }
        JsonObject obj{ nullptr };
        if (JsonObject::TryParse(content, obj)) {
            configObj = obj;
            return;
        }
        MessageBox(nullptr, L"config.json parse error，use default config", SNAP_BRAND_W, MB_OK | MB_ICONWARNING);
    }
    configObj = JsonObject::Parse(defaultConfig);
    // 文件不存在（或解析失败）时也要落盘：否则首次运行永远不生成 config.json，
    // 用户想改语言/热键时都找不到文件（空文件那条分支早就这么做了，这里漏了）
    save();
}

void Setting::init()
{
    setting.reset(new Setting());
    // 「开机自启」默认开：勾选状态按配置画（getAutoStart），注册表启动项在启动时
    // 对齐到配置 —— 配置 true 而注册表没有的（新装 / 老配置缺键 / 被清理工具删过）
    // 补上，配置 false 而注册表还挂着的清掉。注册表写不进去时 setAutoStart
    // 不落配置，设置页的勾就照实画不出来，不会出现假勾
    if (autoStartInRegistry() != setting->getAutoStart())
        setting->setAutoStart(setting->getAutoStart());
}

void Setting::dispose()
{
    setting.reset();
}

Setting* Setting::get()
{
    return setting.get();
}

std::filesystem::path Setting::getDataPath()
{
    return dataPath; //复制一份路径对象，不允许就地修改
}

void Setting::setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys)
{
    // keys 为空 = 用户主动把这项热键清空（设置页按退格），要照常写空串并注销注册，
    // 不能当误调用忽略掉，否则下次启动又会把默认值填回来
    std::wstring str;
    for (size_t i = 0; i < keys.size(); i++)
    {
        if (!str.empty()) str += L"+";
        str += keys[i];
    }
    auto shortcutKey = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!shortcutKey) {
        // 老配置文件里没有这一层时现建一层，别让规范默认配置缺的键变成一次异常
        shortcutKey = JsonObject();
        configObj.SetNamedValue(L"shortcutKey", shortcutKey);
    }
    auto app = ui::App::get();
    // 排他：同一个组合只能挂在一个功能上。新设的组合若已被别的功能占用，
    // 直接把那一边清空（"顶替"），而不是让两边都去抢同一个热键 ——
    // RegisterHotKey 对同一个组合只有第一个能成功，先注册的那个赢，
    // 表现为"设置页写着两个功能都用这个键，实际只有一个响"，比直接顶掉更难理解
    for (const auto& other : allShortcutTypes()) {
        if (other == type) continue;
        // 清空不是"占了一个空组合"，不需要去顶替谁
        if (str.empty()) break;
        const std::wstring cur{ shortcutKey.GetNamedString(other, L"") };
        if (cur.empty()) continue;
        if (_wcsicmp(cur.c_str(), str.c_str()) != 0) continue;
        shortcutKey.SetNamedValue(other, JsonValue::CreateStringValue(L""));
        // 被顶掉的功能立刻注销，否则旧的注册还在，热键会继续触发它
        app->unRegHotKey(msgIdOfType(other));
    }
    shortcutKey.SetNamedValue(type, JsonValue::CreateStringValue(str));
    // 先把这个 type 原来占着的那组注销掉 —— RegisterHotKey 同一 id 重复注册不会失败，
    // 结果是新旧两组都能触发这一个回调
    app->unRegHotKey(msgIdOfType(type));
    // 空串不再注册：设置页把这项清空后就是"没有热键"，重启也不该冒出来
    if (hotkeysOn && !str.empty()) app->regHotKey(str, msgIdOfType(type));
    save();
}

bool Setting::setHotkeysEnabled(bool enabled)
{
    hotkeysOn = enabled;
    auto app = ui::App::get();
    bool allOk = true;
    for (auto& type : allShortcutTypes()) {
        const int id = msgIdOfType(type);
        app->unRegHotKey(id);
        if (!enabled) continue;
        auto s = getShortcutKey(type);
        if (s.empty()) continue;
        // 注册失败的常见原因是被别的程序先占了（ERROR_HOTKEY_ALREADY_REGISTERED）。
        // 记下来交给调用方提示，否则界面上写着"已保存"，按下去却毫无反应
        if (!app->regHotKey(s, id)) allOk = false;
    }
    return allOk;
}

bool Setting::hotkeysEnabled() const
{
    return hotkeysOn;
}


std::wstring Setting::getShortcutKey(const std::wstring& type)
{
    // 一路用带默认值的重载：启动时 ensureDefaults 已经补齐过，这里只是别让运行期
    // 意外（配置被外部改动、问了个没配过的 type）变成一次崩溃
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!obj) return L"";
    return std::wstring{ obj.GetNamedString(type, L"") };
}

bool Setting::setAutoStart(bool autoStart)
{
    // 启动项名 = 任务管理器「启动应用」里显示的名字，跟着品牌走。
    // 旧版本写过 "Xin OCR"（更早还写过 "SnapCore"）；升级时必须把它们清掉，
    // 否则同一条命令会在启动项里挂两三条，用户看到的就是"开机启动好几个同样的东西"。
    //   SnapCore 版：只剩 "Xin OCR" 要清；
    //   SnapOCR 版：还得清 "SnapCore" —— 带引擎的旧版当年用的就是这个名字。
    static constexpr wchar_t kRunValue[] = SNAP_BRAND_W;
#if SNAP_HAS_OCR
    static constexpr const wchar_t* kRunValueLegacy[] = { L"Xin OCR", L"SnapCore" };
#else
    static constexpr const wchar_t* kRunValueLegacy[] = { L"Xin OCR" };
#endif
    // writable 记的是"注册表到底改成了没"：RegOpenKeyEx 打不开（组策略）、
    // RegSetValueEx 被安全软件拦下，都算没改成，此时不能把配置也写成新值
    bool writable = false;
    if (autoStart) {
        // 路径可能超过 MAX_PATH（长路径策略打开后很常见）。GetModuleFileName 截断时
        // 返回值等于缓冲容量，这里按需翻倍重取，避免写进注册表的是被截断的路径
        std::wstring exePath(MAX_PATH, L'\0');
        for (;;) {
            const DWORD len = GetModuleFileName(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
            if (len == 0) {
                exePath.clear();
                break;
            }
            if (len < exePath.size()) {
                exePath.resize(len);
                break;
            }
            exePath.resize(exePath.size() * 2);
        }
        if (exePath.empty()) return false;
        auto curPath = std::filesystem::path(exePath);
        std::wstring commandLine = std::format(L"\"{}\" --auto-start", curPath.wstring());
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            // cbData 是 DWORD：字符串长度一定小于 4GB，但得显式窄化，不然 /W4 直接报
            const DWORD bytes = static_cast<DWORD>((commandLine.size() + 1) * sizeof(wchar_t));
            writable = RegSetValueEx(hKey, kRunValue, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(commandLine.c_str()), bytes) == ERROR_SUCCESS;
            for (const wchar_t* legacy : kRunValueLegacy) RegDeleteValue(hKey, legacy);
            RegCloseKey(hKey);
        }
    }
    else {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            // 值本来就不存在（返回 ERROR_FILE_NOT_FOUND）等于"已经是关闭状态"，也算成功
            const LONG r = RegDeleteValue(hKey, kRunValue);
            writable = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
            for (const wchar_t* legacy : kRunValueLegacy) RegDeleteValue(hKey, legacy);
            RegCloseKey(hKey);
        }
    }
    // 注册表没改成就别落配置：设置页按 getAutoStart 画勾，配置写 true 会画出一个
    // 假勾 —— 用户以为开机自启已经生效，实际下次开机什么都没有
    if (!writable) return false;
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
    save();
    return true;
}

bool Setting::getAutoStart()
{
    // 默认开（见 init 里的注册表对齐）：勾是按配置画的，注册表由启动时那次对齐兜底
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"autoStart", true);
}

void Setting::setZipGuard(bool on)
{
    setCommonBool(L"zipGuard", on);
}

bool Setting::getZipGuard()
{
    // 默认开：这个功能不开就等于没有，用户要正常多开同一个包时自己关
    return getCommonBool(L"zipGuard", true);
}

void Setting::setHideMainOnCap(bool on)
{
    setCommonBool(L"hideMainOnCap", on);
}

bool Setting::getHideMainOnCap()
{
    // 默认关：主界面藏在后面不碍事，想截到工具窗内容（比如对照着识码）的用户省一步
    return getCommonBool(L"hideMainOnCap", false);
}

void Setting::setCloseToTray(bool on)
{
    setCommonBool(L"closeToTray", on);
}

bool Setting::getCloseToTray()
{
    // 默认开：历来"点关闭只是收进托盘、进程不退出"，定时关机倒计时要靠这个
    return getCommonBool(L"closeToTray", true);
}

void Setting::setSingleInstance(bool on)
{
    setCommonBool(L"singleInstance", on);
}

bool Setting::getSingleInstance()
{
    // 默认开：历来就是"双击图标必定只出一个窗口"，这条不能被升级悄悄改掉
    return getCommonBool(L"singleInstance", true);
}

void Setting::setShowMainOnStart(bool on)
{
    setCommonBool(L"showMainOnStart", on);
}

bool Setting::getShowMainOnStart()
{
    // 默认开：双击就该直接看到界面；想"先落托盘、再双击才出界面"的用户自己关掉
    return getCommonBool(L"showMainOnStart", true);
}

std::wstring Setting::getCommonString(const wchar_t* key)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    // 一路用带默认值的重载：老配置里没有这个键是常态，不该抛异常
    if (!common) return L"";
    return std::wstring{ common.GetNamedString(key, L"") };
}

void Setting::setCommonString(const wchar_t* key, const std::wstring& val)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(key, JsonValue::CreateStringValue(val));
    save();
}

bool Setting::getCommonBool(const wchar_t* key, bool def)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    // 与历史行为逐条一致：common 层不存在时一律 false，不走 def
    if (!common) return false;
    return common.GetNamedBoolean(key, def);
}

void Setting::setCommonBool(const wchar_t* key, bool val)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

std::wstring Setting::getShotDir()
{
    return getCommonString(L"shotDir");
}

void Setting::setShotDir(const std::wstring& dir)
{
    setCommonString(L"shotDir", dir);
}

std::wstring Setting::getVideoDir()
{
    return getCommonString(L"videoDir");
}

void Setting::setVideoDir(const std::wstring& dir)
{
    setCommonString(L"videoDir", dir);
}

std::wstring Setting::getOcrLayout()
{
    return getCommonString(L"ocrLayout");
}

void Setting::setOcrLayout(const std::wstring& mode)
{
    setCommonString(L"ocrLayout", mode);
}

std::filesystem::path Setting::initDataPath()
{
    PWSTR pathTmp;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr)) {
        _ASSERT_EXPR(FALSE, L"get roaming path，error");
        return L"";
    }
    // 局部名别叫 dataPath：类里有同名成员，遮蔽之后很容易写错赋值目标
    auto dir = std::filesystem::path{ pathTmp };
    CoTaskMemFree(pathTmp);
    // 配置落 %APPDATA%\<品牌>：SnapCore 与 SnapOCR 各用各的目录，
    // 两个版本同时装时互不覆盖配置（热键、保存位置、OCR 排版档位）
    dir.append(SNAP_DIR_W);
    if (!std::filesystem::exists(dir)) {
        if (!std::filesystem::create_directories(dir)) {
            _ASSERT_EXPR(FALSE, L"create data path，error");
        }
    }
    return dir;
}

std::filesystem::path Setting::initConfigPath()
{
    // 与插件的查找顺序一致（见 Util.cpp 里的 findImageReader）：先看 exe 同目录。
    // 只有那份文件本来就存在时才认它 —— 不存在就不要在程序目录里新建，
    // 装在 Program Files 下时那儿通常没有写权限，况且默认位置该是 appdata
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    auto path = std::filesystem::path{ buffer }.parent_path().append(L"config.json");
    if (std::filesystem::exists(path)) return path;
    auto fallback = this->dataPath; //复制一份路径对象，append 会就地改
    return fallback.append(L"config.json");
}

void Setting::save()
{
    std::wstring str{ configObj.Stringify() };
    ui::Util::saveFile(configPath.wstring(), str);
}

JsonObject Setting::getToolObj(const std::wstring& tool)
{
    // 用带默认值的重载：这两层在旧配置文件里都不存在，直接 GetNamedObject 会抛异常，
    // 值被手工改成非对象时它也一样返回默认值，不会炸
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) {
        root = JsonObject();
        configObj.SetNamedValue(L"toolPin", root);
    }
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    return getToolObj(tool).GetNamedBoolean(key, def);
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    return static_cast<float>(getToolObj(tool).GetNamedNumber(key, def));
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

void Setting::initShortcutKeys()
{
    auto ensureDefault = [this](const std::wstring& type, const std::wstring& defVal) -> std::wstring {
        auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
        if (!obj) {
            obj = JsonObject();
            configObj.SetNamedValue(L"shortcutKey", obj);
        }
        // 只有"键不存在"才补默认值：显式存了空串表示用户主动清空了这项，
        // 每次启动再填回来就等于清空没生效
        if (!obj.HasKey(type)) {
            // 老配置里没有这一项：把默认值写回去，设置界面才看得到现在挂的是哪个组合
            obj.SetNamedValue(type, JsonValue::CreateStringValue(defVal));
            save();
            return defVal;
        }
        return std::wstring{ obj.GetNamedString(type, L"") };
        };

    // 默认组合换过一次（截图 Ctrl+Alt+A → F1，识码 Ctrl+Alt+Q → F2）。老配置里存着旧默认值
    // 的时候必须跟着迁到新默认，否则升级完按 F1 毫无反应，看起来就是"默认值改了但没生效"。
    // 只迁"完全等于旧默认值"的那一条：用户自己设过的组合一律不动 —— 那是他的选择，
    // 不因为他没改过硬盘上的文件就被悄悄换掉
    auto migrateOldDefault = [this](const std::wstring& type, const std::wstring& oldVal, const std::wstring& newVal) {
        auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
        if (!obj) return;
        auto cur = std::wstring{ obj.GetNamedString(type, L"") };
        if (_wcsicmp(cur.c_str(), oldVal.c_str()) != 0) return;
        obj.SetNamedValue(type, JsonValue::CreateStringValue(newVal));
        save();
        };
    migrateOldDefault(L"cap", L"Ctrl+Alt+A", L"F1");
    migrateOldDefault(L"qrcap", L"Ctrl+Alt+Q", L"F2");

    auto lingApp = ui::App::get();
    if (hotkeysOn) {
        // 取不到就用默认组合：热键注册不上顶多是快捷键不好用，不该让程序起不来
        lingApp->regHotKey(ensureDefault(L"cap", L"F1"), capShortcutMsgId);
        lingApp->regHotKey(ensureDefault(L"qrcap", L"F2"), qrShortcutMsgId);
        lingApp->regHotKey(ensureDefault(L"hide", L"F10"), hideShortcutMsgId);
        // zipkill 没有默认值，也不走 ensureDefault —— 那会把空串当"缺键"每次启动写一次盘。
        // 没录就是没录，注册空组合 regHotKey 会直接返回 false，不装就是了
        const auto zipKill = getShortcutKey(L"zipkill");
        if (!zipKill.empty()) lingApp->regHotKey(zipKill, zipKillShortcutMsgId);
    }

    lingApp->onHotKey.add([this](UINT msg) {
        if (msg == capShortcutMsgId) {
            // 截图前先把主界面收起来（设置页「截图时隐藏主界面」，默认开）：
            // 自己的窗口压在选区里，截出来的图里就带着它。恢复由 WinCap 关窗时统一做
            WinQr::hideForCapture();
            // 建窗失败（上一次还没收干净、D2D 不可用）时没有 onClosed 来收尾，
            // 必须在这里放回去，否则主界面就一直不见了。
            // 放回去还不够 —— 用户看到的是"闪一下、什么都没发生"，还得告诉他为什么
            if (!WinCap::init()) {
                WinQr::restoreAfterCapture();
                WinQr::showCapFailTip();
            }
        }
        else if (msg == qrShortcutMsgId) {
            // 扫码：拉起主界面后直接进截图识别，解出来回填输入框。
            // 隐藏 / 恢复由 startScreenDecode 自己做 —— 它要在抓完像素后先把窗口摆出来再填结果
            WinQr::decodeFromScreen();
        }
        else if (msg == hideShortcutMsgId) {
            // 隐藏 / 显示主界面：同一个键来回切。不截图、不识别，只是把窗口收起来或亮出来，
            // 平时挂着二维码工具不想让它占着桌面时用
            WinQr::toggleMainVisible();
        }
        else if (msg == zipKillShortcutMsgId) {
            // 压缩软件全关：把正在运行的压缩软件进程一次性结束掉（原来只是二维码页的一个按钮，
            // 现在挪到这里做成全局热键，人不在主界面上也能按）。实现见 WinQr::killZipByHotkey
            WinQr::killZipByHotkey();
        }
    });
    // 【不要再在这里挂 onSecondInstance】第二次启动的应答权已经统一在 App 构造里
    // （WinQr::toggleMainVisible：亮 → 收 → 亮，一下一下地切）。这里原先也挂了一个
    // WinQr::init()，它必然会显示窗口，于是每次双击都变成"toggle 刚收起来、又被这里
    // 弹回去"，表现就是只会显示、永远不会隐藏
}
