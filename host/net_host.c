// Network for the desktop simulator: always "connected" through the host, pages fetched with the
// curl command-line tool (so the simulator needs no TLS library).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "host_port.h"
#include "net.h"

void net_boot(void) {}
bool net_start(void) { return true; }

void net_status(net_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = NET_CONNECTED;
    strlcpy(o->ssid, "Host network (simulator)", sizeof(o->ssid));
    strlcpy(o->ip, "127.0.0.1", sizeof(o->ip));
    o->rssi = -40;
}

int net_scan(net_ap_t *out, int max)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    if (max < 1) return 0;
    strlcpy(out[0].ssid, "Host network (simulator)", sizeof(out[0].ssid));
    out[0].rssi = -40;
    out[0].secure = false;
    return 1;
}

void net_connect(const char *ssid, const char *password, bool save) { (void)ssid; (void)password; (void)save; }
void net_disconnect(bool forget) { (void)forget; }

const char *net_error_text(int code)
{
    switch (code) {
    case -1: return "There is no network connection. Open Network from the Bevel menu to join a Wi-Fi network.";
    case -2: return "The server could not be reached.";
    case -3: return "The page is too large for this computer.";
    case -4: return "Stopped.";
    case -5: return "Not enough memory to load the page.";
    }
    return "The page could not be loaded.";
}

int net_fetch(const char *url, char **body, int *len, char *ctype, int ctype_n, char *final_url, int final_n,
              int max_bytes, volatile bool *cancel, volatile int *progress)
{
    *body = NULL; *len = 0;
    if (ctype && ctype_n) ctype[0] = 0;
    char bodyf[] = "/tmp/bevel-fetch-XXXXXX";
    int bfd = mkstemp(bodyf);
    if (bfd < 0) return -5;
    close(bfd);
    int out[2];
    if (pipe(out) != 0) { unlink(bodyf); return -5; }
    char maxs[24];
    snprintf(maxs, sizeof(maxs), "%d", max_bytes);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], 1);
        close(out[0]); close(out[1]);
        int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 2);
        execlp("curl", "curl", "-s", "-L", "--max-redirs", "7", "-m", "20", "--max-filesize", maxs,
               "-A", "Mozilla/5.0 (compatible; BevelBrowser/1.0; simulator)",
               "-H", "Accept: text/html,text/plain,*/*;q=0.5", "-H", "Accept-Encoding: identity",
               "-o", bodyf, "-w", "%{http_code} %{url_effective} %{content_type}", "--", url, (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    int status = 0, res = -2;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (cancel && *cancel) { kill(pid, SIGKILL); waitpid(pid, &status, 0); res = -4; break; }
        struct stat st;
        if (progress && stat(bodyf, &st) == 0) *progress = (int)st.st_size;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    char info[1024] = "";
    int n = read(out[0], info, sizeof(info) - 1);
    close(out[0]);
    info[n > 0 ? n : 0] = 0;
    if (res != -4 && WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        int http = 0;
        char eff[600] = "", ct[200] = "";
        sscanf(info, "%d %599s %199[^\n]", &http, eff, ct);
        if (code == 63) res = -3;                    // --max-filesize exceeded
        else if (code != 0 || http == 0) res = -2;
        else {
            FILE *f = fopen(bodyf, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > max_bytes) res = -3;
                else if (!(*body = malloc(sz + 1))) res = -5;
                else {
                    *len = (int)fread(*body, 1, sz, f);
                    (*body)[*len] = 0;
                    res = http;
                    if (ctype && ctype_n) strlcpy(ctype, ct, ctype_n);
                    if (final_url && final_n) strlcpy(final_url, eff[0] ? eff : url, final_n);
                }
                fclose(f);
            }
        }
    }
    unlink(bodyf);
    return res;
}
