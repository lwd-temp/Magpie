#include "pch.h"
#include "Renderer.h"
#include "DeviceResources.h"
#include "ScalingOptions.h"
#include "Logger.h"
#include "Win32Utils.h"
#include "EffectDrawer.h"
#include "StrUtils.h"
#include "Utils.h"
#include "EffectCompiler.h"
#include "GraphicsCaptureFrameSource.h"
#include "DirectXHelper.h"

namespace Magpie::Core {

Renderer::Renderer() noexcept {}

Renderer::~Renderer() noexcept {
	if (_backendThread.joinable()) {
		DWORD backendThreadId = GetThreadId(_backendThread.native_handle());
		// 持续尝试直到 _backendThread 创建了消息队列
		while (!PostThreadMessage(backendThreadId, WM_QUIT, 0, 0)) {
			Sleep(1);
		}
		_backendThread.join();
	}
}

bool Renderer::Initialize(HWND hwndSrc, HWND hwndScaling, const ScalingOptions& options) noexcept {
	_hwndSrc = hwndSrc;
	_hwndScaling = hwndScaling;

	_backendThread = std::thread(std::bind(&Renderer::_BackendThreadProc, this, options));

	if (!_frontendResources.Initialize(options)) {
		Logger::Get().Error("初始化前端资源失败");
		return false;
	}

	if (!_CreateSwapChain(options)) {
		Logger::Get().Error("_CreateSwapChain 失败");
		return false;
	}

	return true;
}

bool Renderer::_CreateSwapChain(const ScalingOptions& options) noexcept {
	RECT scalingWndRect;
	GetWindowRect(_hwndScaling, &scalingWndRect);

	DXGI_SWAP_CHAIN_DESC1 sd {};
	sd.Width = scalingWndRect.right - scalingWndRect.left;
	sd.Height = scalingWndRect.bottom - scalingWndRect.top;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.SampleDesc.Count = 1;
	sd.Scaling = DXGI_SCALING_NONE;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = (options.IsTripleBuffering() || !options.IsVSync()) ? 3 : 2;
	// 渲染每帧之前都会清空后缓冲区，因此无需 DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	// 只要显卡支持始终启用 DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 以支持可变刷新率
	sd.Flags = (_frontendResources.IsSupportTearing() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)
		| DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	winrt::com_ptr<IDXGISwapChain1> dxgiSwapChain = nullptr;
	HRESULT hr = _frontendResources.GetDXGIFactory()->CreateSwapChainForHwnd(
		_frontendResources.GetD3DDevice(),
		_hwndScaling,
		&sd,
		nullptr,
		nullptr,
		dxgiSwapChain.put()
	);
	if (FAILED(hr)) {
		Logger::Get().ComError("创建交换链失败", hr);
		return false;
	}

	_swapChain = dxgiSwapChain.try_as<IDXGISwapChain4>();
	if (!_swapChain) {
		Logger::Get().Error("获取 IDXGISwapChain2 失败");
		return false;
	}

	_swapChain->SetMaximumFrameLatency(1);

	_frameLatencyWaitableObject.reset(_swapChain->GetFrameLatencyWaitableObject());
	if (!_frameLatencyWaitableObject) {
		Logger::Get().Error("GetFrameLatencyWaitableObject 失败");
		return false;
	}

	hr = _frontendResources.GetDXGIFactory()->MakeWindowAssociation(_hwndScaling, DXGI_MWA_NO_ALT_ENTER);
	if (FAILED(hr)) {
		Logger::Get().ComError("MakeWindowAssociation 失败", hr);
	}

	hr = _swapChain->GetBuffer(0, IID_PPV_ARGS(_backBuffer.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("获取后缓冲区失败", hr);
		return false;
	}
	return true;
}

bool Renderer::_ObtainSharedTexture() noexcept {
	// 获取共享纹理
	HRESULT hr = _frontendResources.GetD3DDevice()->OpenSharedResource(
		_sharedTextureHandle, IID_PPV_ARGS(_frontendSharedTexture.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("OpenSharedResource 失败", hr);
		return false;
	}

	_frontendSharedTextureMutex = _frontendSharedTexture.try_as<IDXGIKeyedMutex>();

	D3D11_TEXTURE2D_DESC desc;
	_frontendSharedTexture->GetDesc(&desc);
	_sharedTextureSize = { (LONG)desc.Width,(LONG)desc.Height };

	return true;
}

void Renderer::Render() noexcept {
	if (_sharedTextureMutexKey == 0) {
		// 第一帧尚未渲染完成
		return;
	}

	if (!_frontendSharedTexture) {
		if (!_ObtainSharedTexture()) {
			Logger::Get().Error("_ObtainSharedTexture 失败");
			return;
		}
	}

	WaitForSingleObjectEx(_frameLatencyWaitableObject.get(), 1000, TRUE);

	ID3D11DeviceContext4* d3dDC = _frontendResources.GetD3DDC();
	d3dDC->ClearState();

	ID3D11RenderTargetView* backBufferRtv = _frontendResources.GetRenderTargetView(_backBuffer.get());
	static constexpr FLOAT black[4] = { 0.0f,0.0f,0.0f,1.0f };
	d3dDC->ClearRenderTargetView(backBufferRtv, black);

	const uint64_t key = ++_sharedTextureMutexKey;
	HRESULT hr = _frontendSharedTextureMutex->AcquireSync(key - 1, INFINITE);
	if (FAILED(hr)) {
		return;
	}

	d3dDC->CopyResource(_backBuffer.get(), _frontendSharedTexture.get());

	_frontendSharedTextureMutex->ReleaseSync(key);

	_swapChain->Present(1, 0);

	// 丢弃渲染目标的内容。仅当现有内容将被完全覆盖时，此操作才有效
	d3dDC->DiscardView(backBufferRtv);
}

bool Renderer::_InitFrameSource(const ScalingOptions& options) noexcept {
	switch (options.captureMethod) {
	case CaptureMethod::GraphicsCapture:
		_frameSource = std::make_unique<GraphicsCaptureFrameSource>();
		break;
	/*case CaptureMethod::DesktopDuplication:
		frameSource = std::make_unique<DesktopDuplicationFrameSource>();
		break;
	case CaptureMethod::GDI:
		frameSource = std::make_unique<GDIFrameSource>();
		break;
	case CaptureMethod::DwmSharedSurface:
		frameSource = std::make_unique<DwmSharedSurfaceFrameSource>();
		break;*/
	default:
		Logger::Get().Error("未知的捕获模式");
		return false;
	}

	Logger::Get().Info(StrUtils::Concat("当前捕获模式：", _frameSource->GetName()));

	ID3D11Device5* d3dDevice = _backendResources.GetD3DDevice();
	if (!_frameSource->Initialize(_hwndSrc, _hwndScaling, options, d3dDevice)) {
		Logger::Get().Error("初始化 FrameSource 失败");
		return false;
	}

	D3D11_TEXTURE2D_DESC desc;
	_frameSource->GetOutput()->GetDesc(&desc);
	Logger::Get().Info(fmt::format("源窗口尺寸：{}x{}", desc.Width, desc.Height));

	return true;
}

static std::optional<EffectDesc> CompileEffect(
	const ScalingOptions& scalingOptions,
	const EffectOption& effectOption
) noexcept {
	EffectDesc result;

	result.name = StrUtils::UTF16ToUTF8(effectOption.name);

	if (effectOption.flags & EffectOptionFlags::InlineParams) {
		result.flags |= EffectFlags::InlineParams;
	}
	if (effectOption.flags & EffectOptionFlags::FP16) {
		result.flags |= EffectFlags::FP16;
	}

	uint32_t compileFlag = 0;
	if (scalingOptions.IsDisableEffectCache()) {
		compileFlag |= EffectCompilerFlags::NoCache;
	}
	if (scalingOptions.IsSaveEffectSources()) {
		compileFlag |= EffectCompilerFlags::SaveSources;
	}
	if (scalingOptions.IsWarningsAreErrors()) {
		compileFlag |= EffectCompilerFlags::WarningsAreErrors;
	}

	bool success = true;
	int duration = Utils::Measure([&]() {
		success = !EffectCompiler::Compile(result, compileFlag, &effectOption.parameters);
	});

	if (success) {
		Logger::Get().Info(fmt::format("编译 {}.hlsl 用时 {} 毫秒",
			StrUtils::UTF16ToUTF8(effectOption.name), duration / 1000.0f));
		return result;
	} else {
		Logger::Get().Error(StrUtils::Concat("编译 ",
			StrUtils::UTF16ToUTF8(effectOption.name), ".hlsl 失败"));
		return std::nullopt;
	}
}

ID3D11Texture2D* Renderer::_BuildEffects(const ScalingOptions& options) noexcept {
	assert(!options.effects.empty());

	const uint32_t effectCount = (uint32_t)options.effects.size();

	// 并行编译所有效果
	std::vector<EffectDesc> effectDescs(options.effects.size());
	std::atomic<bool> allSuccess = true;

	int duration = Utils::Measure([&]() {
		Win32Utils::RunParallel([&](uint32_t id) {
			std::optional<EffectDesc> desc = CompileEffect(options, options.effects[id]);
			if (desc) {
				effectDescs[id] = std::move(*desc);
			} else {
				allSuccess = false;
			}
		}, effectCount);
	});

	if (!allSuccess) {
		return nullptr;
	}

	if (effectCount > 1) {
		Logger::Get().Info(fmt::format("编译着色器总计用时 {} 毫秒", duration / 1000.0f));
	}

	/*DownscalingEffect& downscalingEffect = MagApp::Get().GetOptions().downscalingEffect;
	if (!downscalingEffect.name.empty()) {
		_effects.reserve(effectsOption.size() + 1);
	}*/
	_effectDrawers.resize(options.effects.size());

	RECT scalingWndRect;
	GetWindowRect(_hwndScaling, &scalingWndRect);
	SIZE scalingWndSize = Win32Utils::GetSizeOfRect(scalingWndRect);

	ID3D11Texture2D* inOutTexture = _frameSource->GetOutput();
	for (uint32_t i = 0; i < effectCount; ++i) {
		if (!_effectDrawers[i].Initialize(
			effectDescs[i],
			options.effects[i],
			_backendResources,
			scalingWndSize,
			&inOutTexture
		)) {
			Logger::Get().Error(fmt::format("初始化效果#{} ({}) 失败", i, StrUtils::UTF16ToUTF8(options.effects[i].name)));
			return nullptr;
		}
	}

	return inOutTexture;

	/*if (!downscalingEffect.name.empty()) {
		const SIZE hostSize = Win32Utils::GetSizeOfRect(MagApp::Get().GetHostWndRect());
		const SIZE outputSize = Win32Utils::GetSizeOfRect(_virtualOutputRect);
		if (outputSize.cx > hostSize.cx || outputSize.cy > hostSize.cy) {
			// 需降采样
			EffectOption downscalingEffectOption;
			downscalingEffectOption.name = downscalingEffect.name;
			downscalingEffectOption.parameters = downscalingEffect.parameters;
			downscalingEffectOption.scalingType = ScalingType::Fit;
			downscalingEffectOption.flags = EffectOptionFlags::InlineParams;	// 内联参数

			EffectDesc downscalingEffectDesc;

			// 最后一个效果需重新编译
			// 在分离光标渲染逻辑后这里可优化
			duration = Utils::Measure([&]() {
				Win32Utils::RunParallel([&](uint32_t id) {
					if (!CompileEffect(
						id == 1,
						id == 0 ? effectsOption.back() : downscalingEffectOption,
						id == 0 ? effectDescs.back() : downscalingEffectDesc
						)) {
						allSuccess = false;
					}
				}, 2);
			});

			if (!allSuccess) {
				return false;
			}

			Logger::Get().Info(fmt::format("编译降采样着色器用时 {} 毫秒", duration / 1000.0f));

			_effects.pop_back();
			if (_effects.empty()) {
				effectInput = MagApp::Get().GetFrameSource().GetOutput();
			} else {
				effectInput = _effects.back().GetOutputTexture();
			}

			_effects.resize(_effects.size() + 2);

			// 重新构建最后一个效果
			const size_t originLastEffectIdx = _effects.size() - 2;
			if (!_effects[originLastEffectIdx].Initialize(effectDescs.back(), effectsOption.back(),
				effectInput, nullptr, nullptr)
				) {
				Logger::Get().Error(fmt::format("初始化效果#{} ({}) 失败",
					originLastEffectIdx, StrUtils::UTF16ToUTF8(effectsOption.back().name)));
				return false;
			}
			effectInput = _effects[originLastEffectIdx].GetOutputTexture();

			// 构建降采样效果
			if (!_effects.back().Initialize(downscalingEffectDesc, downscalingEffectOption,
				effectInput, &_outputRect, &_virtualOutputRect
				)) {
				Logger::Get().Error(fmt::format("初始化降采样效果 ({}) 失败",
					StrUtils::UTF16ToUTF8(downscalingEffect.name)));
			}
		}
	}*/
}

bool Renderer::_CreateSharedTexture(ID3D11Texture2D* effectsOutput) noexcept {
	D3D11_TEXTURE2D_DESC desc;
	effectsOutput->GetDesc(&desc);
	SIZE textureSize = { (LONG)desc.Width, (LONG)desc.Height };

	// 创建共享纹理
	_backendSharedTexture = DirectXHelper::CreateTexture2D(
		_backendResources.GetD3DDevice(),
		DXGI_FORMAT_R8G8B8A8_UNORM,
		textureSize.cx,
		textureSize.cy,
		D3D11_BIND_SHADER_RESOURCE,
		D3D11_USAGE_DEFAULT,
		D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
	);
	if (!_backendSharedTexture) {
		Logger::Get().Error("创建 Texture2D 失败");
		return false;
	}

	_backendSharedTextureMutex = _backendSharedTexture.try_as<IDXGIKeyedMutex>();

	winrt::com_ptr<IDXGIResource> sharedDxgiRes = _backendSharedTexture.try_as<IDXGIResource>();

	HRESULT hr = sharedDxgiRes->GetSharedHandle(&_sharedTextureHandle);
	if (FAILED(hr)) {
		Logger::Get().Error("GetSharedHandle 失败");
		return false;
	}

	return true;
}

void Renderer::_BackendThreadProc(const ScalingOptions& options) noexcept {
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	if (!_backendResources.Initialize(options)) {
		return;
	}

	ID3D11Device5* d3dDevice = _backendResources.GetD3DDevice();

	if (!_InitFrameSource(options)) {
		return;
	}

	ID3D11Texture2D* outputTexture = _BuildEffects(options);
	if (!outputTexture) {
		return;
	}
	
	HRESULT hr = d3dDevice->CreateFence(_fenceValue, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&_d3dFence));
	if (FAILED(hr)) {
		Logger::Get().ComError("CreateFence 失败", hr);
		return;
	}

	_fenceEvent.reset(Win32Utils::SafeHandle(CreateEvent(nullptr, FALSE, FALSE, nullptr)));
	if (!_fenceEvent) {
		Logger::Get().Win32Error("CreateEvent 失败");
		return;
	}

	if (!_CreateSharedTexture(outputTexture)) {
		Logger::Get().Win32Error("_CreateSharedTexture 失败");
		return;
	}

	MSG msg;
	while (true) {
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				// 不能在前端线程释放
				_frameSource.reset();
				return;
			}

			DispatchMessage(&msg);
		}
		
		_BackendRender(outputTexture);

		// 等待新消息 1ms
		MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
	}
}

