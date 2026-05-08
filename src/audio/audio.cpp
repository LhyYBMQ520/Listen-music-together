#define _WIN32_WINNT 0x0A00
#define NOMINMAX

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <conio.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#define SAFE_RELEASE(p) if ((p) != nullptr) { (p)->Release(); (p) = nullptr; }

struct AudioSessionInfo {
    DWORD processId = 0;
    std::wstring processName;
    std::wstring sessionName;
    std::wstring sessionId;
    AudioSessionState state = AudioSessionStateInactive;
    float volume = 0.0f;
    bool muted = false;
};

std::atomic<bool> g_ctrlCPressed{ false };

BOOL WINAPI ConsoleCtrlHandler(DWORD signalType) {
    if (signalType == CTRL_C_EVENT) {
        g_ctrlCPressed.store(true);
        return TRUE;
    }
    return FALSE;
}

std::wstring Trim(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]) != 0) {
        ++first;
    }

    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

std::wstring FormatHresult(HRESULT hr) {
    std::wostringstream out;
    out << L"0x" << std::hex << std::uppercase << static_cast<unsigned long long>(hr);
    return out.str();
}

std::wstring FormatHresultMessage(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD langId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    const DWORD size = FormatMessageW(flags, nullptr, hr, langId, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0 || !buffer) {
        return L"";
    }

    std::wstring message(buffer, size);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

DWORD GetWindowsBuildNumber() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return 0;
    }

    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) {
        return 0;
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) {
        return 0;
    }

    return info.dwBuildNumber;
}

std::wstring NormalizeWavPath(const std::wstring& input, std::wstring& errorMessage) {
    errorMessage.clear();

    std::wstring trimmed = Trim(input);
    if (trimmed.empty()) {
        return trimmed;
    }

    std::filesystem::path path(trimmed);
    std::error_code ec;

    if (std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec)) {
        path /= L"capture.wav";
    }

    if (!path.has_extension()) {
        path += L".wav";
    }

    if (!path.parent_path().empty() && !std::filesystem::exists(path.parent_path(), ec)) {
        errorMessage = L"Output folder does not exist.";
        return L"";
    }

    return path.wstring();
}

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };

    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }

    std::wostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << L" " << units[unit];
    return out.str();
}

void ClearConsole() {
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (console == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(console, &csbi)) {
        return;
    }

    const DWORD cellCount = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
    DWORD written = 0;
    const COORD home = { 0, 0 };

    FillConsoleOutputCharacterW(console, L' ', cellCount, home, &written);
    FillConsoleOutputAttribute(console, csbi.wAttributes, cellCount, home, &written);
    SetConsoleCursorPosition(console, home);
}

std::wstring GetSessionStateDescription(AudioSessionState state) {
    switch (state) {
    case AudioSessionStateInactive:
        return L"Inactive";
    case AudioSessionStateActive:
        return L"Active";
    case AudioSessionStateExpired:
        return L"Expired";
    default:
        return L"Unknown";
    }
}

std::wstring GetProcessName(DWORD pid) {
    if (pid == 0) {
        return L"System Audio";
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return L"Unknown Process";
    }

    wchar_t path[MAX_PATH] = { 0 };
    DWORD pathSize = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &pathSize) != 0) {
        wchar_t* fileName = wcsrchr(path, L'\\');
        CloseHandle(process);
        if (fileName != nullptr) {
            return fileName + 1;
        }
        return path;
    }

    CloseHandle(process);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return L"Unknown Process";
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            if (entry.th32ProcessID == pid) {
                CloseHandle(snapshot);
                return entry.szExeFile;
            }
        } while (Process32NextW(snapshot, &entry) != 0);
    }

    CloseHandle(snapshot);
    return L"Unknown Process";
}

std::wstring GetDeviceName(IMMDevice* device) {
    if (!device) {
        return L"Unknown Device";
    }

    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr) || !props) {
        return L"Device Properties Unavailable";
    }

    PROPVARIANT name;
    PropVariantInit(&name);
    hr = props->GetValue(PKEY_Device_FriendlyName, &name);

    std::wstring result = (SUCCEEDED(hr) && name.pwszVal) ? name.pwszVal : L"Unnamed Device";
    PropVariantClear(&name);
    SAFE_RELEASE(props);
    return result;
}

HRESULT GetDefaultRenderMixFormat(WAVEFORMATEX** mixFormat, std::wstring& errorMessage) {
    if (!mixFormat) {
        return E_POINTER;
    }
    *mixFormat = nullptr;
    errorMessage.clear();

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        errorMessage = L"Failed to create MMDeviceEnumerator for mix format.";
        goto Cleanup;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        errorMessage = L"Failed to get default render endpoint for mix format.";
        if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        goto Cleanup;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        errorMessage = L"Failed to activate IAudioClient for mix format.";
        if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        goto Cleanup;
    }

    hr = audioClient->GetMixFormat(mixFormat);
    if (FAILED(hr) || !*mixFormat) {
        errorMessage = L"Failed to query default render mix format.";
        if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        goto Cleanup;
    }

