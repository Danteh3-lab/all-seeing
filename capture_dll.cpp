#include <windows.h>
#define INITGUID
#include <dshow.h>
#include <gdiplus.h>
#include <string>
#include "capture_shared.h"

static const CLSID CLSID_SampleGrabber = {0xC1F400A0,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};
static const CLSID CLSID_NullRenderer = {0xC1F400A4,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};
static const IID IID_ISampleGrabber = {0x6B652FFF,0x11FE,0x4fce,{0x92,0xAD,0x02,0x66,0xB5,0xD7,0xC7,0x8F}};
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSample(IMediaSample**) = 0;
};

static std::wstring ToWide(const char* s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (len <= 0) return L"";
    std::wstring buf((size_t)len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &buf[0], len);
    return buf;
}

static int GetEncoderClsid(const wchar_t* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)malloc(size);
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; free(codecs); return i; }
    }
    free(codecs);
    return -1;
}

static bool CaptureWebcamFrame(const char* outputPath) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    bool result = false;
    IGraphBuilder* pGraph = NULL;
    ICaptureGraphBuilder2* pBuilder = NULL;
    IMediaControl* pControl = NULL;
    IBaseFilter* pCapFilter = NULL;
    IBaseFilter* pNullFilter = NULL;
    IBaseFilter* pGrabberBase = NULL;
    ISampleGrabber* pGrabber = NULL;
    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, NULL);
    do {
        CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&pGraph);
        CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&pBuilder);
        if (!pGraph || !pBuilder) break;
        pBuilder->SetFiltergraph(pGraph);
        ICreateDevEnum* pDevEnum = NULL;
        IEnumMoniker* pEnum = NULL;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&pDevEnum))) break;
        hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
        pDevEnum->Release();
        if (FAILED(hr) || !pEnum) break;
        IMoniker* pMoniker = NULL;
        if (pEnum->Next(1, &pMoniker, NULL) != S_OK) { pEnum->Release(); break; }
        pEnum->Release();
        if (FAILED(pMoniker->BindToObject(0, 0, IID_IBaseFilter, (void**)&pCapFilter))) { pMoniker->Release(); break; }
        pMoniker->Release();
        pGraph->AddFilter(pCapFilter, L"Capture");
        CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, IID_ISampleGrabber, (void**)&pGrabber);
        if (!pGrabber) break;
        if (FAILED(pGrabber->QueryInterface(IID_IBaseFilter, (void**)&pGrabberBase))) break;
        AM_MEDIA_TYPE mt;
        ZeroMemory(&mt, sizeof(mt));
        mt.majortype = MEDIATYPE_Video;
        mt.subtype = MEDIASUBTYPE_RGB24;
        pGrabber->SetMediaType(&mt);
        pGraph->AddFilter(pGrabberBase, L"Grabber");
        CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&pNullFilter);
        if (pNullFilter) pGraph->AddFilter(pNullFilter, L"Null");
        if (FAILED(pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, pCapFilter, pGrabberBase, pNullFilter))) break;
        pGrabber->SetOneShot(TRUE);
        pGrabber->SetBufferSamples(TRUE);
        if (FAILED(pGraph->QueryInterface(IID_IMediaControl, (void**)&pControl))) break;
        if (FAILED(pControl->Run())) break;
        DWORD waitStart = GetTickCount();
        bool gotSample = false;
        while (GetTickCount() - waitStart < 5000) {
            OAFilterState state;
            pControl->GetState(10, &state);
            if (state == State_Running) { Sleep(300); long cb = 0; if (pGrabber->GetCurrentBuffer(&cb, NULL) == S_OK) { gotSample = true; break; } }
            Sleep(100);
        }
        if (!gotSample) break;
        long cbBuffer = 0;
        if (FAILED(pGrabber->GetCurrentBuffer(&cbBuffer, NULL))) break;
        BYTE* pBuffer = new BYTE[cbBuffer];
        if (FAILED(pGrabber->GetCurrentBuffer(&cbBuffer, (long*)pBuffer))) { delete[] pBuffer; break; }
        AM_MEDIA_TYPE actualMt;
        ZeroMemory(&actualMt, sizeof(actualMt));
        if (FAILED(pGrabber->GetConnectedMediaType(&actualMt))) { delete[] pBuffer; break; }
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)actualMt.pbFormat;
        if (actualMt.formattype == FORMAT_VideoInfo && actualMt.cbFormat >= sizeof(VIDEOINFOHEADER) && vih) {
            LONG w = vih->bmiHeader.biWidth;
            LONG h = abs(vih->bmiHeader.biHeight);
            BITMAPINFO bmi;
            ZeroMemory(&bmi, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            HDC hdc = GetDC(NULL);
            HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, NULL, NULL, 0);
            if (hBmp) {
                SetBitmapBits(hBmp, cbBuffer, pBuffer);
                Gdiplus::Bitmap bitmap(hBmp, NULL);
                CLSID clsid;
                if (GetEncoderClsid(L"image/jpeg", &clsid) >= 0) {
                    Gdiplus::EncoderParameters eps;
                    eps.Count = 1;
                    eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
                    eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                    eps.Parameter[0].NumberOfValues = 1;
                    UINT q = 80;
                    eps.Parameter[0].Value = &q;
                    result = (bitmap.Save(ToWide(outputPath).c_str(), &clsid, &eps) == Gdiplus::Ok);
                }
                DeleteObject(hBmp);
            }
            ReleaseDC(NULL, hdc);
        }
        if (actualMt.pbFormat) CoTaskMemFree(actualMt.pbFormat);
        delete[] pBuffer;
    } while (false);
    if (pControl) { pControl->Stop(); pControl->Release(); }
    if (pGrabberBase) pGrabberBase->Release();
    if (pGrabber) pGrabber->Release();
    if (pNullFilter) pNullFilter->Release();
    if (pCapFilter) pCapFilter->Release();
    if (pBuilder) pBuilder->Release();
    if (pGraph) pGraph->Release();
    Gdiplus::GdiplusShutdown(gdiToken);
    CoUninitialize();
    return result;
}

extern "C" __declspec(dllexport) DWORD WINAPI CaptureThread(LPVOID lpParam) {
    CaptureParams* params = (CaptureParams*)lpParam;
    if (!params || !params->outputPath[0]) return 1;
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, params->outputPath, -1, narrowPath, MAX_PATH, NULL, NULL);
    return CaptureWebcamFrame(narrowPath) ? 0 : 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
