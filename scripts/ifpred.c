// ifpred — evaluate the test IO80211's _getIfListCopy applies to candidate interfaces.
//
// airportd enumerates Wi-Fi devices with Apple80211GetIfListCopy -> _getIfListCopy, which
// walks getifaddrs() and keeps an interface only if it looks like 802.11. When that count
// comes back 0 the driver has attached but macOS will never adopt it: no Wi-Fi service can
// be enabled and APPLE80211_IOC_POWER is never sent, so the radio stays off. This prints the
// exact inputs to that decision so a driver-side mismatch is visible without a reboot.
//
// The ioctls and constants were recovered from _getIfListCopy in the dyld shared cache; see
// "Interface adoption by airportd" in AirportItlwm/AGENTS.md. It accepts an interface when
//
//     (SIOCGIFMEDIA current & IFM_NMASK) == IFM_IEEE80211
//
// or, when that ioctl fails, when SIOCGIFTYPE is IFT_ETHER *and* the functional type is
// IFRTYPE_FUNCTIONAL_WIFI_INFRA. Note Wi-Fi is IFT_ETHER, not IFT_IEEE80211.
//
//     clang -O0 -Wall -o /tmp/ifpred scripts/ifpred.c && /tmp/ifpred en0 en1 en3
//
// No special privileges needed; every ioctl here is a read.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

// Hardcoded rather than taken from <sys/sockio.h> so this stays a faithful record of what
// the disassembly showed, and still builds if Apple renames or hides one of them.
#define RE_SIOCGIFTYPE           0xc020699fu   // _IOWR('i', 159, struct ifreq)
#define RE_SIOCGIFMEDIA          0xc02c6938u   // _IOWR('i',  56, struct ifmediareq)
#define RE_SIOCGIFFUNCTIONALTYPE 0xc02069adu   // _IOWR('i', 173, struct ifreq)

#define IFM_NMASK       0x000000e0
#define IFM_ETHER       0x00000020
#define IFM_IEEE80211   0x00000080
#define IFT_ETHER       0x06

static const char *functional(uint32_t t)
{
    switch (t) {
    case 0: return "UNKNOWN";
    case 1: return "LOOPBACK";
    case 2: return "WIRED";
    case 3: return "WIFI_INFRA";
    case 4: return "WIFI_AWDL";
    case 5: return "CELLULAR";
    default: return "other";
    }
}

static int get32(int s, unsigned long req, const char *name, uint32_t *out)
{
    unsigned char buf[128];
    memset(buf, 0, sizeof(buf));
    strlcpy((char *)buf, name, IFNAMSIZ);
    if (ioctl(s, req, buf) != 0)
        return -1;
    *out = *(uint32_t *)(buf + IFNAMSIZ);
    return 0;
}

static void probe(int s, const char *name)
{
    uint32_t type = 0, func = 0, media = 0;
    int have_type  = get32(s, RE_SIOCGIFTYPE, name, &type) == 0;
    int have_func  = get32(s, RE_SIOCGIFFUNCTIONALTYPE, name, &func) == 0;
    int have_media = get32(s, RE_SIOCGIFMEDIA, name, &media) == 0;

    printf("=== %s ===\n", name);
    if (have_type)
        printf("  SIOCGIFTYPE           = 0x%02x %s\n", type,
               type == IFT_ETHER ? "(IFT_ETHER)" : "");
    else
        printf("  SIOCGIFTYPE           : %s\n", strerror(errno));

    if (have_func)
        printf("  SIOCGIFFUNCTIONALTYPE = %u (%s)\n", func, functional(func));
    else
        printf("  SIOCGIFFUNCTIONALTYPE : %s\n", strerror(errno));

    if (have_media) {
        uint32_t n = media & IFM_NMASK;
        printf("  SIOCGIFMEDIA current  = 0x%08x, type nibble 0x%02x %s\n", media, n,
               n == IFM_IEEE80211 ? "(IFM_IEEE80211)" :
               n == IFM_ETHER     ? "(IFM_ETHER)" : "");
    } else {
        printf("  SIOCGIFMEDIA          : %s\n", strerror(errno));
    }

    int gate = have_media ? ((media & IFM_NMASK) == IFM_IEEE80211)
                          : (have_type && type == IFT_ETHER && have_func && func == 3);
    // Only the FIRST gate. _getIfListCopy has at least one further test after this one that
    // is not decoded yet, so "passes" is necessary but demonstrably not sufficient: en3
    // passes here while airportd still reports ifCount[0]. Never read a pass as "adopted".
    printf("  -> first gate (media / type+functional): %s\n\n",
           gate ? "PASSES" : "FAILS");
}

int main(int argc, char **argv)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ifname> [<ifname> ...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++)
        probe(s, argv[i]);
    close(s);
    return 0;
}