Cleanup:
    SAFE_RELEASE(audioClient);
    SAFE_RELEASE(device);
    SAFE_RELEASE(enumerator);
    return hr;
}

bool EnumerateAudioSessions(std::vector<AudioSessionInfo>& sessions, std::wstring& deviceName, std::wstring& errorMessage) {
    sessions.clear();
    deviceName.clear();
    errorMessage.clear();

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* sessionManager = nullptr;
    IAudioSessionEnumerator* sessionEnumerator = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        errorMessage = L"Failed to create MMDeviceEnumerator.";
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        errorMessage = L"Failed to get default render endpoint.";
        SAFE_RELEASE(enumerator);
        return false;
    }

    deviceName = GetDeviceName(device);

    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessionManager));
    if (FAILED(hr) || !sessionManager) {
        errorMessage = L"Failed to activate IAudioSessionManager2.";
        SAFE_RELEASE(device);
        SAFE_RELEASE(enumerator);
        return false;
    }

    hr = sessionManager->GetSessionEnumerator(&sessionEnumerator);
    if (FAILED(hr) || !sessionEnumerator) {
        errorMessage = L"Failed to get IAudioSessionEnumerator.";
        SAFE_RELEASE(sessionManager);
        SAFE_RELEASE(device);
        SAFE_RELEASE(enumerator);
        return false;
    }

    int count = 0;
    hr = sessionEnumerator->GetCount(&count);
    if (FAILED(hr)) {
        errorMessage = L"Failed to query audio session count.";
        SAFE_RELEASE(sessionEnumerator);
        SAFE_RELEASE(sessionManager);
        SAFE_RELEASE(device);
        SAFE_RELEASE(enumerator);
        return false;
    }

    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        hr = sessionEnumerator->GetSession(i, &control);
        if (FAILED(hr) || !control) {
            continue;
        }

        IAudioSessionControl2* control2 = nullptr;
        hr = control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2));
        if (FAILED(hr) || !control2) {
            SAFE_RELEASE(control);
            continue;
        }

        AudioSessionInfo info;
        control2->GetProcessId(&info.processId);
        info.processName = GetProcessName(info.processId);

        wchar_t* displayName = nullptr;
        control2->GetDisplayName(&displayName);
        info.sessionName = (displayName && displayName[0] != L'\0') ? displayName : L"(No Display Name)";
        CoTaskMemFree(displayName);

        wchar_t* sessionId = nullptr;
        control2->GetSessionIdentifier(&sessionId);
        info.sessionId = sessionId ? sessionId : L"(No Session ID)";
        CoTaskMemFree(sessionId);

        control->GetState(&info.state);

        ISimpleAudioVolume* volume = nullptr;
        hr = control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume));
        if (SUCCEEDED(hr) && volume) {
            float masterVolume = 0.0f;
            BOOL muted = FALSE;
            volume->GetMasterVolume(&masterVolume);
            volume->GetMute(&muted);
            info.volume = masterVolume;
            info.muted = (muted != FALSE);
            SAFE_RELEASE(volume);
        }

        sessions.push_back(std::move(info));

        SAFE_RELEASE(control2);
        SAFE_RELEASE(control);
    }

    SAFE_RELEASE(sessionEnumerator);
    SAFE_RELEASE(sessionManager);
    SAFE_RELEASE(device);
    SAFE_RELEASE(enumerator);
    return true;
}

std::string WstrToUtf8(const std::wstring& wstr);
void WriteFrame(uint8_t type, const void* payload, uint32_t size);
void WriteTextFrame(const std::string& text);
void WriteAudioFormatFrame(const WAVEFORMATEX* fmt);
void WriteAudioChunkFrame(uint64_t timestampUs, const uint8_t* data, uint32_t size);
void WriteStdoutLine(const std::string& line);
void WriteStdoutBuf(const void* data, DWORD size);
std::string ReadStdinLine();

class WavWriter {
public:
    bool Open(const std::wstring& path, const WAVEFORMATEX* format, std::wstring& errorMessage) {
        if (path.empty()) {
            enabled_ = false;
            return true;
        }

        if (!format) {
            errorMessage = L"Invalid format. WAV open failed.";
            return false;
        }

        const std::filesystem::path filePath(path);
        file_.open(filePath, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            errorMessage = L"Cannot open WAV output file: " + path;
            return false;
        }

        formatChunkSize_ = static_cast<uint32_t>(sizeof(WAVEFORMATEX) + format->cbSize);

        WriteTag("RIFF");
        riffSizePos_ = file_.tellp();
        WriteU32(0);
        WriteTag("WAVE");

        WriteTag("fmt ");
        WriteU32(formatChunkSize_);
        file_.write(reinterpret_cast<const char*>(format), formatChunkSize_);

        WriteTag("data");
        dataSizePos_ = file_.tellp();
        WriteU32(0);

        enabled_ = true;
        dataSize_ = 0;
        return true;
    }

