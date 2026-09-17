#include "weaknet_client.h"

static void event_callback(const char* event_type,
                           const char* message,
                           int32_t counter,
                           const char* source) {
    (void)event_type;
    (void)message;
    (void)counter;
    (void)source;
}

int main(void) {
    weaknet_event_callback_t* callback = event_callback;
    return callback == 0;
}
