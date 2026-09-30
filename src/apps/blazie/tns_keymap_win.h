/* tns_keymap_win.h -- a Windows key to the Type 'n Speak's key code (tns_board.h: the down code, bit 7 set; the up
 * code is the same without bit 7).  The codes were measured by pressing each on the running firmware
 * (investigation notes); keys the unit does not have give 0.
 */
#ifndef BLAZIE_TNS_KEYMAP_WIN_H
#define BLAZIE_TNS_KEYMAP_WIN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* vk and the key message's lParam (for left/right and the extended keys) */
int tns_code_for_key(WPARAM vk, LPARAM lp);

#endif