    void WriteData(const BYTE* data, uint32_t size) {
        if (!enabled_ || !data || size == 0) {
            return;
        }

        file_.write(reinterpret_cast<const char*>(data), size);
        dataSize_ += size;
    }

    void Finalize() {
        if (!enabled_) {
            return;
        }

        const uint64_t riffSize64 = 4ull + (8ull + static_cast<uint64_t>(formatChunkSize_)) + (8ull + dataSize_);
        const uint32_t riffSize = riffSize64 > (std::numeric_limits<uint32_t>::max)()
            ? (std::numeric_limits<uint32_t>::max)()
            : static_cast<uint32_t>(riffSize64);
        const uint32_t dataSize = dataSize_ > (std::numeric_limits<uint32_t>::max)()
            ? (std::numeric_limits<uint32_t>::max)()
            : static_cast<uint32_t>(dataSize_);

        file_.seekp(riffSizePos_);
        WriteU32(riffSize);

        file_.seekp(dataSizePos_);
        WriteU32(dataSize);

        file_.close();
        enabled_ = false;
    }

    ~WavWriter() {
        Finalize();
    }

    bool Enabled() const {
        return enabled_;
    }

private:
    void WriteTag(const char tag[4]) {
        file_.write(tag, 4);
    }

    void WriteU32(uint32_t value) {
        file_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

private:
    std::ofstream file_;
    bool enabled_ = false;
    uint64_t dataSize_ = 0;
    uint32_t formatChunkSize_ = 0;
    std::streampos riffSizePos_{};
    std::streampos dataSizePos_{};
};

class ActivateAudioInterfaceCompletionHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateAudioInterfaceCompletionHandler()
        : refCount_(1), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), activateResult_(E_PENDING), activatedInterface_(nullptr) {
    }

    ~ActivateAudioInterfaceCompletionHandler() {
        if (event_ != nullptr) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        SAFE_RELEASE(activatedInterface_);
    }

    HRESULT WaitForClient(IAudioClient** client) {
        if (!client) {
            return E_POINTER;
        }
        *client = nullptr;

        if (!event_) {
            return E_FAIL;
        }

        const DWORD waitResult = WaitForSingleObject(event_, 5000);
        if (waitResult != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }

        if (FAILED(activateResult_)) {
            return activateResult_;
        }

        if (!activatedInterface_) {
            return E_FAIL;
        }

        return activatedInterface_->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client));
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (!operation) {
            activateResult_ = E_POINTER;
            if (event_) {
                SetEvent(event_);
            }
            return S_OK;
        }

        HRESULT activationHr = E_FAIL;
        IUnknown* activated = nullptr;
        const HRESULT opHr = operation->GetActivateResult(&activationHr, &activated);

        if (SUCCEEDED(opHr) && SUCCEEDED(activationHr) && activated) {
            activatedInterface_ = activated;
            activateResult_ = S_OK;
        }
        else {
            if (activated) {
                activated->Release();
            }
            activateResult_ = FAILED(opHr) ? opHr : activationHr;
        }

        if (event_) {
            SetEvent(event_);
        }

        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }

        if (riid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG value = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

private:
    LONG refCount_;
    HANDLE event_;
    HRESULT activateResult_;
    IUnknown* activatedInterface_;
};

HRESULT ActivateProcessLoopbackAudioClient(DWORD processId, IAudioClient** audioClient) {
    if (!audioClient) {
        return E_POINTER;
    }
    *audioClient = nullptr;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = processId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    BYTE* blobData = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(params)));
    if (!blobData) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(blobData, &params, sizeof(params));

    PROPVARIANT activationParams;
    PropVariantInit(&activationParams);
    activationParams.vt = VT_BLOB;
    activationParams.blob.cbSize = sizeof(params);
    activationParams.blob.pBlobData = blobData;

    auto* completion = new (std::nothrow) ActivateAudioInterfaceCompletionHandler();
    if (!completion) {
        if (activationParams.vt == VT_BLOB && activationParams.blob.pBlobData) {
            CoTaskMemFree(activationParams.blob.pBlobData);
            activationParams.blob.pBlobData = nullptr;
        }
        return E_OUTOFMEMORY;
    }

    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activationParams,
        completion,
        &operation);

    if (SUCCEEDED(hr)) {
        hr = completion->WaitForClient(audioClient);
    }

    SAFE_RELEASE(operation);
    if (activationParams.vt == VT_BLOB && activationParams.blob.pBlobData) {
        CoTaskMemFree(activationParams.blob.pBlobData);
        activationParams.blob.pBlobData = nullptr;
    }
    completion->Release();
    return hr;
}

