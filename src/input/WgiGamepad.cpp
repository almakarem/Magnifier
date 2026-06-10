#include "input/WgiGamepad.h"
#include "util/Log.h"

// Raw WinRT ABI approach (no cppwinrt). The cppwinrt headers shipped in
// the Windows 10.0.19041 SDK have an unconditional #include of
// <experimental/coroutine>, which under /std:c++20 errors out. The ABI
// header below is plain COM, no coroutines, no header gymnastics.
#include <Windows.h>
#include <Unknwn.h>
#include <roapi.h>                              // RoGetActivationFactory
#include <wrl/client.h>                          // ComPtr
#include <wrl/wrappers/corewrappers.h>           // HStringReference

// ABI projections shipped in the SDK
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.gaming.input.h>

#include <Xinput.h>                              // XINPUT_GAMEPAD_* constants

#include <algorithm>
#include <mutex>
#include <vector>

#pragma comment(lib, "runtimeobject.lib")        // RoGetActivationFactory
// WindowsApp.lib is a UWP umbrella; for raw RoGetActivationFactory the
// runtimeobject.lib above is sufficient on desktop.

namespace wgi  = ABI::Windows::Gaming::Input;
namespace wfnd = ABI::Windows::Foundation;
namespace wfc  = ABI::Windows::Foundation::Collections;

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace magnifier {

namespace {

// WGI's GamepadButtons -> XInput wButtons translation table. The mapping
// matches the canonical Xbox layout so InputRouter / button bindings work
// transparently across both backends.
unsigned MapButtons(wgi::GamepadButtons b) {
    using B = wgi::GamepadButtons;
    unsigned out = 0;
    auto bit = [&](B mask, unsigned xi) {
        if ((static_cast<unsigned>(b) & static_cast<unsigned>(mask)) != 0)
            out |= xi;
    };
    bit(B::GamepadButtons_DPadUp,          XINPUT_GAMEPAD_DPAD_UP);
    bit(B::GamepadButtons_DPadDown,        XINPUT_GAMEPAD_DPAD_DOWN);
    bit(B::GamepadButtons_DPadLeft,        XINPUT_GAMEPAD_DPAD_LEFT);
    bit(B::GamepadButtons_DPadRight,       XINPUT_GAMEPAD_DPAD_RIGHT);
    bit(B::GamepadButtons_Menu,            XINPUT_GAMEPAD_START);
    bit(B::GamepadButtons_View,            XINPUT_GAMEPAD_BACK);
    bit(B::GamepadButtons_LeftThumbstick,  XINPUT_GAMEPAD_LEFT_THUMB);
    bit(B::GamepadButtons_RightThumbstick, XINPUT_GAMEPAD_RIGHT_THUMB);
    bit(B::GamepadButtons_LeftShoulder,    XINPUT_GAMEPAD_LEFT_SHOULDER);
    bit(B::GamepadButtons_RightShoulder,   XINPUT_GAMEPAD_RIGHT_SHOULDER);
    bit(B::GamepadButtons_A,               XINPUT_GAMEPAD_A);
    bit(B::GamepadButtons_B,               XINPUT_GAMEPAD_B);
    bit(B::GamepadButtons_X,               XINPUT_GAMEPAD_X);
    bit(B::GamepadButtons_Y,               XINPUT_GAMEPAD_Y);
    return out;
}

float Clamp1(double v) {
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    return static_cast<float>(v);
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
// We use a simple polling strategy: each Read() asks IGamepadStatics for
// the current Gamepads collection and takes the first one. That avoids
// the complexity of subscribing to events (which in raw ABI requires a
// hand-rolled IEventHandler<T> implementation) and is fast enough for a
// per-tick poll (the call is essentially a vtable hop into combase).
struct WgiGamepad::Impl {
    ComPtr<wgi::IGamepadStatics> statics;
    bool inited_winrt = false;
};

WgiGamepad::WgiGamepad() : impl_(std::make_unique<Impl>()) {
    // Initialise WinRT in multi-threaded apartment on this thread. If
    // someone already initialised STA we just live with that.
    HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        impl_->inited_winrt = true;
    } else if (hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        spdlog::warn("WGI: RoInitialize failed (0x{:08x}); WGI disabled",
                     static_cast<unsigned>(hr));
        return;
    }

    // Activate the Gamepad statics factory.
    HStringReference cls(RuntimeClass_Windows_Gaming_Input_Gamepad);
    hr = ::RoGetActivationFactory(
        cls.Get(),
        __uuidof(wgi::IGamepadStatics),
        reinterpret_cast<void**>(impl_->statics.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::warn("WGI: RoGetActivationFactory(Gamepad) failed (0x{:08x}); "
                     "falling back to XInput only",
                     static_cast<unsigned>(hr));
        impl_->statics.Reset();
        return;
    }

    available_ = true;

    // One-shot probe to log what's currently connected.
    ComPtr<wfc::IVectorView<wgi::Gamepad*>> view;
    if (SUCCEEDED(impl_->statics->get_Gamepads(view.GetAddressOf()))) {
        unsigned n = 0;
        view->get_Size(&n);
        spdlog::info("WGI: Windows.Gaming.Input backend ready "
                     "({} device(s) at startup)", n);
    } else {
        spdlog::info("WGI: backend ready (could not enumerate at startup)");
    }
}

WgiGamepad::~WgiGamepad() {
    // Release the factory before tearing down WinRT.
    if (impl_) impl_->statics.Reset();
    // We deliberately do NOT call RoUninitialize(): WGI keeps worker
    // threads with outstanding references that release asynchronously,
    // and uninit'ing here can race with them.
}

WgiGamepad::Reading WgiGamepad::Read() {
    Reading r{};
    if (!available_ || !impl_ || !impl_->statics) return r;

    ComPtr<wfc::IVectorView<wgi::Gamepad*>> view;
    HRESULT hr = impl_->statics->get_Gamepads(view.GetAddressOf());
    if (FAILED(hr) || !view) return r;

    unsigned count = 0;
    view->get_Size(&count);
    if (count == 0) return r;

    ComPtr<wgi::IGamepad> gp;
    if (FAILED(view->GetAt(0, gp.GetAddressOf())) || !gp) return r;

    wgi::GamepadReading reading{};
    hr = gp->GetCurrentReading(&reading);
    if (FAILED(hr)) {
        // Stale handle (device dropped between snapshot and read).
        return r;
    }

    r.present     = true;
    r.lx          = Clamp1(reading.LeftThumbstickX);
    r.ly          = Clamp1(reading.LeftThumbstickY);
    r.rx          = Clamp1(reading.RightThumbstickX);
    r.ry          = Clamp1(reading.RightThumbstickY);
    r.lt          = Clamp1(reading.LeftTrigger);
    r.rt          = Clamp1(reading.RightTrigger);
    r.buttons     = MapButtons(reading.Buttons);
    r.device_name = "Gamepad (WGI)";

    return r;
}

} // namespace magnifier
