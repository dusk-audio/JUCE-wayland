/*
 // Distributed under the GPLv3 license
 */

#pragma once

namespace juce
{

struct WaylandInputState
{
    using Forwarder = WaylandWindowSystem::InputForwarder;

    WaylandInputState() = default;

    ~WaylandInputState()
    {
        jassert (callbackDepth == 0 && ! updatingDevices);
        detach (registration);
        finishReset();
        if (keymapFd >= 0)
            close (keymapFd);
    }

    uint64 attach (const Forwarder& next)
    {
        if (registration != 0 || callbackDepth != 0 || updatingDevices || ! next.keyboard || ! next.pointer
            || ! next.consumed || ! next.hasKeyboardFocus || ! next.releaseFocus || ! next.reset)
            return 0;

        const auto fd = keymapFd < 0 ? -1 : fcntl (keymapFd, F_DUPFD_CLOEXEC, 0);
        if (keymapFd >= 0 && fd < 0)
            return 0;

        forwarder = next;
        if (++generation == 0)
            ++generation;
        registration = generation;
        replay (fd);
        return registration;
    }

    bool detach (uint64 token)
    {
        if (token == 0 || token != registration)
            return false;

        if (! suspended)
            pendingReset = forwarder;
        forwarder = {};
        registration = 0;
        finishReset();
        return true;
    }

    template <typename Proxy, typename Create, typename Destroy>
    static void updateProxy (Proxy*& proxy, bool available, Create create, Destroy destroy)
    {
        if (available && ! proxy)
            proxy = create();
        else if (! available && proxy)
            destroy (std::exchange (proxy, nullptr));
    }

    template <typename Replace>
    bool replaceDevices (bool removeKeyboard, bool removePointer, Replace replace)
    {
        if (callbackDepth != 0 || updatingDevices)
            return false;

        updatingDevices = true;
        const ScopeGuard complete { [this] { updatingDevices = false; suspended = false; } };
        suspended = removeKeyboard || removePointer;
        if (suspended && registration != 0)
        {
            const auto target = forwarder;
            ++callbackDepth;
            const ScopeGuard resetComplete { [this] { --callbackDepth; } };
            target.reset (target.data);
        }
        if (removeKeyboard)
        {
            if (keymapFd >= 0)
                close (keymapFd);
            keymapFd = -1;
            keyboard = nullptr;
            keyboardSurface = nullptr;
            heldKeys.clear();
            consumedKeys.clear();
            haveModifiers = false;
            haveRepeat = false;
        }
        if (removePointer)
        {
            pointer = nullptr;
            pointerSurface = nullptr;
            consumedButtons.clear();
        }
        replace();
        suspended = false;

        if ((removeKeyboard || removePointer) && registration != 0)
        {
            const auto fd = keymapFd < 0 ? -1 : fcntl (keymapFd, F_DUPFD_CLOEXEC, 0);
            if (keymapFd >= 0 && fd < 0)
                detach (registration);
            else
                replay (fd);
        }
        return true;
    }

    bool hasKeyboardFocus()
    {
        bool focused = false;
        withForwarder ([&] (const Forwarder& target) { focused = target.hasKeyboardFocus (target.data); });
        return focused;
    }

    void releaseFocus()
    {
        withForwarder ([] (const Forwarder& target) { target.releaseFocus (target.data); });
    }

    void forgetSurface (wl_surface* surface)
    {
        if (keyboardSurface == surface)
        {
            keyboardSurface = nullptr;
            heldKeys.clear();
        }
        if (pointerSurface == surface)
            pointerSurface = nullptr;
    }

    template <typename IsHostSurface>
    void forgetForeignSurfaces (IsHostSurface isHostSurface)
    {
        if (keyboardSurface && ! isHostSurface (keyboardSurface))
            forgetSurface (keyboardSurface);
        if (pointerSurface && ! isHostSurface (pointerSurface))
            forgetSurface (pointerSurface);
    }