class SessionCapturer {
public:
    bool Start(const AudioSessionInfo& session, const std::wstring& wavPath, bool streamToStdout = false) {
        if (running_.load()) {
            return false;
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        stopRequested_.store(false);
        running_.store(true);
        capturedBytes_.store(0);
        SetLastMessage(L"Starting capture...");

        worker_ = std::thread(&SessionCapturer::CaptureWorker, this, session, wavPath, streamToStdout);
        return true;
    }

    void RequestStop() {
        stopRequested_.store(true);
    }

    bool IsRunning() const {
        return running_.load();
    }

    uint64_t GetCapturedBytes() const {
        return capturedBytes_.load();
    }

    std::wstring GetLastMessage() const {
        std::lock_guard<std::mutex> lock(messageMutex_);
        return lastMessage_;
    }

    void Wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ~SessionCapturer() {
        RequestStop();
        Wait();
    }

private:
    void SetLastMessage(const std::wstring& message) {
        std::lock_guard<std::mutex> lock(messageMutex_);
        lastMessage_ = message;
    }

    void CaptureWorker(AudioSessionInfo session, std::wstring wavPath, bool streamToStdout) {
        bool failed = false;
        bool wroteFile = false;
        HRESULT hr = S_OK;

        const DWORD loopbackFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool shouldCoUninit = SUCCEEDED(comHr);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            SetLastMessage(L"Capture thread COM init failed.");
            running_.store(false);
            return;
        }

        IAudioClient* audioClient = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        WAVEFORMATEX* mixFormat = nullptr;
        WavWriter writer;
        std::vector<BYTE> silenceBuffer;

        const DWORD buildNumber = GetWindowsBuildNumber();
        if (buildNumber != 0 && buildNumber < 19041) {
            SetLastMessage(L"Process loopback requires Windows 10 2004+ (build >= 19041). Current build: " + std::to_wstring(buildNumber));
            failed = true;
            goto Cleanup;
        }

        hr = ActivateProcessLoopbackAudioClient(session.processId, &audioClient);
        if (FAILED(hr) || !audioClient) {
            std::wstring message = L"Capture start failed: process loopback activation failed (" + FormatHresult(hr) + L").";
            const std::wstring systemMessage = FormatHresultMessage(hr);
            if (!systemMessage.empty()) {
                message += L" " + systemMessage;
            }
            SetLastMessage(message);
            failed = true;
            goto Cleanup;
        }

        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            if (SUCCEEDED(hr)) {
                hr = E_FAIL;
            }

            std::wstring message = L"Capture start failed: cannot get mix format (" + FormatHresult(hr) + L").";
            const std::wstring systemMessage = FormatHresultMessage(hr);
            if (!systemMessage.empty()) {
                message += L" " + systemMessage;
            }

            std::wstring fallbackError;
            WAVEFORMATEX* fallback = nullptr;
            HRESULT fallbackHr = GetDefaultRenderMixFormat(&fallback, fallbackError);
            if (SUCCEEDED(fallbackHr) && fallback) {
                WAVEFORMATEX* closest = nullptr;
                HRESULT supportHr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, fallback, &closest);
                if (supportHr == S_FALSE && closest) {
                    CoTaskMemFree(fallback);
                    fallback = closest;
                    closest = nullptr;
                }
                else if (FAILED(supportHr) && supportHr != E_NOTIMPL) {
                    if (closest) {
                        CoTaskMemFree(closest);
                    }
                    CoTaskMemFree(fallback);
                    SetLastMessage(message + L" Fallback format unsupported (" + FormatHresult(supportHr) + L").");
                    failed = true;
                    goto Cleanup;
                }

                mixFormat = fallback;
            }
            else {
                if (!fallbackError.empty()) {
                    message += L" " + fallbackError;
                }
                SetLastMessage(message);
                failed = true;
                goto Cleanup;
            }
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, loopbackFlags, 0, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            const HRESULT loopbackHr = hr;
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
            if (FAILED(hr)) {
                std::wstring message = L"Capture start failed: IAudioClient::Initialize failed (loopback="
                    + FormatHresult(loopbackHr) + L", default=" + FormatHresult(hr) + L").";
                const std::wstring systemMessage = FormatHresultMessage(hr);
                if (!systemMessage.empty()) {
                    message += L" " + systemMessage;
                }
                SetLastMessage(message);
                failed = true;
                goto Cleanup;
            }
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr) || !captureClient) {
            SetLastMessage(L"Capture start failed: cannot get IAudioCaptureClient.");
            failed = true;
            goto Cleanup;
        }

        {
            std::wstring wavError;
            if (!writer.Open(wavPath, mixFormat, wavError)) {
                SetLastMessage(L"Capture start failed: " + wavError);
                failed = true;
                goto Cleanup;
            }
            wroteFile = writer.Enabled();
        }

        if (streamToStdout) {
            WriteAudioFormatFrame(mixFormat);
            QueryPerformanceFrequency(&qpcFreq_);
            QueryPerformanceCounter(&captureStartQpc_);
            minChunkBytes_ = (mixFormat->nSamplesPerSec * mixFormat->nBlockAlign) / 10;
            chunkBuf_.clear();
            chunkFirstBuffer_ = true;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            SetLastMessage(L"Capture start failed: IAudioClient::Start failed.");
            failed = true;
            goto Cleanup;
        }

        SetLastMessage(L"Capturing: " + session.processName + L" (PID=" + std::to_wstring(session.processId) + L")");

        while (!stopRequested_.load()) {
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                SetLastMessage(L"Capture failed: GetNextPacketSize failed.");
                failed = true;
                break;
            }

            if (packetLength == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            while (packetLength > 0) {
                BYTE* data = nullptr;
                UINT32 frameCount = 0;
                DWORD flags = 0;

                hr = captureClient->GetBuffer(&data, &frameCount, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    SetLastMessage(L"Capture failed: GetBuffer failed.");
                    failed = true;
                    break;
                }

                const uint32_t bytesToWrite = frameCount * mixFormat->nBlockAlign;
                if (writer.Enabled() && bytesToWrite > 0) {
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                        if (silenceBuffer.size() < bytesToWrite) {
                            silenceBuffer.resize(bytesToWrite, 0);
                        }
                        writer.WriteData(silenceBuffer.data(), bytesToWrite);
                    }
                    else {
                        writer.WriteData(data, bytesToWrite);
                    }
                }

                if (streamToStdout && bytesToWrite > 0) {
                    if (chunkFirstBuffer_) {
                        chunkStartTimestamp_ = GetTimestampUs();
                        chunkFirstBuffer_ = false;
                    }
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                        if (silenceBuffer.size() < bytesToWrite) {
                            silenceBuffer.resize(bytesToWrite, 0);
                        }
                        chunkBuf_.insert(chunkBuf_.end(), silenceBuffer.begin(), silenceBuffer.begin() + bytesToWrite);
                    }
                    else {
                        chunkBuf_.insert(chunkBuf_.end(), data, data + bytesToWrite);
                    }
                    if (chunkBuf_.size() >= minChunkBytes_) {
                        FlushChunk();
                    }
                }

                capturedBytes_.fetch_add(bytesToWrite);

                hr = captureClient->ReleaseBuffer(frameCount);
                if (FAILED(hr)) {
                    SetLastMessage(L"Capture failed: ReleaseBuffer failed.");
                    failed = true;
                    break;
                }

                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    SetLastMessage(L"Capture failed: next packet query failed.");
                    failed = true;
                    break;
                }
            }

            if (failed) {
                break;
            }
        }

        if (streamToStdout) {
            FlushChunk();
        }

