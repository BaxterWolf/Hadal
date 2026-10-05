// Screen + sound streaming
// Packets: [type u8][len u32][payload]
// H info, R audio rate, V h264, A pcm, C cursor, L encode ms, P ping, E error
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <strmif.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <initguid.h>
#include <codecapi.h>

using Microsoft::WRL::ComPtr;

namespace {

struct Out {
    HANDLE h;
    std::mutex mu;
    std::atomic<bool> dead{false};
    bool send(char type, const void* p, DWORD n) {
        std::vector<char> b(5 + (size_t)n);
        b[0] = type;
        memcpy(&b[1], &n, 4);
        if (n) memcpy(&b[5], p, n);
        std::lock_guard<std::mutex> l(mu);
        DWORD w;
        if (dead || !WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr)) dead = true;
        return !dead;
    }
    bool text(char type, const std::string& s) { return send(type, s.data(), (DWORD)s.size()); }
};

void audioLoop(Out& out, const std::function<bool()>& stopped) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> ac;
    ComPtr<IAudioCaptureClient> cc;
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf())) || FAILED(ac->GetMixFormat(&wf)) ||
        FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 500000, 0, wf, nullptr)) ||
        FAILED(ac->GetService(IID_PPV_ARGS(&cc))) || FAILED(ac->Start())) {
        if (wf) CoTaskMemFree(wf);
        return;
    }
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
    int ch = wf->nChannels, bits = wf->wBitsPerSample;
    out.text('R', std::to_string(wf->nSamplesPerSec));
    std::vector<int16_t> pcm;
    while (!stopped() && !out.dead) {
        Sleep(10);
        UINT32 n;
        while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) {
            BYTE* d;
            UINT32 frames;
            DWORD flags;
            if (FAILED(cc->GetBuffer(&d, &frames, &flags, nullptr, nullptr))) break;
            pcm.resize((size_t)frames * 2);
            for (UINT32 i = 0; i < frames; i++)
                for (int c = 0; c < 2; c++) {
                    int s = c < ch ? c : 0;
                    float v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f
                            : isFloat && bits == 32 ? ((float*)d)[i * ch + s]
                            : bits == 16 ? ((int16_t*)d)[i * ch + s] / 32768.f : 0.f;
                    pcm[(size_t)i * 2 + c] = (int16_t)std::clamp(v * 32767.f, -32768.f, 32767.f);
                }
            cc->ReleaseBuffer(frames);
            if (!out.send('A', pcm.data(), (DWORD)(pcm.size() * 2))) break;
        }
    }
    ac->Stop();
    CoTaskMemFree(wf);
}

// Pick monitor
bool pickOutput(int index, ComPtr<IDXGIAdapter1>& adapter, ComPtr<IDXGIOutput1>& output, RECT& rc, int& count) {
    struct Mon { ComPtr<IDXGIAdapter1> a; ComPtr<IDXGIOutput1> o; RECT r; };
    std::vector<Mon> mons;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            ComPtr<IDXGIOutput1> o1;
            if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop && SUCCEEDED(o.As(&o1))) mons.push_back({a, o1, d.DesktopCoordinates});
        }
    }
    std::sort(mons.begin(), mons.end(), [](const Mon& x, const Mon& y) {
        bool px = x.r.left == 0 && x.r.top == 0, py = y.r.left == 0 && y.r.top == 0;
        return px != py ? px : x.r.left != y.r.left ? x.r.left < y.r.left : x.r.top < y.r.top;
    });
    count = (int)mons.size();
    if (mons.empty()) return false;
    Mon& m = mons[(size_t)index % mons.size()];
    adapter = m.a;
    output = m.o;
    rc = m.r;
    return true;
}

