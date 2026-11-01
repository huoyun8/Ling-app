#include "UiPch.h"
#include "D2D.h"
#include "UiUtil.h"
#include "SvgPath.h"
#include <wincodec.h>
namespace ui {
	static std::unique_ptr<D2D> d2d;
	std::vector<std::wstring> D2D::fontNames;
	unsigned D2D::gen{ 0 };

	D2D::D2D()
	{
		++gen;
		initDevice();
		initFont();
		loadFonts();
	}
    D2D::~D2D()
    {
		// 几何与描边样式都是工厂产物。工厂跟着单例一起没了，缓存里那些对象要么成了
		// 另一代工厂的产物、要么已经失效 —— 后者在 DrawGeometry 里静默失败，
		// 而图标全是描边路径，结果就是"图标整片消失"。所以跟着设备一起清干净
		SvgPath::clear();
    }
    unsigned D2D::generation()
    {
        return gen;
    }
    D2D* D2D::get()
    {
        if (!d2d) d2d.reset(new D2D());
        return d2d.get();
    }

	void D2D::dispose()
	{
		d2d.reset();
	}

	// 三个核心对象都在才叫"能用"：少了任何一个，后面 CreateBitmap / CreateDeviceContext /
	// CreateSwapChain 之类的调用都会直接吃空指针
	bool D2D::ready()
	{
		auto p = get();
		return p && p->d3dDevice && p->d2dFactory && p->d2dDevice && p->deviceContext;
	}