Cleanup:
        if (audioClient) {
            audioClient->Stop();
        }

        writer.Finalize();

        if (!failed) {
            std::wstring summary = L"Capture stopped. Total: " + FormatBytes(capturedBytes_.load());
            if (wroteFile) {
                summary += L", saved to: " + wavPath;
            }
            else {
                summary += L", no file output.";
            }
            SetLastMessage(summary);
        }

        if (mixFormat) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }

        SAFE_RELEASE(captureClient);
        SAFE_RELEASE(audioClient);

        if (shouldCoUninit) {
            CoUninitialize();
        }

        running_.store(false);
    }

    void FlushChunk() {
        if (!chunkBuf_.empty()) {
            WriteAudioChunkFrame(chunkStartTimestamp_, chunkBuf_.data(), static_cast<uint32_t>(chunkBuf_.size()));
            chunkBuf_.clear();
        }
        chunkFirstBuffer_ = true;
    }

    uint64_t GetTimestampUs() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return ((now.QuadPart - captureStartQpc_.QuadPart) * 1000000ULL) / qpcFreq_.QuadPart;
    }

private:
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<uint64_t> capturedBytes_{ 0 };
    mutable std::mutex messageMutex_;
    std::wstring lastMessage_;
    std::thread worker_;

    // Chunk batching for framed stdout output
    std::vector<BYTE> chunkBuf_;
    uint64_t chunkStartTimestamp_ = 0;
    LARGE_INTEGER captureStartQpc_ = {};
    LARGE_INTEGER qpcFreq_ = {};
    uint32_t minChunkBytes_ = 0;
    bool chunkFirstBuffer_ = true;
};