void Renderer::_BackendRender(ID3D11Texture2D* effectsOutput) noexcept {
	FrameSourceBase::UpdateState updateState = _frameSource->Update();
	if (updateState != FrameSourceBase::UpdateState::NewFrame) {
		return;
	}

	ID3D11DeviceContext4* d3dDC = _backendResources.GetD3DDC();

	for (const EffectDrawer& effectDrawer : _effectDrawers) {
		effectDrawer.Draw(d3dDC);
	}

	const uint64_t key = ++_sharedTextureMutexKey;
	HRESULT hr = _backendSharedTextureMutex->AcquireSync(key - 1, INFINITE);
	if (FAILED(hr)) {
		return;
	}

	d3dDC->CopyResource(_backendSharedTexture.get(), effectsOutput);

	_backendSharedTextureMutex->ReleaseSync(key);

	hr = d3dDC->Signal(_d3dFence.get(), ++_fenceValue);
	if (FAILED(hr)) {
		return;
	}
	hr = _d3dFence->SetEventOnCompletion(_fenceValue, _fenceEvent.get());
	if (FAILED(hr)) {
		return;
	}

	d3dDC->Flush();

	// 等待渲染完成
	WaitForSingleObject(_fenceEvent.get(), INFINITE);

	// 唤醒前端线程
	PostMessage(_hwndScaling, WM_NULL, 0, 0);
}

}
