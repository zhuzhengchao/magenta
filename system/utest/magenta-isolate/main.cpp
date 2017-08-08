#include <inttypes.h>

#include <mxcpp/new.h>

// kernel/lib/magenta
#include <magenta/dispatcher.h>
#include <magenta/event_dispatcher.h>
#include <magenta/handle.h>
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

int main(int argc, char** argv) {
    Handle* h = MakeHandle(nullptr, MX_RIGHT_READ);
    StateTracker st(0x5);
    printf("signals 0x%x\n", st.GetSignalsState());
    printf("rights 0x%x\n", h->rights());

    mxtl::RefPtr<Dispatcher> ev;
    mx_rights_t rights;
    EventDispatcher::Create(0u, &ev, &rights);
    printf("ev koid %" PRIu64 "\n", ev->get_koid());
    return 0;
}