	void D2D::initDevice()
	{
		D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1 };
		auto count = std::size(levels);
		auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, count, D3D11_SDK_VERSION, d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
		if (FAILED(hr) || !d3dDevice) {
			// 硬件设备建不出来：远程桌面会话里没显卡加速、显卡驱动刚重置、纯虚拟机、
			// 或者被显卡厂商的驱动黑名单挡住 —— 以前这里只 assert 一下就 return，
			// 于是 d2dFactory / d2dDevice / deviceContext 全是空的，
			// 之后任何一次 CreateSolidColorBrush / CreateBitmap 都是空指针崩溃，
			// 而且崩在 UI 线程的绘制路径上，表现为"一按热键程序就没了"。
			// 退到 WARP（微软的软件光栅化器）至少能把界面画出来，慢一点但能用
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, count, D3D11_SDK_VERSION, d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
		}
		if (FAILED(hr) || !d3dDevice) {
			_ASSERT_EXPR(FALSE, L"D3D11CreateDevice，error");
			return;
		}
		D2D1_FACTORY_OPTIONS options{};
		hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory1, &options, (void**)&d2dFactory);
		if (FAILED(hr) || !d2dFactory) {
			_ASSERT_EXPR(FALSE, L"D2D1CreateFactory，error");
			d3dDevice.Reset();
			return;
		}
		ComPtr<IDXGIDevice> dxgiDevice;
		d3dDevice->QueryInterface(dxgiDevice.ReleaseAndGetAddressOf());
		// 队列里只排一帧：默认允许 3 帧在飞，鼠标一路动下来画面就落后两三帧。
		// 截图选区、工具条悬停这类"必须贴着光标走"的场景，帧深越浅越跟手；
		// 代价是设备忙时会丢帧，而丢一帧比慢一帧更容易接受
		if (dxgiDevice) {
			ComPtr<IDXGIDevice1> dxgiDevice1;
			if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1)) && dxgiDevice1) {
				dxgiDevice1->SetMaximumFrameLatency(1);
			}
		}
		// 顺着 device -> adapter -> factory 拿到 IDXGIFactory2，createSwapChain 要用它
		ComPtr<IDXGIAdapter> adapter;
		if (dxgiDevice && SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
			adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(dxgiFactory.ReleaseAndGetAddressOf()));
		}
		hr = d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.ReleaseAndGetAddressOf());
		if (FAILED(hr) || !d2dDevice) {
			_ASSERT_EXPR(FALSE, L"D2D1CreateFactory CreateDevice，error");
			return;
		}
		hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, deviceContext.ReleaseAndGetAddressOf());
	}
    // 1. 初始化：只建工厂和默认 TextFormat。
    // CreateTextFormat 的字体集合传 nullptr = 直接用 DWrite 的系统字体集合。
    // 早先的做法是把系统字体集经 FontSetBuilder 在进程内重建一份再绑上去，那份副本
    // （每个 face 的族名/样式/文件引用等元数据）随机器上装的字体数量增长，能占好几 MB，
    // 而且 addFonts 一来还要以它为基底再重建一遍。自定义字体现在单独成集合，见 loadFonts
    void D2D::initFont()
    {
        auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory5),reinterpret_cast<::IUnknown**>(dwriteFactory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            _ASSERT_EXPR(FALSE, L"DWriteCreateFactory, error");
            return;
        }
        hr = dwriteFactory->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.f, L"zh-CN", baseTextFormat.GetAddressOf());
    }

    // 2. 附加：只把资源名记下来，真正的加载交给 loadFonts。设备可能还没建（启动到托盘时
    //    就是这样），这里绝不能因为加字体就把整套 D3D 拉起来
    void D2D::addFonts(const std::vector<std::wstring>& fontResourceNames)
    {
        if (fontResourceNames.empty()) return;
        fontNames.insert(fontNames.end(), fontResourceNames.begin(), fontResourceNames.end());
        if (d2d) d2d->loadFonts();
    }

    // 自定义字体单独成一个集合，不与系统字体合并（合并的代价见 initFont 的注释）。
    // 每次都按 fontNames 全量重建，所以不必操心与上一份集合的合并
    void D2D::loadFonts()
    {
        if (fontNames.empty() || !dwriteFactory) return;
        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(dwriteFactory->CreateFontSetBuilder(&builder))) {
            _ASSERT_EXPR(FALSE, L"CreateFontSetBuilder, error");
            return;
        }
        if (!fontLoader) {
            dwriteFactory->CreateInMemoryFontFileLoader(fontLoader.GetAddressOf());
            dwriteFactory->RegisterFontFileLoader(fontLoader.Get());
        }
        // 逐个加载并追加自定义字体
        for (const auto& resName : fontNames) {
            auto [pData, size] = Util::getRes(resName.c_str());
            if (pData == nullptr || size == 0) {
                _ASSERT_EXPR(FALSE, L"load font res, error");
                // 一个字体资源缺失不该让后面所有字体都不加载（之前这里是 return，
                // 于是缺一个资源 = 整套自定义字体全丢，界面回落到系统字体）
                continue;
            }
            ComPtr<IDWriteFontFile> fontFile;
            auto hr = fontLoader->CreateInMemoryFontFileReference(dwriteFactory.Get(), pData, size, nullptr, fontFile.GetAddressOf());
            if (FAILED(hr)) {
                _ASSERT_EXPR(FALSE, L"font res decode, error");
                continue;
            }
            builder->AddFontFile(fontFile.Get());
        }
        ComPtr<IDWriteFontSet> customFontSet;
        if (FAILED(builder->CreateFontSet(&customFontSet))) {
            _ASSERT_EXPR(FALSE, L"CreateFontSet, error");
            return;
        }
        if (FAILED(dwriteFactory->CreateFontCollectionFromFontSet(customFontSet.Get(), customFontCollection.ReleaseAndGetAddressOf()))) {
            _ASSERT_EXPR(FALSE, L"CreateFontCollectionFromFontSet, error");
            return;
        }
        makeCustomFormats();
    }

    void D2D::makeCustomFormats()
    {
        customFormats.clear();
        if (!customFontCollection) return;
        auto familyCount = customFontCollection->GetFontFamilyCount();
        for (UINT32 i = 0; i < familyCount; i++) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(customFontCollection->GetFontFamily(i, family.GetAddressOf()))) continue;
            ComPtr<IDWriteLocalizedStrings> names;
            if (FAILED(family->GetFamilyNames(names.GetAddressOf()))) continue;
            auto nameCount = names->GetCount();
            if (nameCount == 0) continue;
            // 先取第 0 个名字建 format
            UINT32 len{ 0 };
            if (FAILED(names->GetStringLength(0, &len))) continue;
            // GetString 的 size 是"含结尾 0"的容量：缓冲必须留出这一格，
            // 否则 DWrite 会往 len 个字符的缓冲里多写一个 0，越界一个 wchar
            std::wstring first(len + 1, L'\0');
            if (FAILED(names->GetString(0, first.data(), len + 1))) continue;
            first.resize(len);
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwriteFactory->CreateTextFormat(first.data(), customFontCollection.Get(),
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                12.f, L"zh-CN", format.GetAddressOf()))) continue;
            // 各本地化名字都登记一份（同一个 format），调用方写哪个名字都能命中
            for (UINT32 j = 0; j < nameCount; j++) {
                if (FAILED(names->GetStringLength(j, &len))) continue;
                std::wstring name(len + 1, L'\0');
                if (FAILED(names->GetString(j, name.data(), len + 1))) continue;
                name.resize(len);
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                customFormats[name] = format;
            }
        }
    }

    IDWriteTextFormat* D2D::getTextFormat(const std::wstring& fontFamily)
    {
        if (fontFamily.empty() || customFormats.empty()) return baseTextFormat.Get();
        std::wstring key{ fontFamily };
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        auto it = customFormats.find(key);
        return it == customFormats.end() ? baseTextFormat.Get() : it->second.Get();
    }

	Composition::CompositionDrawingSurface D2D::createDrawingSurface(const Composition::Compositor& comp, float w, float h)
	{
		Composition::CompositionGraphicsDevice graphicsDevice{ nullptr };
		// 设备没建起来（initDevice 失败）时 d2dDevice 是空的，CreateGraphicsDevice 会返回
		// E_INVALIDARG/空设备，随后的 CreateDrawingSurface 抛 hresult_error。
		// 这条路径常从按钮回调里进来（Invoke 是 noexcept），异常穿出去就是整进程终止
		if (!d2dDevice || !comp) return Composition::CompositionDrawingSurface{ nullptr };
		auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
		auto hr = interop->CreateGraphicsDevice(d2dDevice.Get(), reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice)));
		if (FAILED(hr) || !graphicsDevice) return Composition::CompositionDrawingSurface{ nullptr };
		return graphicsDevice.CreateDrawingSurface(
			winrt::Windows::Foundation::Size{ w, h },
			winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied
		);
	}

	ComPtr<IDXGISwapChain1> D2D::createSwapChain(UINT w, UINT h)
	{
		ComPtr<IDXGISwapChain1> swap;
		if (!dxgiFactory || w == 0 || h == 0) return swap;
		DXGI_SWAP_CHAIN_DESC1 scd{
			.Width{ w },
			.Height{ h },
			.Format{ DXGI_FORMAT_B8G8R8A8_UNORM },
			.SampleDesc{ .Count{ 1 } },
			.BufferUsage{ DXGI_USAGE_RENDER_TARGET_OUTPUT },
			// 双缓冲：一张给合成器读，一张给我们画。FLIP_DISCARD 的标准配置，
			// 出帧延迟与帧成本都不受影响（Present 只是换指针）。
			// 这里曾经是 3：当时配的是"不丢帧"的 Present，想靠多一张缓冲避免
			// 连续拖拽时撞车。但实测多出来的那张就是一份整屏显存（1080p 8MB、
			// 4K 33MB）常驻，而合成器交还缓冲的间隔本身就在一帧之内 ——
			// 撞上的代价是一次最多 16ms 的等待，收益却买了一份永远闲置的缓冲
			.BufferCount{ 2 },
			.Scaling{ DXGI_SCALING_STRETCH },
			.SwapEffect{ DXGI_SWAP_EFFECT_FLIP_DISCARD },
			// 窗口是 WS_EX_NOREDIRECTIONBITMAP 的分层窗口，得让 swap chain 也带 alpha
			.AlphaMode{ DXGI_ALPHA_MODE_PREMULTIPLIED }
		};
		auto hr = dxgiFactory->CreateSwapChainForComposition(d3dDevice.Get(), &scd, nullptr, swap.ReleaseAndGetAddressOf());
		if (FAILED(hr)) {
			_ASSERT_EXPR(FALSE, L"CreateSwapChainForComposition，error");
			swap.Reset();
		}
		return swap;
	}

	Composition::ICompositionSurface D2D::createSurfaceForSwapChain(const Composition::Compositor& comp, IDXGISwapChain1* swap)
	{
		Composition::ICompositionSurface surface{ nullptr };
		if (!swap) return surface;
		auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
		interop->CreateCompositionSurfaceForSwapChain(swap, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(winrt::put_abi(surface)));
		return surface;
	}

    ComPtr<IDWriteTextLayout> D2D::makeTextLayout(const std::wstring& text, float fontSize, float w, float h)
    {
        ComPtr<IDWriteTextLayout> layout;
        auto d2d = D2D::get();
        // dwriteFactory / baseTextFormat 建失败时是空的（见 initFont），
        // 直接往下调就是空指针 —— 调用方一律按"layout 为空就不画"处理，这里返回空即可
        if (!d2d || !d2d->dwriteFactory || !d2d->baseTextFormat) return layout;
        d2d->dwriteFactory->CreateTextLayout(text.data(), (UINT32)text.length(), d2d->baseTextFormat.Get(), w, h, layout.GetAddressOf());
        if (!layout) return layout;
        layout->SetFontSize(fontSize, { 0,INT_MAX });
        return layout;
    }

}
