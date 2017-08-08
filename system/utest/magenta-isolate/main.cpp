#include <inttypes.h>

#include <mxcpp/new.h>

// kernel/lib/magenta
#include <magenta/channel_dispatcher.h>
#include <magenta/dispatcher.h>
#include <magenta/event_dispatcher.h>
#include <magenta/handle.h>
#include <magenta/handle_reaper.h>
#include <magenta/state_tracker.h>

// system/public/magenta
#include <magenta/types.h>

Handle* MakeHandle(mxtl::RefPtr<Dispatcher> dispatcher, mx_rights_t rights) {
    return new Handle(dispatcher, rights, /*base_value=*/0x55555555);
}

namespace internal {
void TearDownHandle(Handle* handle) {
    delete handle;
}
} // namespace internal

void DeleteHandle(Handle* handle) {
    mxtl::RefPtr<Dispatcher> dispatcher(handle->dispatcher());
    auto state_tracker = dispatcher->get_state_tracker();

    if (state_tracker) {
        state_tracker->Cancel(handle);
    }

    internal::TearDownHandle(handle);
}

void ReapHandles(Handle** handles, uint32_t num_handles) {
    while (num_handles > 0) {
        DeleteHandle(*handles++);
    }
}

int main(int argc, char** argv) {
    Handle* h = MakeHandle(nullptr, MX_RIGHT_READ);
    StateTracker st(0x5);
    printf("signals 0x%x\n", st.GetSignalsState());
    printf("rights 0x%x\n", h->rights());

    mxtl::RefPtr<Dispatcher> ev;
    mx_rights_t evr;
    EventDispatcher::Create(0u, &ev, &evr);
    printf("ev koid %" PRIu64 "\n", ev->get_koid());

    mxtl::RefPtr<Dispatcher> ch0;
    mxtl::RefPtr<Dispatcher> ch1;
    mx_rights_t chr;
    ChannelDispatcher::Create(0u, &ch0, &ch1, &chr);
    printf("ch0 koid %" PRIu64 ", related %" PRIu64 "\n",
           ch0->get_koid(), ch0->get_related_koid());
    printf("ch1 koid %" PRIu64 ", related %" PRIu64 "\n",
           ch1->get_koid(), ch1->get_related_koid());
    return 0;
}
