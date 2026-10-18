// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ aoc:u (DLC)
// The DLCs whose data is set up next to the game (<data root>/aoc/<data id>.bin, runtime/services/fs.c): none for
// Let's Go; Sword's Expansion Pass is two tiny ones (a keyfile.dat each), the areas themselves are in the 1.3.x update.
// A DLC's data id is the base id (program id + 0x1000) plus its index.
int aoc_list(uint64_t base_id, uint32_t *indices, int max);  // (runtime/services/fs.c)
SERVICE("aoc:u", aoc_u) {
    (void)s;
    uint64_t base = g_program_id + 0x1000;
    uint32_t idx[64];
    int n = aoc_list(base, idx, 64);
    if (n > 64) n = 64;
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = (uint32_t)n; resp_size(resp, 4); break;  // CountAddOnContent
    case 3: {  // ListAddOnContent(offset, count) -> count, indices in the out buffer
        uint32_t off = IN(uint32_t, 0), want = IN(uint32_t, 4), got = 0;
        IpcBuf *b = out_buf(req, 0);
        uint32_t *out = b ? (uint32_t *)(uintptr_t)b->addr : NULL;
        for (uint32_t i = off; out && i < (uint32_t)n && got < want && (got + 1) * 4 <= b->size; i++) out[got++] = idx[i];
        OUT(uint32_t, 0) = got;
        resp_size(resp, 4);
        static bool logged;
        if (!logged) { logged = true; LOG("[aoc] %d add-on content(s), base id %016llx\n", n, (unsigned long long)base); }
        break;
    }
    case 5: OUT(uint64_t, 0) = base; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 7: break;  // PrepareAddOnContent(index): nothing to prepare, the data is on disk
    case 8: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetAddOnContentListChangedEvent
    default: ok_logger(s, req, resp);
    }
}
