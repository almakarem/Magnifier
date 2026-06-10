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
//
// We additionally hold an IRawGameControllerStatics so we can fall back
// to non-"standard gamepad" devices (DualSense in some BT modes, Switch
// Pro variants, generic HID gamepads). RawGameController exposes raw
// axis/button arrays — we apply a DirectInput-style heuristic mapping.
struct WgiGamepad::Impl {
    ComPtr<wgi::IGamepadStatics>           statics;
    ComPtr<wgi::IRawGameControllerStatics> raw_statics;
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

    // Also activate the RawGameController statics — covers pads that
    // don't conform to the strict "standard gamepad" mapping but ARE
    // visible to Windows as HID gamepads / joysticks. Best-effort; if
    // this fails we just skip the fallback path.
    HStringReference raw_cls(RuntimeClass_Windows_Gaming_Input_RawGameController);
    HRESULT raw_hr = ::RoGetActivationFactory(
        raw_cls.Get(),
        __uuidof(wgi::IRawGameControllerStatics),
        reinterpret_cast<void**>(impl_->raw_statics.GetAddressOf()));
    if (FAILED(raw_hr)) {
        spdlog::info("WGI: RawGameController statics unavailable (0x{:08x}); "
                     "fallback path disabled.",
                     static_cast<unsigned>(raw_hr));
        impl_->raw_statics.Reset();
    }

    available_ = true;

    // One-shot probe to log what's currently connected.
    {
        unsigned ng = 0, nr = 0;
        ComPtr<wfc::IVectorView<wgi::Gamepad*>> view;
        if (SUCCEEDED(impl_->statics->get_Gamepads(view.GetAddressOf()))) {
            view->get_Size(&ng);
        }
        ComPtr<wfc::IVectorView<wgi::RawGameController*>> rview;
        if (impl_->raw_statics &&
            SUCCEEDED(impl_->raw_statics->get_RawGameControllers(rview.GetAddressOf()))) {
            rview->get_Size(&nr);
        }
        spdlog::info("WGI: backend ready (Gamepads={}, RawGameControllers={})",
                     ng, nr);
    }
}

WgiGamepad::~WgiGamepad() {
    // Release the factories before tearing down WinRT.
    if (impl_) {
        impl_->statics.Reset();
        impl_->raw_statics.Reset();
    }
    // We deliberately do NOT call RoUninitialize(): WGI keeps worker
    // threads with outstanding references that release asynchronously,
    // and uninit'ing here can race with them.
}

