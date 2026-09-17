#include "weaknet_client.h"

#include <type_traits>

int main() {
    static_assert(std::is_same_v<decltype(&weaknet_init), bool (*)()>);
    static_assert(std::is_same_v<decltype(&weaknet_cleanup), void (*)()>);
    static_assert(std::is_same_v<decltype(&weaknet_is_connected), bool (*)()>);
    return 0;
}
