/*
 * appsandbox-audio.exe — Guest-side audio capture for AppSandbox.
 *
 * Runs as SYSTEM in the console session, spawned by the agent service.
 * Listens on AF_HYPERV socket (GUID :0004) for the host IDD display to
 * connect, then captures audio from the "App Sandbox Virtual Audio" render
 * endpoint via WASAPI loopback and streams raw PCM frames to the host.
 *
 * Low-latency path: VAD (kernel) → WASAPI loopback → HvSocket → host.
 * Socket writes are basically memcpy on the virtual bus so each packet
 * costs almost nothing; we poll the capture client at 5 ms and drain.
 *
 * Wire protocol:
 *   Once per connection: AudioHeader { magic, rate, ch, bits, fmt, rsvd }
 *   Then repeating:      AudioFrameHeader { bytes } + raw PCM bytes
 *
 * Logs to C:\Windows\AppSandbox\audio.log.
 */

#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wtsapi32.lib")

/* ---- GUID storage (materialize COM + KS GUIDs locally) ---- */

DEFINE_GUID(APPSANDBOX_CLSID_MMDeviceEnumerator,
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(APPSANDBOX_IID_IMMDeviceEnumerator,
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(APPSANDBOX_IID_IAudioClient,
    0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(APPSANDBOX_IID_IAudioCaptureClient,
    0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(APPSANDBOX_KSDATAFORMAT_SUBTYPE_PCM,
    0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(APPSANDBOX_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Audio channel: {A5B0CAFE-0004-4000-8000-000000000001} */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Wire protocol ---- */

#define AUDIO_HEADER_MAGIC  0x31415341u  /* 'ASA1' little-endian */

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;       /* 1 = WAVE_FORMAT_PCM, 3 = WAVE_FORMAT_IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

static const wchar_t VAD_DEVICE_NAME[] = L"App Sandbox Virtual Audio";

/* Capture poll interval — 5 ms gives good latency without burning CPU. */
#define AUDIO_POLL_MS       5

/* WASAPI requested buffer size (100-ns units). 40 ms = 400000. */
#define AUDIO_BUFFER_HNS    400000

/* ---- Logging ---- */

static void audio_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\audio.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- Reliable send ---- */

static BOOL send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Find the VAD render endpoint by friendly name ---- */

static IMMDevice *find_vad_device(void)
{
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDeviceCollection *pColl = NULL;
    IMMDevice *pFound = NULL;
    UINT count = 0;
    UINT i;
    HRESULT hr;

    hr = CoCreateInstance(&APPSANDBOX_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &APPSANDBOX_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) {
        audio_log("CoCreateInstance(MMDeviceEnumerator) failed (0x%08lx).", hr);
        return NULL;
    }

    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eRender, DEVICE_STATE_ACTIVE, &pColl);
    if (FAILED(hr)) {
        audio_log("EnumAudioEndpoints failed (0x%08lx).", hr);
        pEnum->lpVtbl->Release(pEnum);
        return NULL;
    }

    pColl->lpVtbl->GetCount(pColl, &count);
    audio_log("Enumerated %u active render endpoints.", count);

    for (i = 0; i < count; i++) {
        IMMDevice *pDev = NULL;
        IPropertyStore *pProps = NULL;
        PROPVARIANT name;

        PropVariantInit(&name);

        if (FAILED(pColl->lpVtbl->Item(pColl, i, &pDev)))
            continue;

        if (FAILED(pDev->lpVtbl->OpenPropertyStore(pDev, STGM_READ, &pProps))) {
            pDev->lpVtbl->Release(pDev);
            continue;
        }

        if (SUCCEEDED(pProps->lpVtbl->GetValue(pProps, &PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR && name.pwszVal != NULL) {
            audio_log("  [%u] %ls", i, name.pwszVal);
            /* Endpoint friendly names look like "Speakers (App Sandbox Virtual Audio)" —
               substring match so we tolerate the class prefix and any dup index. */
            if (wcsstr(name.pwszVal, VAD_DEVICE_NAME) != NULL) {
                pFound = pDev;
                pDev = NULL;  /* transfer ownership */
            }
        }

        PropVariantClear(&name);
        pProps->lpVtbl->Release(pProps);
        if (pDev) pDev->lpVtbl->Release(pDev);

        if (pFound) break;
    }

    pColl->lpVtbl->Release(pColl);
    pEnum->lpVtbl->Release(pEnum);

    if (!pFound)
        audio_log("VAD endpoint '%ls' not found.", VAD_DEVICE_NAME);

    return pFound;
}

/* ---- Resolve the wire format tag (PCM=1, float=3) from a WAVEFORMATEX ---- */

static UINT16 wfx_format_tag(const WAVEFORMATEX *wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ext->SubFormat, &APPSANDBOX_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            return WAVE_FORMAT_IEEE_FLOAT;
        if (IsEqualGUID(&ext->SubFormat, &APPSANDBOX_KSDATAFORMAT_SUBTYPE_PCM))
            return WAVE_FORMAT_PCM;
    }
    return wfx->wFormatTag;
}

/* ---- Initialize loopback capture on the given device ---- */

static BOOL init_capture(IMMDevice *pDevice,
                         IAudioClient **outClient,
                         IAudioCaptureClient **outCapture,
                         WAVEFORMATEX **outFmt)
{
    IAudioClient *pClient = NULL;
    IAudioCaptureClient *pCapture = NULL;
    WAVEFORMATEX *pwfx = NULL;
    HRESULT hr;

    hr = pDevice->lpVtbl->Activate(pDevice, &APPSANDBOX_IID_IAudioClient, CLSCTX_ALL, NULL,
                                   (void **)&pClient);
    if (FAILED(hr)) {
        audio_log("IMMDevice::Activate(IAudioClient) failed (0x%08lx).", hr);
        return FALSE;
    }

    hr = pClient->lpVtbl->GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        audio_log("GetMixFormat failed (0x%08lx).", hr);
        pClient->lpVtbl->Release(pClient);
        return FALSE;
    }

    audio_log("Mix format: %u Hz, %u ch, %u bit, tag=0x%04x (wire tag=%u).",
              pwfx->nSamplesPerSec, pwfx->nChannels, pwfx->wBitsPerSample,
              pwfx->wFormatTag, wfx_format_tag(pwfx));

    hr = pClient->lpVtbl->Initialize(pClient, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     AUDIO_BUFFER_HNS, 0, pwfx, NULL);
    if (FAILED(hr)) {
        audio_log("IAudioClient::Initialize failed (0x%08lx).", hr);
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        return FALSE;
    }

    hr = pClient->lpVtbl->GetService(pClient, &APPSANDBOX_IID_IAudioCaptureClient,
                                     (void **)&pCapture);
    if (FAILED(hr)) {
        audio_log("GetService(IAudioCaptureClient) failed (0x%08lx).", hr);
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        return FALSE;
    }

    hr = pClient->lpVtbl->Start(pClient);
    if (FAILED(hr)) {
        audio_log("IAudioClient::Start failed (0x%08lx).", hr);
        pCapture->lpVtbl->Release(pCapture);
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        return FALSE;
    }

    *outClient  = pClient;
    *outCapture = pCapture;
    *outFmt     = pwfx;
    return TRUE;
}

static void teardown_capture(IAudioClient *pClient,
                             IAudioCaptureClient *pCapture,
                             WAVEFORMATEX *pwfx)
{
    if (pClient)  pClient->lpVtbl->Stop(pClient);
    if (pCapture) pCapture->lpVtbl->Release(pCapture);
    if (pClient)  pClient->lpVtbl->Release(pClient);
    if (pwfx)     CoTaskMemFree(pwfx);
}

/* ---- Stream loop: drain packets, send over socket ---- */

static BOOL stream_loop(SOCKET s,
                        IAudioCaptureClient *pCapture,
                        const WAVEFORMATEX *pwfx)
{
    /* Silence buffer for AUDCLNT_BUFFERFLAGS_SILENT packets — zero once. */
    static BYTE silence[64 * 1024];

    for (;;) {
        UINT32 packetLen = 0;
        HRESULT hr;

        Sleep(AUDIO_POLL_MS);

        /* Drain everything that's queued right now. */
        for (;;) {
            BYTE *pData = NULL;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            UINT32 bytes;
            AudioFrameHeader fh;

            hr = pCapture->lpVtbl->GetNextPacketSize(pCapture, &packetLen);
            if (FAILED(hr)) {
                audio_log("GetNextPacketSize failed (0x%08lx).", hr);
                return FALSE;
            }
            if (packetLen == 0) break;

            hr = pCapture->lpVtbl->GetBuffer(pCapture, &pData, &numFrames,
                                             &flags, NULL, NULL);
            if (FAILED(hr)) {
                audio_log("GetBuffer failed (0x%08lx).", hr);
                return FALSE;
            }

            bytes = numFrames * pwfx->nBlockAlign;
            fh.bytes = bytes;

            if (!send_all(s, &fh, sizeof(fh))) {
                pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
                return FALSE;
            }

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                UINT32 remaining = bytes;
                while (remaining > 0) {
                    UINT32 chunk = remaining > sizeof(silence) ? (UINT32)sizeof(silence) : remaining;
                    if (!send_all(s, silence, (int)chunk)) {
                        pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
                        return FALSE;
                    }
                    remaining -= chunk;
                }
            } else {
                if (!send_all(s, pData, (int)bytes)) {
                    pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
                    return FALSE;
                }
            }

            pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
        }
    }
}

/* ---- Handle one connected host ---- */

static void handle_client(SOCKET s)
{
    IMMDevice *pDevice = NULL;
    IAudioClient *pClient = NULL;
    IAudioCaptureClient *pCapture = NULL;
    WAVEFORMATEX *pwfx = NULL;
    AudioHeader hdr;

    pDevice = find_vad_device();
    if (!pDevice) {
        audio_log("VAD device unavailable, dropping client.");
        return;
    }

    if (!init_capture(pDevice, &pClient, &pCapture, &pwfx)) {
        pDevice->lpVtbl->Release(pDevice);
        return;
    }

    /* Release device now that the client holds a reference. */
    pDevice->lpVtbl->Release(pDevice);
    pDevice = NULL;

    hdr.magic           = AUDIO_HEADER_MAGIC;
    hdr.sample_rate     = pwfx->nSamplesPerSec;
    hdr.channels        = pwfx->nChannels;
    hdr.bits_per_sample = pwfx->wBitsPerSample;
    hdr.format_tag      = wfx_format_tag(pwfx);
    hdr.block_align     = pwfx->nBlockAlign;

    if (!send_all(s, &hdr, sizeof(hdr))) {
        audio_log("Failed to send header (%d).", WSAGetLastError());
        teardown_capture(pClient, pCapture, pwfx);
        return;
    }

    audio_log("Capture started: %u Hz, %u ch, %u bit, tag=%u.",
              hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

    stream_loop(s, pCapture, pwfx);

    audio_log("Stream loop ended (host disconnected or error).");
    teardown_capture(pClient, pCapture, pwfx);
}

/* ---- Main ---- */

int main(void)
{
    WSADATA wsa;
    SOCKET listen_s;
    SOCKADDR_HV addr;
    HRESULT hr;

    audio_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        audio_log("WSAStartup failed (%d).", WSAGetLastError());
        return 1;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        audio_log("CoInitializeEx failed (0x%08lx).", hr);
        WSACleanup();
        return 1;
    }

    listen_s = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (listen_s == INVALID_SOCKET) {
        audio_log("socket(AF_HYPERV) failed (%d).", WSAGetLastError());
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.Family    = AF_HYPERV;
    addr.VmId      = HV_GUID_WILDCARD;
    addr.ServiceId = AUDIO_SERVICE_GUID;

    {
        int tries;
        for (tries = 0; tries < 10; tries++) {
            if (bind(listen_s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
                break;
            audio_log("bind attempt %d failed (%d), retrying...",
                      tries + 1, WSAGetLastError());
            Sleep(500);
        }
        if (tries == 10) {
            audio_log("bind failed after 10 attempts, exiting.");
            closesocket(listen_s);
            CoUninitialize();
            WSACleanup();
            return 1;
        }
    }

    if (listen(listen_s, 2) != 0) {
        audio_log("listen failed (%d).", WSAGetLastError());
        closesocket(listen_s);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    audio_log("Listening on GUID a5b0cafe-0004-4000-8000-000000000001.");

    for (;;) {
        SOCKET client_s = accept(listen_s, NULL, NULL);
        if (client_s == INVALID_SOCKET) {
            audio_log("accept failed (%d).", WSAGetLastError());
            Sleep(1000);
            continue;
        }
        audio_log("Host connected.");
        handle_client(client_s);
        closesocket(client_s);
        audio_log("Host disconnected.");
    }
}