void RenderIdleView(
    const std::wstring& deviceName,
    const std::vector<AudioSessionInfo>& sessions,
    const std::wstring& inputBuffer,
    const std::wstring& message) {
    ClearConsole();

    std::wcout << L"Audio Session Capture (auto-refresh every 3s)\n";
    std::wcout << L"================================================\n";
    std::wcout << L"Default Device: " << (deviceName.empty() ? L"(unknown)" : deviceName) << L"\n\n";

    if (!message.empty()) {
        std::wcout << L"Status: " << message << L"\n\n";
    }

    if (sessions.empty()) {
        std::wcout << L"No visible audio sessions right now.\n\n";
    }
    else {
        std::wcout << L"Sessions:\n";
        for (size_t i = 0; i < sessions.size(); ++i) {
            const AudioSessionInfo& s = sessions[i];

            std::wcout
                << std::setw(2) << (i + 1) << L". "
                << L"[" << GetSessionStateDescription(s.state) << L"] "
                << L"PID=" << s.processId << L" | "
                << s.processName << L" | "
                << s.sessionName
                << L" | Vol " << std::fixed << std::setprecision(1) << (s.volume * 100.0f) << L"%";

            if (s.muted) {
                std::wcout << L" (Muted)";
            }
            if (s.processId == 0) {
                std::wcout << L" | Not capturable by PID";
            }

            std::wcout << L"\n";
        }
        std::wcout << L"\n";
    }

    std::wcout << L"Type session index then press Enter to start capture.\n";
    std::wcout << L"Idle controls: ESC / q / Q / Ctrl+C to exit.\n";
    std::wcout << L"Input: " << (inputBuffer.empty() ? L"(empty)" : inputBuffer) << std::flush;
}

void RenderCaptureView(
    const AudioSessionInfo& activeSession,
    const std::wstring& outputPath,
    const SessionCapturer& capturer) {
    ClearConsole();

    std::wcout << L"Capturing Audio\n";
    std::wcout << L"================================================\n";
    std::wcout << L"Target: " << activeSession.processName << L" | PID=" << activeSession.processId << L"\n";
    std::wcout << L"Session: " << activeSession.sessionName << L"\n";
    std::wcout << L"Output: " << (outputPath.empty() ? L"(none)" : outputPath) << L"\n";
    std::wcout << L"Captured: " << FormatBytes(capturer.GetCapturedBytes()) << L"\n";
    std::wcout << L"Status: " << capturer.GetLastMessage() << L"\n\n";
    std::wcout << L"Press ESC to stop capture..." << std::flush;
}

std::string WstrToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], len, nullptr, nullptr);
    return result;
}

void WriteFrame(uint8_t type, const void* payload, uint32_t size) {
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    // Header: 1 byte type + 3 bytes length (big-endian)
    uint8_t header[4];
    header[0] = type;
    header[1] = static_cast<uint8_t>((size >> 16) & 0xFF);
    header[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(size & 0xFF);
    DWORD written = 0;
    WriteFile(hStdout, header, 4, &written, nullptr);
    if (size > 0 && payload != nullptr) {
        WriteFile(hStdout, payload, size, &written, nullptr);
    }
}

void WriteTextFrame(const std::string& text) {
    WriteFrame(0x01, text.c_str(), static_cast<uint32_t>(text.size()));
}

void WriteAudioFormatFrame(const WAVEFORMATEX* fmt) {
    // Payload: [sampleRate:4 LE][channels:2 LE][bitsPerSample:2 LE][blockAlign:2 LE][formatTag:2 LE] = 12 bytes
    uint8_t payload[12];
    payload[0] = static_cast<uint8_t>(fmt->nSamplesPerSec & 0xFF);
    payload[1] = static_cast<uint8_t>((fmt->nSamplesPerSec >> 8) & 0xFF);
    payload[2] = static_cast<uint8_t>((fmt->nSamplesPerSec >> 16) & 0xFF);
    payload[3] = static_cast<uint8_t>((fmt->nSamplesPerSec >> 24) & 0xFF);
    payload[4] = static_cast<uint8_t>(fmt->nChannels & 0xFF);
    payload[5] = static_cast<uint8_t>((fmt->nChannels >> 8) & 0xFF);
    payload[6] = static_cast<uint8_t>(fmt->wBitsPerSample & 0xFF);
    payload[7] = static_cast<uint8_t>((fmt->wBitsPerSample >> 8) & 0xFF);
    payload[8] = static_cast<uint8_t>(fmt->nBlockAlign & 0xFF);
    payload[9] = static_cast<uint8_t>((fmt->nBlockAlign >> 8) & 0xFF);
    payload[10] = static_cast<uint8_t>(fmt->wFormatTag & 0xFF);
    payload[11] = static_cast<uint8_t>((fmt->wFormatTag >> 8) & 0xFF);
    WriteFrame(0x03, payload, 12);
}

void WriteAudioChunkFrame(uint64_t timestampUs, const uint8_t* data, uint32_t size) {
    // Payload: [timestamp:8 LE][PCM data:N]
    std::vector<uint8_t> payload;
    payload.reserve(8 + size);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<uint8_t>((timestampUs >> (i * 8)) & 0xFF));
    }
    payload.insert(payload.end(), data, data + size);
    WriteFrame(0x02, payload.data(), static_cast<uint32_t>(payload.size()));
}