WgiGamepad::Reading WgiGamepad::Read() {
    Reading r{};
    if (!available_ || !impl_ || !impl_->statics) return r;

    // ---- Standard Gamepad path -----------------------------------------
    ComPtr<wfc::IVectorView<wgi::Gamepad*>> view;
    HRESULT hr = impl_->statics->get_Gamepads(view.GetAddressOf());
    unsigned count = 0;
    if (SUCCEEDED(hr) && view) {
        view->get_Size(&count);
    }

    if (count > 0) {
        ComPtr<wgi::IGamepad> gp;
        if (SUCCEEDED(view->GetAt(0, gp.GetAddressOf())) && gp) {
            wgi::GamepadReading reading{};
            if (SUCCEEDED(gp->GetCurrentReading(&reading))) {
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
        }
    }

    // ---- RawGameController fallback ------------------------------------
    // Many BT-paired pads (DualSense in some modes, Switch Pro variants,
    // generic HID gamepads) don't promote to the strict "standard
    // gamepad" class and so don't appear in get_Gamepads(). They DO show
    // up here. We apply a DirectInput-style axis order:
    //   axes[0] = LX, [1] = LY, [2] = RX, [3] = RY, [4] = LT, [5] = RT
    // Axes are doubles in [0,1]; convert sticks to [-1, 1] (Y is
    // typically +down in HID, so we negate to match our +up convention).
    // Buttons are read as bools and packed in HID order; we expose them
    // as a raw bitfield. Hardcoded button-action bindings (A/B/X/Y) may
    // not map correctly across vendors, but axis-based panning will.
    if (!impl_->raw_statics) return r;

    ComPtr<wfc::IVectorView<wgi::RawGameController*>> rview;
    if (FAILED(impl_->raw_statics->get_RawGameControllers(rview.GetAddressOf()))
        || !rview) {
        return r;
    }
    unsigned rcount = 0;
    rview->get_Size(&rcount);
    if (rcount == 0) return r;

    ComPtr<wgi::IRawGameController> rc;
    if (FAILED(rview->GetAt(0, rc.GetAddressOf())) || !rc) return r;

    INT32 nbuttons = 0, nswitches = 0, naxes = 0;
    rc->get_ButtonCount (&nbuttons);
    rc->get_SwitchCount (&nswitches);
    rc->get_AxisCount   (&naxes);

    // Allocate small buffers on the stack (sane upper bounds).
    constexpr INT32 kMaxButtons  = 64;
    constexpr INT32 kMaxSwitches = 8;
    constexpr INT32 kMaxAxes     = 16;
    if (nbuttons  > kMaxButtons)  nbuttons  = kMaxButtons;
    if (nswitches > kMaxSwitches) nswitches = kMaxSwitches;
    if (naxes     > kMaxAxes)     naxes     = kMaxAxes;

    boolean buttons[kMaxButtons]{};
    wgi::GameControllerSwitchPosition switches[kMaxSwitches]{};
    DOUBLE axes[kMaxAxes]{};

    UINT64 timestamp = 0;
    if (FAILED(rc->GetCurrentReading(
            static_cast<UINT32>(nbuttons),  buttons,
            static_cast<UINT32>(nswitches), switches,
            static_cast<UINT32>(naxes),     axes,
            &timestamp))) {
        return r;
    }

    auto axis_to_stick = [](DOUBLE v) -> float {
        // Map [0,1] HID axis to [-1,1] centered. 0.5 = center.
        return Clamp1((v - 0.5) * 2.0);
    };

    r.present = true;
    if (naxes > 0) r.lx =  axis_to_stick(axes[0]);
    if (naxes > 1) r.ly = -axis_to_stick(axes[1]);  // HID Y is +down
    if (naxes > 2) r.rx =  axis_to_stick(axes[2]);
    if (naxes > 3) r.ry = -axis_to_stick(axes[3]);
    if (naxes > 4) r.lt =  Clamp1(axes[4]);
    if (naxes > 5) r.rt =  Clamp1(axes[5]);

    // Pack the first 16 buttons in HID order into the bitfield. Note:
    // these will NOT match XInput's A/B/X/Y/etc. bit positions, so
    // button-action bindings configured for an Xbox pad will likely
    // trigger different actions on a generic pad. That's acceptable
    // for the v0.1.2 fallback path; we'll add per-vendor mapping later
    // once we know which pads users have.
    unsigned bits = 0;
    const INT32 nb = nbuttons < 16 ? nbuttons : 16;
    for (INT32 i = 0; i < nb; ++i) {
        if (buttons[i]) bits |= (1u << i);
    }
    r.buttons = bits;

    r.device_name = "RawGameController (WGI fallback)";
    return r;
}

WgiGamepad::Probe WgiGamepad::ProbeDevices() {
    Probe p{};
    if (!available_ || !impl_) return p;

    if (impl_->statics) {
        ComPtr<wfc::IVectorView<wgi::Gamepad*>> view;
        if (SUCCEEDED(impl_->statics->get_Gamepads(view.GetAddressOf()))) {
            view->get_Size(&p.gamepad_count);
        }
    }
    if (impl_->raw_statics) {
        ComPtr<wfc::IVectorView<wgi::RawGameController*>> rview;
        if (SUCCEEDED(impl_->raw_statics->get_RawGameControllers(rview.GetAddressOf()))) {
            rview->get_Size(&p.raw_controller_count);
            if (p.raw_controller_count > 0) {
                ComPtr<wgi::IRawGameController> rc;
                if (SUCCEEDED(rview->GetAt(0, rc.GetAddressOf())) && rc) {
                    // IRawGameController doesn't expose DisplayName directly;
                    // we'd need IRawGameController2 (added in 19H1+). Best-
                    // effort: leave the name as a generic placeholder so the
                    // user still sees that something was detected.
                    p.first_raw_name = "(unnamed HID gamepad — see log)";
                }
            }
        }
    }
    return p;
}

} // namespace magnifier
