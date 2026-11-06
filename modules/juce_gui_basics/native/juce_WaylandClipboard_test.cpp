/*
 // Distributed under the GPLv3 license
 */

namespace juce
{
class WaylandClipboardTest final : public UnitTest
{
public:
    WaylandClipboardTest() : UnitTest ("WaylandClipboard", UnitTestCategories::gui) {}

    void runTest() override
    {
        beginTest ("Large self-copy drains its receiver without blocking the event thread");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            WaylandPipeTransfer receiver (fds[0]);
            std::string text (1024 * 1024, 'x');
            text.replace (4095, 1, "\xe6\x97\xa5\xf0\x9f\x99\x82");
            expect (WaylandPipeTransfer::send (fds[1], text.data(), text.size(), &receiver));
            expect (fcntl (fds[1], F_GETFD) < 0);
            expect (receiver.receive());
            expect (std::string (receiver.bytes.begin(), receiver.bytes.end()) == text);
            expect (String::fromUTF8 (receiver.bytes.data(), (int) receiver.bytes.size()) == String::fromUTF8 (text.c_str()));
        }

        beginTest ("An empty publication is a successful transfer");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            WaylandPipeTransfer receiver (fds[0]);
            expect (WaylandPipeTransfer::send (fds[1], "", 0, &receiver));
            expect (receiver.receive());
            expect (receiver.bytes.empty());
        }

        beginTest ("A fragmented UTF-8 character is decoded only after EOF");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            WaylandPipeTransfer receiver (fds[0]);
            const char text[] = "a\xe6\x97\xa5\xf0\x9f\x99\x82z";
            for (size_t i = 0; i < sizeof (text) - 1; ++i)
            {
                expectEquals ((int) write (fds[1], text + i, 1), 1);
                receiver.drain();
                expect (! receiver.complete && ! receiver.failed);
            }
            close (fds[1]);
            expect (receiver.receive());
            expect (String::fromUTF8 (receiver.bytes.data(), (int) receiver.bytes.size()) == String::fromUTF8 (text));
        }

        beginTest ("An incomplete transfer expires without publishing partial bytes");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            WaylandPipeTransfer receiver (fds[0]);
            expectEquals ((int) write (fds[1], "part", 4), 4);
            expect (! receiver.receive (5));
            expect (receiver.failed && ! receiver.complete);
            close (fds[1]);
        }

        beginTest ("A stalled consumer cannot block the source indefinitely");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            const std::string text (1024 * 1024, 'x');
            expect (! WaylandPipeTransfer::send (fds[1], text.data(), text.size(), nullptr, 5));
            expect (fcntl (fds[1], F_GETFD) < 0);
            close (fds[0]);
        }

        beginTest ("A closed consumer does not deliver SIGPIPE or change the signal mask");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            close (fds[0]);
            sigset_t before {}, after {}, pending {};
            pthread_sigmask (SIG_SETMASK, nullptr, &before);
            expect (! WaylandPipeTransfer::send (fds[1], "x", 1, nullptr));
            pthread_sigmask (SIG_SETMASK, nullptr, &after);
            expectEquals (sigismember (&after, SIGPIPE), sigismember (&before, SIGPIPE));
            sigpending (&pending);
            if (! sigismember (&before, SIGPIPE)) expectEquals (sigismember (&pending, SIGPIPE), 0);
            expect (fcntl (fds[1], F_GETFD) < 0);
        }

        beginTest ("A caller-owned pending SIGPIPE survives a failed send");
        {
            int fds[2];
            if (! makePipe (fds)) return;
            close (fds[0]);
            sigset_t onlyPipe {}, saved {}, pending {};
            sigemptyset (&onlyPipe);
            sigaddset (&onlyPipe, SIGPIPE);
            pthread_sigmask (SIG_BLOCK, &onlyPipe, &saved);
            raise (SIGPIPE);
            expect (! WaylandPipeTransfer::send (fds[1], "x", 1, nullptr));
            sigpending (&pending);
            expectEquals (sigismember (&pending, SIGPIPE), 1);
            const timespec zero {};
            expectEquals (sigtimedwait (&onlyPipe, nullptr, &zero), SIGPIPE);
            pthread_sigmask (SIG_SETMASK, &saved, nullptr);
        }
    }

private:
    bool makePipe (int (&fds)[2])
    {
        const auto result = pipe (fds);
        expectEquals (result, 0);
        return result == 0;
    }
};
static WaylandClipboardTest waylandClipboardTest;
}
