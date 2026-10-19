/*
 * Local wireless play (3ds_local.h) is not on the Thor yet: a LocalLink that reports no
 * radio, so the link counters say nothing is nearby. The adapter the game talks to
 * (3ds_rfu_adapter.c) is a game unit and stays as it is; docs/THOR_PORT_PLAN.md 4.9 has the
 * Wi-Fi version for later.
 */
#include <3ds.h>

#include "3ds_local.h"

void LocalLink_Init(void) {}
void LocalLink_Shutdown(void) {}
uint8_t LocalLink_Enabled(void) { return 0; }
uint16_t LocalLink_MyId(void) { return 0; }
uint32_t LocalLink_Generation(void) { return 0; }
void LocalLink_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data) { (void)hosting; (void)nextSlot; (void)data; }
int LocalLink_GetPeers(struct LocalPeer *out, int max) { (void)out; (void)max; return 0; }
void LocalLink_Send(uint16_t dest, const void *data, uint32_t len) { (void)dest; (void)data; (void)len; }
uint8_t LocalLink_PollEvent(struct LocalEvent *ev) { (void)ev; return 0; }
uint32_t LocalLink_Milliseconds(void) { return (uint32_t)(svcGetSystemTick() / 1000000ULL); }
void LocalLink_StartSession(uint8_t kind) { (void)kind; }
void LocalLink_LeaveSession(void) {}
uint8_t LocalLink_GetSession(void) { return 0; }
uint8_t LocalLink_GetSessionKind(void) { return 0; }
uint8_t LocalLink_InSession(void) { return 0; }
void LocalLink_LeaveBeforeSleep(void) {}
void LocalLink_AfterWake(void) {}
uint8_t LocalLink_SessionSize(void) { return 0; }