// Legacy functions retained for interactive (non-slave) mode only.
void WriteStdoutLine(const std::string& line) {
    DWORD written = 0;
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(hStdout, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    const char nl = '\n';
    WriteFile(hStdout, &nl, 1, &written, nullptr);
}

void WriteStdoutBuf(const void* data, DWORD size) {
    DWORD written = 0;
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(hStdout, data, size, &written, nullptr);
}

std::string ReadStdinLine() {
    std::string result;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    char ch;
    DWORD read = 0;
    while (ReadFile(hStdin, &ch, 1, &read, nullptr) && read == 1) {
        if (ch == '\n') break;
        if (ch != '\r') result.push_back(ch);
    }
    return result;
}

int RunSlaveMode() {
    SessionCapturer capturer;
    std::vector<AudioSessionInfo> sessions;
    std::wstring deviceName;
    bool running = true;

    while (running) {
        std::wstring error;
        if (!EnumerateAudioSessions(sessions, deviceName, error)) {
            WriteTextFrame("ERROR|" + WstrToUtf8(error));
            continue;
        }

        WriteTextFrame("SESSIONS|" + std::to_string(sessions.size()));
        for (size_t i = 0; i < sessions.size(); ++i) {
            const auto& s = sessions[i];
            std::string line = "SESSION|";
            line += std::to_string(i + 1) + "|";
            line += std::to_string(s.processId) + "|";
            line += WstrToUtf8(s.processName) + "|";
            line += WstrToUtf8(GetSessionStateDescription(s.state)) + "|";
            line += std::to_string(static_cast<int>(s.volume * 100)) + "|";
            line += (s.muted ? "1" : "0");
            WriteTextFrame(line);
        }
        WriteTextFrame("READY");

        std::string input = ReadStdinLine();
        if (input.empty()) {
            break;
        }

        if (input == "REFRESH") {
            continue;
        }

        if (input == "EXIT" || input == "QUIT") {
            running = false;
            break;
        }

        int selectedIndex = 0;
        try {
            selectedIndex = std::stoi(input);
        } catch (...) {
            WriteTextFrame("ERROR|Invalid session index");
            continue;
        }

        if (selectedIndex < 1 || selectedIndex > static_cast<int>(sessions.size())) {
            WriteTextFrame("ERROR|Invalid session index");
            continue;
        }

        AudioSessionInfo selected = sessions[static_cast<size_t>(selectedIndex - 1)];
        if (selected.processId == 0) {
            WriteTextFrame("ERROR|Cannot capture system audio session");
            continue;
        }

        if (!capturer.Start(selected, L"", true)) {
            WriteTextFrame("ERROR|Failed to start capture");
            continue;
        }

        std::atomic<bool> stopCapture{false};
        std::thread inputThread([&]() {
            std::string cmd = ReadStdinLine();
            if (cmd == "STOP") {
                stopCapture.store(true);
            }
            if (cmd == "EXIT" || cmd == "QUIT") {
                stopCapture.store(true);
                running = false;
            }
        });

        while (capturer.IsRunning() && !stopCapture.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (capturer.IsRunning()) {
            capturer.RequestStop();
        }
        capturer.Wait();

        if (inputThread.joinable()) {
            inputThread.join();
        }

        if (running) {
            WriteTextFrame("STOPPED|" + std::to_string(capturer.GetCapturedBytes()));
        }
    }

    WriteFrame(0xFF, nullptr, 0);
    return 0;
}

bool IsStdinPipe() {
    DWORD mode;
    return !GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
}

int main() {
    bool slaveMode = IsStdinPipe();

    if (slaveMode) {
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool shouldCoUninit = SUCCEEDED(comHr);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            fprintf(stderr, "COM initialization failed. HRESULT=0x%llx\n",
                static_cast<unsigned long long>(comHr));
            return 1;
        }
        int result = RunSlaveMode();
        if (shouldCoUninit) {
            CoUninitialize();
        }
        return result;
    }

    setlocale(LC_ALL, "");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldCoUninit = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"COM initialization failed. HRESULT=0x"
            << std::hex << static_cast<unsigned long long>(comHr) << std::endl;
        return 1;
    }

    SessionCapturer capturer;
    std::vector<AudioSessionInfo> sessions;

    std::wstring deviceName;
    std::wstring message = L"Program started. Session list will refresh automatically.";
    std::wstring inputBuffer;

    bool captureMode = false;
    bool shouldExit = false;
    AudioSessionInfo activeSession;
    std::wstring activeOutputPath;

    const auto refreshInterval = std::chrono::seconds(3);
    auto nextRefreshAt = std::chrono::steady_clock::now();
    auto nextCaptureUiAt = std::chrono::steady_clock::now();

    while (!shouldExit) {
        if (g_ctrlCPressed.exchange(false)) {
            if (!captureMode) {
                shouldExit = true;
                break;
            }
        }

        if (captureMode && !capturer.IsRunning()) {
            capturer.Wait();
            captureMode = false;
            message = capturer.GetLastMessage();
            inputBuffer.clear();
            nextRefreshAt = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();

        if (!captureMode && now >= nextRefreshAt) {
            std::wstring error;
            if (!EnumerateAudioSessions(sessions, deviceName, error)) {
                message = error;
            }
            else if (message.find(L"Capture") == std::wstring::npos) {
                message = L"Session list refreshed.";
            }

            RenderIdleView(deviceName, sessions, inputBuffer, message);
            nextRefreshAt = now + refreshInterval;
        }

        if (captureMode && now >= nextCaptureUiAt) {
            RenderCaptureView(activeSession, activeOutputPath, capturer);
            nextCaptureUiAt = now + std::chrono::milliseconds(300);
        }

        if (_kbhit() != 0) {
            int key = _getch();
            if (key == 0 || key == 224) {
                (void)_getch();
                continue;
            }

            if (captureMode) {
                if (key == 27) {
                    capturer.RequestStop();
                    nextCaptureUiAt = std::chrono::steady_clock::now();
                }
                continue;
            }

            if (key == 3 || key == 27 || key == 'q' || key == 'Q') {
                shouldExit = true;
                break;
            }

            if (key == '\b') {
                if (!inputBuffer.empty()) {
                    inputBuffer.pop_back();
                }
                RenderIdleView(deviceName, sessions, inputBuffer, message);
                continue;
            }

            if (key >= '0' && key <= '9') {
                inputBuffer.push_back(static_cast<wchar_t>(key));
                RenderIdleView(deviceName, sessions, inputBuffer, message);
                continue;
            }

            if (key == '\r') {
                if (inputBuffer.empty()) {
                    message = L"Please enter a session index first.";
                    RenderIdleView(deviceName, sessions, inputBuffer, message);
                    continue;
                }

                int selectedIndex = 0;
                try {
                    selectedIndex = std::stoi(inputBuffer);
                }
                catch (...) {
                    selectedIndex = 0;
                }

                inputBuffer.clear();

                if (selectedIndex <= 0 || selectedIndex > static_cast<int>(sessions.size())) {
                    message = L"Invalid session index.";
                    RenderIdleView(deviceName, sessions, inputBuffer, message);
                    continue;
                }

                AudioSessionInfo selected = sessions[static_cast<size_t>(selectedIndex - 1)];
                if (selected.processId == 0) {
                    message = L"This session cannot be captured by process PID.";
                    RenderIdleView(deviceName, sessions, inputBuffer, message);
                    continue;
                }

                ClearConsole();
                std::wcout << L"Selected session #" << selectedIndex << L"\n";
                std::wcout << L"Process: " << selected.processName << L" (PID=" << selected.processId << L")\n";
                std::wcout << L"Session: " << selected.sessionName << L"\n\n";
                std::wcout << L"Enter WAV output path (file or folder; empty for no file output):\n> " << std::flush;

                std::wstring wavPath;
                std::getline(std::wcin, wavPath);
                if (!std::wcin.good()) {
                    std::wcin.clear();
                    wavPath.clear();
                }
                {
                    std::wstring pathError;
                    wavPath = NormalizeWavPath(wavPath, pathError);
                    if (!pathError.empty()) {
                        message = pathError;
                        RenderIdleView(deviceName, sessions, inputBuffer, message);
                        continue;
                    }
                }

                if (!capturer.Start(selected, wavPath)) {
                    message = L"Capture start failed: an existing capture task is still running.";
                    RenderIdleView(deviceName, sessions, inputBuffer, message);
                    continue;
                }

                activeSession = selected;
                activeOutputPath = wavPath;
                captureMode = true;
                nextCaptureUiAt = std::chrono::steady_clock::now();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (captureMode || capturer.IsRunning()) {
        capturer.RequestStop();
        capturer.Wait();
    }

    ClearConsole();
    std::wcout << L"Program exited." << std::endl;

    if (shouldCoUninit) {
        CoUninitialize();
    }

    return 0;
}