    void keymap (wl_keyboard* proxy, uint32 format, int fd, uint32 size)
    {
        if (keymapFd >= 0)
            close (keymapFd);
        keyboard = proxy;
        keymapFd = fd;
        keymapFormat = format;
        keymapSize = size;
        if (registration == 0 || suspended)
            return;

        const auto copy = fcntl (fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0)
        {
            detach (registration);
            return;
        }
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->keymap)
                target.keyboard->keymap (target.data, proxy, format, copy, size);
            else
                close (copy);
        });
    }

    void keyboardEnter (wl_keyboard* proxy, uint32 serial, wl_surface* surface, wl_array* keys)
    {
        keyboard = proxy;
        keyboardSerial = serial;
        keyboardSurface = surface;
        heldKeys.clear();
        if (keys && keys->data)
        {
            const auto* first = static_cast<const uint32*> (keys->data);
            heldKeys.insert (first, first + keys->size / sizeof (uint32));
        }
        for (auto it = consumedKeys.begin(); it != consumedKeys.end();)
            if (heldKeys.count (*it) == 0)
                it = consumedKeys.erase (it);
            else
                ++it;
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->enter)
                target.keyboard->enter (target.data, proxy, serial, surface, keys);
        });
    }

    void keyboardLeave (wl_keyboard* proxy, uint32 serial, wl_surface* surface)
    {
        if (keyboardSurface == surface)
        {
            keyboardSurface = nullptr;
            heldKeys.clear();
            consumedKeys.clear();
        }
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->leave)
                target.keyboard->leave (target.data, proxy, serial, surface);
        });
    }

    bool key (wl_keyboard* proxy, uint32 serial, uint32 time, uint32 code, uint32 state)
    {
        const auto pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
        if (pressed)
            heldKeys.insert (code);
        else
            heldKeys.erase (code);
        const auto owned = consumedKeys.count (code) != 0;
        bool consumed = false;
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->key)
            {
                target.keyboard->key (target.data, proxy, serial, time, code, state);
                consumed = target.consumed (target.data);
            }
        });
        if (pressed && consumed)
            consumedKeys.insert (code);
        else if (! pressed)
            consumedKeys.erase (code);
        return consumed || owned;
    }

    void modifiers (wl_keyboard* proxy, uint32 serial, uint32 depressed, uint32 latched, uint32 locked, uint32 group)
    {
        modifierSerial = serial;
        depressedMods = depressed;
        latchedMods = latched;
        lockedMods = locked;
        modifierGroup = group;
        haveModifiers = true;
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->modifiers)
                target.keyboard->modifiers (target.data, proxy, serial, depressed, latched, locked, group);
        });
    }

    void repeatInfo (wl_keyboard* proxy, int32 rate, int32 delay)
    {
        keyboard = proxy;
        repeatRate = rate;
        repeatDelay = delay;
        haveRepeat = true;
        withForwarder ([&] (const Forwarder& target) {
            if (target.keyboard->repeat_info)
                target.keyboard->repeat_info (target.data, proxy, rate, delay);
        });
    }

    void pointerEnter (wl_pointer* proxy, uint32 serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
    {
        pointer = proxy;
        pointerSerial = serial;
        pointerSurface = surface;
        pointerX = x;
        pointerY = y;
        pointerEvent (&wl_pointer_listener::enter, proxy, serial, surface, x, y);
    }

    void pointerLeave (wl_pointer* proxy, uint32 serial, wl_surface* surface)
    {
        if (pointerSurface == surface)
            pointerSurface = nullptr;
        for (const auto code : std::exchange (consumedButtons, {}))
            pointerEvent (&wl_pointer_listener::button, proxy, serial, pointerTime, code,
                          (uint32) WL_POINTER_BUTTON_STATE_RELEASED);
        pointerEvent (&wl_pointer_listener::leave, proxy, serial, surface);
    }

    void pointerMotion (wl_pointer* proxy, uint32 time, wl_fixed_t x, wl_fixed_t y)
    {
        pointerTime = time;
        pointerX = x;
        pointerY = y;
        pointerEvent (&wl_pointer_listener::motion, proxy, time, x, y);
    }

    bool isButtonConsumed (uint32 code) const { return consumedButtons.count (code) != 0; }

    ModifierKeys hostMouseModifiers (ModifierKeys modifiers) const
    {
        if (isButtonConsumed (BTN_LEFT))
            modifiers = modifiers.withoutFlags (ModifierKeys::leftButtonModifier);
        if (isButtonConsumed (BTN_RIGHT))
            modifiers = modifiers.withoutFlags (ModifierKeys::rightButtonModifier);
        if (isButtonConsumed (BTN_MIDDLE))
            modifiers = modifiers.withoutFlags (ModifierKeys::middleButtonModifier);
        return modifiers;
    }

    bool button (wl_pointer* proxy, uint32 serial, uint32 time, uint32 code, uint32 state)
    {
        pointerTime = time;
        const auto owned = consumedButtons.count (code) != 0;
        bool consumed = false;
        withForwarder ([&] (const Forwarder& target) {
            if (target.pointer->button)
            {
                target.pointer->button (target.data, proxy, serial, time, code, state);
                consumed = target.consumed (target.data);
            }
        });
        if (state == WL_POINTER_BUTTON_STATE_PRESSED && consumed)
            consumedButtons.insert (code);
        else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
            consumedButtons.erase (code);
        return consumed || owned;
    }

    template <typename Callback, typename... Args>
    void pointerEvent (Callback callback, Args... args)
    {
        withForwarder ([&] (const Forwarder& target) {
            if (const auto fn = target.pointer->*callback)
                fn (target.data, args...);
        });
    }

private:
    template <typename Callback>
    void withForwarder (Callback callback)
    {
        if (registration == 0 || suspended)
            return;
        const auto target = forwarder;
        ++callbackDepth;
        const ScopeGuard complete { [this] { --callbackDepth; finishReset(); } };
        callback (target);
    }

    void finishReset()
    {
        if (callbackDepth != 0 || ! pendingReset.reset)
            return;
        const auto target = pendingReset;
        pendingReset = {};
        ++callbackDepth;
        const ScopeGuard complete { [this] { --callbackDepth; } };
        target.reset (target.data);
    }

    void replay (int fd)
    {
        withForwarder ([&] (const Forwarder& target) {
            if (fd >= 0)
            {
                if (target.keyboard->keymap)
                    target.keyboard->keymap (target.data, keyboard, keymapFormat, fd, keymapSize);
                else
                    close (fd);
            }
            if (registration == 0)
                return;
            if (haveRepeat && target.keyboard->repeat_info)
                target.keyboard->repeat_info (target.data, keyboard, repeatRate, repeatDelay);
            if (registration == 0)
                return;
            if (keyboardSurface && target.keyboard->enter)
            {
                std::vector<uint32> keys (heldKeys.begin(), heldKeys.end());
                wl_array array { keys.size() * sizeof (uint32), 0, keys.data() };
                target.keyboard->enter (target.data, keyboard, keyboardSerial, keyboardSurface, &array);
            }
            if (registration == 0)
                return;
            if (haveModifiers && target.keyboard->modifiers)
                target.keyboard->modifiers (target.data, keyboard, modifierSerial, depressedMods, latchedMods, lockedMods, modifierGroup);
            if (registration == 0)
                return;
            if (pointerSurface && target.pointer->enter)
                target.pointer->enter (target.data, pointer, pointerSerial, pointerSurface, pointerX, pointerY);
        });
    }

    Forwarder forwarder, pendingReset;
    uint64 generation = 0, registration = 0;
    int callbackDepth = 0;
    bool suspended = false, updatingDevices = false;
    int keymapFd = -1;
    uint32 keymapFormat = 0, keymapSize = 0;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    wl_surface* keyboardSurface = nullptr;
    wl_surface* pointerSurface = nullptr;
    uint32 keyboardSerial = 0, pointerSerial = 0, modifierSerial = 0, pointerTime = 0;
    uint32 depressedMods = 0, latchedMods = 0, lockedMods = 0, modifierGroup = 0;
    int32 repeatRate = 0, repeatDelay = 0;
    wl_fixed_t pointerX = 0, pointerY = 0;
    bool haveModifiers = false, haveRepeat = false;
    std::set<uint32> heldKeys, consumedKeys, consumedButtons;

    JUCE_DECLARE_NON_COPYABLE (WaylandInputState)
};

} // namespace juce
