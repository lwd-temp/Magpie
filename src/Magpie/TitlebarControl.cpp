#include "pch.h"
#include "TitleBarControl.h"
#if __has_include("TitleBarControl.g.cpp")
#include "TitleBarControl.g.cpp"
#endif
#include "IconHelper.h"
#include "Win32Helper.h"
#include "App.h"
#include "CaptionButtonsControl.h"
#include "RootPage.h"

using namespace ::Magpie;
using namespace winrt;
using namespace Windows::UI::Xaml::Media::Imaging;

namespace winrt::Magpie::implementation {

TitleBarControl::TitleBarControl() {
	// 异步加载 Logo
	[](TitleBarControl* that)->fire_and_forget {
		auto weakThis = that->get_weak();

		SoftwareBitmapSource bitmap;
		co_await bitmap.SetBitmapAsync(IconHelper::ExtractAppSmallIcon());

		if (!weakThis.get()) {
			co_return;
		}

		that->_logo = std::move(bitmap);
		that->RaisePropertyChanged(L"Logo");
	}(this);
}

void TitleBarControl::TitleBarControl_Loading(FrameworkElement const&, IInspectable const&) {
	MUXC::NavigationView rootNavigationView = App::Get().RootPage()->RootNavigationView();
	rootNavigationView.DisplayModeChanged([this](const auto&, const auto& args) {
		bool expanded = args.DisplayMode() == MUXC::NavigationViewDisplayMode::Expanded;
		VisualStateManager::GoToState(
			*this, expanded ? L"Expanded" : L"Compact", App::Get().RootPage()->IsLoaded());
	});
}

void TitleBarControl::IsWindowActive(bool value) {
	VisualStateManager::GoToState(*this, value ? L"Active" : L"NotActive", false);
	CaptionButtons().IsWindowActive(value);
}

CaptionButtonsControl& TitleBarControl::CaptionButtons() noexcept {
	return *get_self<CaptionButtonsControl>(TitleBarControlT::CaptionButtons());
}

}