std::string videoLoop(Out& out, int maxH, int fps, int mbps, int monitor, bool lowLatency, const std::function<bool()>& stopped) {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    RECT mon;
    int monitors = 0;
    if (!pickOutput(monitor, adapter, output, mon, monitors)) return "no monitor found";
    monitor %= monitors;
    int srcW = mon.right - mon.left, srcH = mon.bottom - mon.top;
    int dstH = std::min(maxH, srcH) & ~1, dstW = (int)((long long)srcW * dstH / srcH) & ~1;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return "cannot open the GPU";
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIOutputDuplication> dup;
    if (FAILED(output->DuplicateOutput(dev.Get(), &dup))) return "screen capture unavailable (is the PC locked?)";

    // BGRA to NV12
    ComPtr<ID3D11VideoDevice> vd;
    ComPtr<ID3D11VideoContext> vc;
    if (FAILED(dev.As(&vd)) || FAILED(ctx.As(&vc))) return "GPU has no video processor";
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {(UINT)fps, 1}, (UINT)srcW, (UINT)srcH,
                                          {(UINT)fps, 1}, (UINT)dstW, (UINT)dstH, D3D11_VIDEO_USAGE_OPTIMAL_SPEED};
    ComPtr<ID3D11VideoProcessorEnumerator> ven;
    ComPtr<ID3D11VideoProcessor> vp;
    if (FAILED(vd->CreateVideoProcessorEnumerator(&cd, &ven)) || FAILED(vd->CreateVideoProcessor(ven.Get(), 0, &vp))) return "cannot create the video processor";
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{}, outCs{};
    inCs.YCbCr_Matrix = 1;
    outCs.YCbCr_Matrix = 1; // BT.709
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vc->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
    vc->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);
    vc->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = srcW;
    td.Height = srcH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> last;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &last))) return "out of GPU memory";
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{0, D3D11_VPIV_DIMENSION_TEXTURE2D, {}};
    ComPtr<ID3D11VideoProcessorInputView> iv;
    if (FAILED(vd->CreateVideoProcessorInputView(last.Get(), ven.Get(), &ivd, &iv))) return "unsupported desktop format";
    const int POOL = 4;
    ComPtr<ID3D11Texture2D> nv12[POOL];
    ComPtr<ID3D11VideoProcessorOutputView> ov[POOL];
    td.Width = dstW;
    td.Height = dstH;
    td.Format = DXGI_FORMAT_NV12;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{D3D11_VPOV_DIMENSION_TEXTURE2D, {}};
    for (int i = 0; i < POOL; i++)
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &nv12[i])) || FAILED(vd->CreateVideoProcessorOutputView(nv12[i].Get(), ven.Get(), &ovd, &ov[i])))
            return "cannot create video frames";

    // Find a working encoder
    ComPtr<IMFTransform> mft;
    ComPtr<IMFActivate> act;
    std::string err;
    auto fail = [&](const char* e) { err = e; return false; };
    auto setup = [&](ID3D11Device* d) -> bool {
        ComPtr<IMFAttributes> ma;
        if (SUCCEEDED(mft->GetAttributes(&ma))) { ma->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE); ma->SetUINT32(MF_LOW_LATENCY, TRUE); }
        UINT token;
        ComPtr<IMFDXGIDeviceManager> dm;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &dm)) || FAILED(dm->ResetDevice(d, token)) ||
            FAILED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dm.Get())))
            return fail("encoder does not accept GPU frames");
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft.As(&api))) {
            VARIANT v;
            v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR; api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.vt = VT_UI4; v.ulVal = (ULONG)mbps * 1000000; api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            v.vt = VT_UI4; v.ulVal = (ULONG)fps * 2; api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
            v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            v.vt = VT_UI4; v.ulVal = 0; api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
            if (lowLatency) {
                v.vt = VT_UI4; v.ulVal = (ULONG)((long long)mbps * 1000000 / fps); api->SetValue(&CODECAPI_AVEncCommonBufferSize, &v);
            }
        }
        ComPtr<IMFMediaType> ot, it;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)mbps * 1000000);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, dstW, dstH);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (FAILED(mft->SetOutputType(0, ot.Get(), 0))) return fail("encoder rejected the output format");
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, dstW, dstH);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(mft->SetInputType(0, it.Get(), 0))) return fail("encoder rejected NV12 input");
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    };
    auto tryEncoders = [&](ID3D11Device* d, LUID luid) {
        ComPtr<IMFAttributes> ea;
        MFCreateAttributes(&ea, 1);
        ea->SetUINT64(MFT_ENUM_ADAPTER_LUID, ((UINT64)(UINT32)luid.HighPart << 32) | luid.LowPart);
        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 nActs = 0;
        MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outInfo, ea.Get(), &acts, &nActs);
        for (UINT32 i = 0; i < nActs; i++) {
            if (!mft) {
                if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)))) err = "no working hardware H.264 encoder";
                else if (setup(d)) act = acts[i];
                else { mft.Reset(); acts[i]->ShutdownObject(); }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        if (mft) err.clear();
        return mft != nullptr;
    };
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    ComPtr<ID3D11Device> encDev = dev;
    ComPtr<ID3D11DeviceContext> encCtx = ctx;
    if (!tryEncoders(dev.Get(), ad.AdapterLuid)) {
        ComPtr<IDXGIFactory1> f;
        ComPtr<IDXGIAdapter1> a;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
            for (UINT i = 0; !mft && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d;
                a->GetDesc1(&d);
                if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (d.AdapterLuid.LowPart == ad.AdapterLuid.LowPart && d.AdapterLuid.HighPart == ad.AdapterLuid.HighPart)) continue;
                encDev.Reset();
                encCtx.Reset();
                if (FAILED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2, D3D11_SDK_VERSION, &encDev, nullptr, &encCtx))) continue;
                ComPtr<ID3D11Multithread> m2;
                if (SUCCEEDED(encCtx.As(&m2))) m2->SetMultithreadProtected(TRUE);
                tryEncoders(encDev.Get(), d.AdapterLuid);
            }
        if (!mft) return err.empty() ? "no hardware H.264 encoder" : err;
    }
    ComPtr<IMFMediaEventGenerator> gen;
    if (FAILED(mft.As(&gen))) {
        if (act) act->ShutdownObject();
        return "encoder is not asynchronous";
    }
    bool cross = encDev != dev;
    ComPtr<ID3D11Texture2D> readTex, writeTex, encTex[POOL];
    if (cross) {
        D3D11_TEXTURE2D_DESC sd = td;
        sd.BindFlags = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &readTex));
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = ok && SUCCEEDED(encDev->CreateTexture2D(&sd, nullptr, &writeTex));
        for (int i = 0; ok && i < POOL; i++) ok = SUCCEEDED(encDev->CreateTexture2D(&td, nullptr, &encTex[i]));
        if (!ok) { act->ShutdownObject(); return "cannot create video frames on the encoding GPU"; }
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    auto norm = [](long long v, long long size) { return (int)std::clamp(v * 65535 / std::max(1LL, size - 1), 0LL, 65535LL); };
    out.text('H', std::to_string(dstW) + " " + std::to_string(dstH) + " " + std::to_string(fps) + " " + std::to_string(norm(mon.left - vx, vw)) + " " +
                      std::to_string(norm(mon.top - vy, vh)) + " " + std::to_string(norm(srcW, vw)) + " " + std::to_string(norm(srcH, vh)) + " " +
                      std::to_string(monitor) + " " + std::to_string(monitors));

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    long long next = now.QuadPart, step = freq.QuadPart / fps;
    LONGLONG pts = 0, dur = 10000000LL / fps;
    int slot = 0;
    bool have = false;
    std::vector<std::pair<LONGLONG, LONGLONG>> inFlight;
    double encSum = 0;
    int encN = 0;
    while (!stopped() && !out.dead && err.empty()) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(gen->GetEvent(0, &ev))) { err = "encoder stopped"; break; }
        MediaEventType type;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            QueryPerformanceCounter(&now);
            if (next > now.QuadPart) {
                LARGE_INTEGER due;
                due.QuadPart = -(next - now.QuadPart) * 10000000 / freq.QuadPart;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 1000);
            } else if (now.QuadPart - next > step * 4) next = now.QuadPart;
            next += step;
            DXGI_OUTDUPL_FRAME_INFO fi;
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(have ? 0 : 500, &fi, &res);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D11Texture2D> t;
                if (SUCCEEDED(res.As(&t))) { ctx->CopyResource(last.Get(), t.Get()); have = true; }
                dup->ReleaseFrame();
            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                dup.Reset();
                if (FAILED(output->DuplicateOutput(dev.Get(), &dup))) {
                    Sleep(250);
                    output->DuplicateOutput(dev.Get(), &dup);
                }
                if (!dup) { err = "screen capture lost (locked, or the display changed)"; break; }
            }
            CURSORINFO ci{sizeof ci};
            if (GetCursorInfo(&ci))
                out.text('C', std::to_string(norm(ci.ptScreenPos.x - mon.left, srcW)) + " " + std::to_string(norm(ci.ptScreenPos.y - mon.top, srcH)) + " " +
                                  ((ci.flags & CURSOR_SHOWING) ? "1" : "0"));
            D3D11_VIDEO_PROCESSOR_STREAM st{};
            st.Enable = TRUE;
            st.pInputSurface = iv.Get();
            vc->VideoProcessorBlt(vp.Get(), ov[slot].Get(), 0, 1, &st);
            if (cross) {
                D3D11_MAPPED_SUBRESOURCE r, w;
                ctx->CopyResource(readTex.Get(), nv12[slot].Get());
                if (SUCCEEDED(ctx->Map(readTex.Get(), 0, D3D11_MAP_READ, 0, &r))) {
                    if (SUCCEEDED(encCtx->Map(writeTex.Get(), 0, D3D11_MAP_WRITE, 0, &w))) {
                        for (int y = 0; y < dstH * 3 / 2; y++)
                            memcpy((BYTE*)w.pData + (size_t)y * w.RowPitch, (BYTE*)r.pData + (size_t)y * r.RowPitch, dstW);
                        encCtx->Unmap(writeTex.Get(), 0);
                        encCtx->CopyResource(encTex[slot].Get(), writeTex.Get());
                    }
                    ctx->Unmap(readTex.Get(), 0);
                }
            }
            ComPtr<IMFMediaBuffer> buf;
            ComPtr<IMFSample> sample;
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), (cross ? encTex : nv12)[slot].Get(), 0, FALSE, &buf)) || FAILED(MFCreateSample(&sample))) { err = "out of memory"; break; }
            ComPtr<IMF2DBuffer> b2;
            DWORD len = 0;
            if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
            sample->AddBuffer(buf.Get());
            QueryPerformanceCounter(&now);
            inFlight.push_back({pts, now.QuadPart});
            if (inFlight.size() > 64) inFlight.erase(inFlight.begin());
            sample->SetSampleTime(pts);
            sample->SetSampleDuration(dur);
            pts += dur;
            slot = (slot + 1) % POOL;
            if (FAILED(mft->ProcessInput(0, sample.Get(), 0))) { err = "encoder refused a frame"; break; }
        } else if (type == METransformHaveOutput) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) mft->SetOutputType(0, t.Get(), 0);
            } else if (SUCCEEDED(hr) && ob.pSample) {
                ComPtr<IMFMediaBuffer> b;
                BYTE* p;
                DWORD n;
                if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&b)) && SUCCEEDED(b->Lock(&p, nullptr, &n))) {
                    out.send('V', p, n);
                    b->Unlock();
                }
                LONGLONG t = 0;
                ob.pSample->GetSampleTime(&t);
                auto it = std::find_if(inFlight.begin(), inFlight.end(), [&](auto& f) { return f.first == t; });
                if (it != inFlight.end()) {
                    QueryPerformanceCounter(&now);
                    encSum += (now.QuadPart - it->second) * 1000.0 / freq.QuadPart;
                    inFlight.erase(inFlight.begin(), it + 1);
                    if (++encN == fps) { out.text('L', std::to_string(std::lround(encSum / encN))); encSum = 0; encN = 0; }
                }
            }
            if (ob.pSample) ob.pSample->Release();
            if (ob.pEvents) ob.pEvents->Release();
        }
    }
    CloseHandle(timer);
    // Drain first or NVENC crashes
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)))
        for (ULONGLONG end = GetTickCount64() + 2000; GetTickCount64() < end;) {
            ComPtr<IMFMediaEvent> ev;
            HRESULT hr = gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) { Sleep(5); continue; }
            MediaEventType type;
            if (FAILED(hr) || FAILED(ev->GetType(&type)) || type == METransformDrainComplete) break;
            if (type == METransformHaveOutput) {
                MFT_OUTPUT_DATA_BUFFER ob{};
                DWORD status = 0;
                mft->ProcessOutput(0, 1, &ob, &status);
                if (ob.pSample) ob.pSample->Release();
                if (ob.pEvents) ob.pEvents->Release();
            }
        }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    ComPtr<IMFShutdown> sd;
    if (SUCCEEDED(mft.As(&sd))) sd->Shutdown();
    if (act) act->ShutdownObject();
    return err;
}

} // namespace

void streamRun(HANDLE pipe, int maxH, int fps, int mbps, int monitor, bool lowLatency, std::function<bool()> stopped) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    Out out;
    out.h = pipe;
    std::thread audio([&] { audioLoop(out, stopped); });
    std::string err = videoLoop(out, maxH, fps, mbps, monitor, lowLatency, stopped);
    if (!err.empty()) out.text('E', err);
    out.dead = true;
    audio.join();
    MFShutdown();
    CoUninitialize();
}
