#include "bs/bs_nav.h"

bs_nav_id_t bs_nav_from_key(bs_key_t key) {
    switch (key.id) {
        case BS_KEY_RIGHT: return BS_NAV_RIGHT;
        case BS_KEY_LEFT:  return BS_NAV_LEFT;
        case BS_KEY_DOWN:  return BS_NAV_DOWN;
        case BS_KEY_UP:    return BS_NAV_UP;
        case BS_KEY_ENTER: return BS_NAV_SELECT;
        case BS_KEY_BACK:  return BS_NAV_BACK;
        case BS_KEY_ESC:   return BS_NAV_BACK;
        case BS_KEY_CHAR:
            switch (key.ch) {
                case 'w': case 'W': return BS_NAV_UP;
                case 's': case 'S': return BS_NAV_DOWN;
                case 'a': case 'A': return BS_NAV_LEFT;
                case 'd': case 'D': return BS_NAV_RIGHT;
                default:            return BS_NAV_NONE;
            }
        default: return BS_NAV_NONE;
    }
}

bs_nav_id_t bs_nav_poll(void) {
    bs_key_t key;
    if (!bs_keys_poll(&key)) return BS_NAV_NONE;
    return bs_nav_from_key(key);
}
