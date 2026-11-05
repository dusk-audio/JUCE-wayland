/*
 // Distributed under the GPLv3 license
 */

namespace juce
{

class WaylandInputTest final : public UnitTest
{
public:
    WaylandInputTest() : UnitTest ("WaylandInput", UnitTestCategories::gui) {}

    void runTest() override
    {
        beginTest ("Late attachment replays owned keymap and raw input state in order");
        {
            WaylandInputState state;
            Recorder recorder;
            int descriptors[2] { -1, -1 };
            expectEquals (pipe (descriptors), 0);
            if (descriptors[0] < 0)
                return;
            close (descriptors[1]);
            auto* surface = reinterpret_cast<wl_surface*> (&recorder);
            state.keymap (nullptr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, descriptors[0], 42);
            state.repeatInfo (nullptr, 25, 500);
            uint32 held[] { 42 };
            wl_array keys { sizeof (held), 0, held };
            state.keyboardEnter (nullptr, 7, surface, &keys);
            state.key (nullptr, 8, 9, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
            state.modifiers (nullptr, 10, 1, 2, 4, 3);
            state.pointerEnter (nullptr, 11, surface, 256, 512);
            state.pointerMotion (nullptr, 12, 768, 1024);
            const auto token = state.attach (recorder.forwarder());
            expect (token != 0);
            expect (recorder.events == std::vector<int> ({ 1, 2, 3, 4, 5 }));
            expect (recorder.keys == std::vector<uint32> ({ 30, 42 }));
            expect (recorder.modifiers == std::vector<uint32> ({ 10, 1, 2, 4, 3 }));
            expectEquals (recorder.x, 768);
            expectEquals (recorder.y, 1024);
            expect (recorder.keymapWasOpen);
            expect (recorder.keymapFd != descriptors[0]);
            expect (fcntl (descriptors[0], F_GETFD) >= 0);
            expect (fcntl (recorder.keymapFd, F_GETFD) < 0);
            expect (state.attach (recorder.forwarder()) == 0);
            state.detach (token);
            expectEquals (recorder.resets, 1);
        }

        beginTest ("Replacing the keymap closes its predecessor and destruction closes the cache");
        {
            int first[2] { -1, -1 }, second[2] { -1, -1 };
            expectEquals (pipe (first), 0);
            expectEquals (pipe (second), 0);
            if (first[0] < 0 || second[0] < 0)
            {
                for (auto fd : { first[0], first[1], second[0], second[1] })
                    if (fd >= 0)
                        close (fd);
                return;
            }
            close (first[1]);
            close (second[1]);
            {
                WaylandInputState state;
                state.keymap (nullptr, 1, first[0], 1);
                state.keymap (nullptr, 1, second[0], 1);
                expect (fcntl (first[0], F_GETFD) < 0);
                expect (fcntl (second[0], F_GETFD) >= 0);
            }
            expect (fcntl (second[0], F_GETFD) < 0);
        }

        beginTest ("Consumed releases survive detach and re-registration");
        {
            WaylandInputState state;
            Recorder first, second;
            first.consume = true;
            const auto token = state.attach (first.forwarder());
            expect (state.key (nullptr, 1, 2, 30, WL_KEYBOARD_KEY_STATE_PRESSED));
            expect (state.button (nullptr, 3, 4, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED));
            const ModifierKeys all (ModifierKeys::ctrlModifier | ModifierKeys::leftButtonModifier | ModifierKeys::rightButtonModifier);
            expect (! state.hostMouseModifiers (all).isLeftButtonDown());
            expect (state.hostMouseModifiers (all).isRightButtonDown());
            expect (state.hostMouseModifiers (all).isCtrlDown());
            state.detach (token);
            expect (! state.hostMouseModifiers (all).isLeftButtonDown());
            const auto replacement = state.attach (second.forwarder());
            expect (! state.detach (token));
            expectEquals (second.resets, 0);
            expect (state.key (nullptr, 5, 6, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
            expect (state.button (nullptr, 7, 8, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED));
            expect (state.hostMouseModifiers (all).isLeftButtonDown());
            expect (! state.key (nullptr, 9, 10, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
            expect (! state.button (nullptr, 11, 12, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED));
            expect (! state.key (nullptr, 13, 14, 31, WL_KEYBOARD_KEY_STATE_PRESSED));
            state.detach (replacement);
        }

        beginTest ("Detach during a callback preserves its verdict and resets after return");
        {
            WaylandInputState state;
            Recorder recorder;
            recorder.consume = true;
            const auto token = state.attach (recorder.forwarder());
            recorder.onKey = [&] {
                state.detach (token);
                expectEquals (recorder.resets, 0);
                expect (state.attach (recorder.forwarder()) == 0);
            };
            recorder.onReset = [&] {
                expect (state.attach (recorder.forwarder()) == 0);
                state.key (nullptr, 0, 0, 31, WL_KEYBOARD_KEY_STATE_PRESSED);
            };
            expect (state.key (nullptr, 1, 2, 30, WL_KEYBOARD_KEY_STATE_PRESSED));
            expectEquals (recorder.resets, 1);
            expectEquals (recorder.keyEvents, 1);
            expect (state.key (nullptr, 3, 4, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
        }

        beginTest ("Destroyed foreign surfaces are not replayed and host focus survives");
        {
            WaylandInputState state;
            Recorder recorder;
            auto* host = reinterpret_cast<wl_surface*> (&state);
            auto* child = reinterpret_cast<wl_surface*> (&recorder);
            wl_array keys { 0, 0, nullptr };
            state.keyboardEnter (nullptr, 1, host, &keys);
            state.pointerEnter (nullptr, 2, child, 0, 0);
            state.forgetSurface (child);
            auto token = state.attach (recorder.forwarder());
            expect (recorder.events == std::vector<int> ({ 3 }));
            state.detach (token);
            state.pointerEnter (nullptr, 3, child, 0, 0);
            state.forgetForeignSurfaces ([&] (wl_surface* surface) { return surface == host; });
            recorder.events.clear();
            token = state.attach (recorder.forwarder());
            expect (recorder.events == std::vector<int> ({ 3 }));
            state.detach (token);
        }

        beginTest ("Keyboard enter reconciles consumed releases without claiming host-held keys");
        {
            WaylandInputState state;
            Recorder recorder;
            auto* surface = reinterpret_cast<wl_surface*> (&recorder);
            const auto token = state.attach (recorder.forwarder());
            recorder.consume = true;
            expect (state.key (nullptr, 1, 2, 30, WL_KEYBOARD_KEY_STATE_PRESSED));
            expect (state.key (nullptr, 3, 4, 31, WL_KEYBOARD_KEY_STATE_PRESSED));
            uint32 held[] { 30, 42 };
            wl_array keys { sizeof (held), 0, held };
            state.keyboardEnter (nullptr, 5, surface, &keys);
            recorder.consume = false;
            expect (state.key (nullptr, 6, 7, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
            expect (! state.key (nullptr, 8, 9, 31, WL_KEYBOARD_KEY_STATE_RELEASED));
            expect (! state.key (nullptr, 10, 11, 42, WL_KEYBOARD_KEY_STATE_RELEASED));
            state.detach (token);
        }

        beginTest ("Repeated capabilities retain exactly one proxy until removal");
        {
            wl_keyboard* proxy = nullptr;
            int creates = 0, destroys = 0, storage = 0;
            const auto create = [&] { ++creates; return reinterpret_cast<wl_keyboard*> (&storage); };
            const auto destroy = [&] (wl_keyboard* old) {
                expect (proxy == nullptr);
                expect (old == reinterpret_cast<wl_keyboard*> (&storage));
                ++destroys;
            };
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                for (int repeat = 0; repeat < 4; ++repeat)
                    WaylandInputState::updateProxy (proxy, true, create, destroy);
                expectEquals (creates, cycle + 1);
                expect (proxy != nullptr);
                for (int repeat = 0; repeat < 4; ++repeat)
                    WaylandInputState::updateProxy (proxy, false, create, destroy);
                expectEquals (destroys, cycle + 1);
            }
        }

        beginTest ("Unchanged capabilities preserve focus and serialize nested device updates");
        {
            WaylandInputState state;
            Recorder recorder;
            const auto token = state.attach (recorder.forwarder());
            recorder.focused = true;
            expect (state.replaceDevices (false, false, [&] {
                expect (! state.replaceDevices (false, false, [] {}));
                expect (state.hasKeyboardFocus());
                state.key (nullptr, 1, 2, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
            }));
            expectEquals (recorder.resets, 0);
            expectEquals (recorder.keyEvents, 1);
            expect (state.hasKeyboardFocus());
            state.detach (token);
        }

        beginTest ("Device removal resets before release and replays only surviving input");
        {
            WaylandInputState state;
            Recorder recorder;
            auto* surface = reinterpret_cast<wl_surface*> (&recorder);
            wl_array keys { 0, 0, nullptr };
            int descriptors[2] { -1, -1 };
            expectEquals (pipe (descriptors), 0);
            if (descriptors[0] < 0)
                return;
            close (descriptors[1]);
            state.keymap (nullptr, 1, descriptors[0], 1);
            state.repeatInfo (nullptr, 25, 500);
            state.keyboardEnter (nullptr, 1, surface, &keys);
            state.modifiers (nullptr, 2, 1, 0, 0, 0);
            state.pointerEnter (nullptr, 3, surface, 256, 512);
            const auto token = state.attach (recorder.forwarder());
            recorder.consume = true;
            expect (state.key (nullptr, 4, 5, 30, WL_KEYBOARD_KEY_STATE_PRESSED));
            expect (state.button (nullptr, 6, 7, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED));
            recorder.events.clear();
            recorder.onReset = [&] { expect (fcntl (descriptors[0], F_GETFD) >= 0); };
            bool released = false;
            expect (state.replaceDevices (true, false, [&] {
                expectEquals (recorder.resets, 1);
                expect (fcntl (descriptors[0], F_GETFD) < 0);
                state.key (nullptr, 8, 9, 31, WL_KEYBOARD_KEY_STATE_PRESSED);
                expectEquals (recorder.keyEvents, 1);
                expect (state.attach (recorder.forwarder()) == 0);
                released = true;
            }));
            expect (released);
            expect (recorder.events == std::vector<int> ({ 5 }));
            expect (! state.key (nullptr, 10, 11, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
            expect (state.button (nullptr, 12, 13, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED));
            recorder.onReset = {};
            state.detach (token);
        }

        beginTest ("Proxy removal waits for a forwarded callback to return");
        {
            WaylandInputState state;
            Recorder recorder;
            const auto token = state.attach (recorder.forwarder());
            bool released = false;
            recorder.onKey = [&] {
                expect (! state.replaceDevices (true, true, [&] { released = true; }));
                expectEquals (recorder.resets, 0);
            };
            state.key (nullptr, 1, 2, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
            expect (! released);
            recorder.onKey = {};
            expect (state.replaceDevices (true, true, [&] { released = true; }));
            expect (released);
            expectEquals (recorder.resets, 1);
            state.detach (token);
        }

        beginTest ("Detach from reset prevents replay to a retired consumer");
        {
            WaylandInputState state;
            Recorder recorder;
            auto* surface = reinterpret_cast<wl_surface*> (&recorder);
            state.pointerEnter (nullptr, 1, surface, 0, 0);
            const auto token = state.attach (recorder.forwarder());
            recorder.events.clear();
            recorder.onReset = [&] { expect (state.detach (token)); };
            expect (state.replaceDevices (true, false, [] {}));
            expectEquals (recorder.resets, 1);
            expect (recorder.events.empty());
            expect (! state.detach (token));
        }

        beginTest ("Actual keyboard leave clears consumed keys and focus release is forwarded");
        {
            WaylandInputState state;
            Recorder recorder;
            auto* surface = reinterpret_cast<wl_surface*> (&recorder);
            wl_array keys { 0, 0, nullptr };
            state.keyboardEnter (nullptr, 1, surface, &keys);
            const auto token = state.attach (recorder.forwarder());
            recorder.consume = true;
            expect (state.key (nullptr, 2, 3, 30, WL_KEYBOARD_KEY_STATE_PRESSED));
            state.keyboardLeave (nullptr, 4, surface);
            recorder.consume = false;
            expect (! state.key (nullptr, 5, 6, 30, WL_KEYBOARD_KEY_STATE_RELEASED));
            recorder.focused = true;
            expect (state.hasKeyboardFocus());
            state.releaseFocus();
            expect (! state.hasKeyboardFocus());
            state.detach (token);
        }
    }

private:
    struct Recorder
    {
        std::vector<int> events;
        std::vector<uint32> keys, modifiers;
        int keymapFd = -1, resets = 0, keyEvents = 0;
        wl_fixed_t x = 0, y = 0;
        bool keymapWasOpen = false, consume = false, focused = false;
        std::function<void()> onKey, onReset;

        WaylandWindowSystem::InputForwarder forwarder()
        {
            static const wl_keyboard_listener keyboard {
                [] (void* data, wl_keyboard*, uint32, int fd, uint32) {
                    auto& r = *static_cast<Recorder*> (data);
                    r.events.push_back (1);
                    r.keymapFd = fd;
                    r.keymapWasOpen = fcntl (fd, F_GETFD) >= 0;
                    close (fd);
                },
                [] (void* data, wl_keyboard*, uint32, wl_surface*, wl_array* rawKeys) {
                    auto& r = *static_cast<Recorder*> (data);
                    r.events.push_back (3);
                    r.keys.clear();
                    if (rawKeys->size != 0)
                    {
                        const auto* first = static_cast<const uint32*> (rawKeys->data);
                        r.keys.assign (first, first + rawKeys->size / sizeof (uint32));
                    }
                },
                [] (void*, wl_keyboard*, uint32, wl_surface*) {},
                [] (void* data, wl_keyboard*, uint32, uint32, uint32, uint32) {
                    auto& r = *static_cast<Recorder*> (data);
                    ++r.keyEvents;
                    if (r.onKey)
                        r.onKey();
                },
                [] (void* data, wl_keyboard*, uint32 serial, uint32 depressed, uint32 latched, uint32 locked, uint32 group) {
                    auto& r = *static_cast<Recorder*> (data);
                    r.events.push_back (4);
                    r.modifiers = { serial, depressed, latched, locked, group };
                },
                [] (void* data, wl_keyboard*, int32, int32) { static_cast<Recorder*> (data)->events.push_back (2); }
            };
            static const wl_pointer_listener pointer {
                [] (void* data, wl_pointer*, uint32, wl_surface*, wl_fixed_t surfaceX, wl_fixed_t surfaceY) {
                    auto& r = *static_cast<Recorder*> (data);
                    r.events.push_back (5);
                    r.x = surfaceX;
                    r.y = surfaceY;
                },
                nullptr, nullptr,
                [] (void*, wl_pointer*, uint32, uint32, uint32, uint32) {}
            };
            return { &keyboard, &pointer, this,
                [] (void* data) { return static_cast<Recorder*> (data)->consume; },
                [] (void* data) { return static_cast<Recorder*> (data)->focused; },
                [] (void* data) { static_cast<Recorder*> (data)->focused = false; },
                [] (void* data) {
                    auto& r = *static_cast<Recorder*> (data);
                    ++r.resets;
                    r.consume = false;
                    if (r.onReset)
                        r.onReset();
                }
            };
        }
    };
};

static WaylandInputTest waylandInputTest;

} // namespace juce
