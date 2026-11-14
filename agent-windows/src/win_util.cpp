#include "win_util.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "archon/crypto.hpp"

using Microsoft::WRL::ComPtr;

namespace agent {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring app_dir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &base))) {
        dir = std::wstring(base) + L"\\Archon";
        CoTaskMemFree(base);
    } else {
        dir = L".\\Archon";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring exe_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

static DATA_BLOB entropy() {
    static const char kEntropy[] = "Archon-trust-v1";
    DATA_BLOB b;
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    b.cbData = DWORD(sizeof kEntropy - 1);
    return b;
}

bool dpapi_protect(const arc::Bytes& in, arc::Bytes& out) {
    DATA_BLOB src{DWORD(in.size()), const_cast<BYTE*>(in.data())};
    DATA_BLOB ent = entropy();
    DATA_BLOB dst{};
    if (!CryptProtectData(&src, L"Archon", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst))
        return false;
    out.assign(dst.pbData, dst.pbData + dst.cbData);
    LocalFree(dst.pbData);
    return true;
}

bool dpapi_unprotect(const arc::Bytes& in, arc::Bytes& out) {
    DATA_BLOB src{DWORD(in.size()), const_cast<BYTE*>(in.data())};
    DATA_BLOB ent = entropy();
    DATA_BLOB dst{};
    if (!CryptUnprotectData(&src, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst))
        return false;
    out.assign(dst.pbData, dst.pbData + dst.cbData);
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return true;
}

Config load_config() {
    Config cfg;
    const std::wstring path = app_dir() + L"\\agent.cfg";
    std::ifstream in(path);
    std::string line;
    bool have_id = false;
    while (std::getline(in, line)) {
        if (line.rfind("id=", 0) == 0) have_id = arc::from_hex(line.substr(3), cfg.agent_id.data(), 16);
        if (line.rfind("lockouts=", 0) == 0) cfg.lockouts = std::atoi(line.c_str() + 9);
        if (line.rfind("locked_until=", 0) == 0) cfg.locked_until = std::atoll(line.c_str() + 13);
        if (line.rfind("port=", 0) == 0) {
            int p = std::atoi(line.c_str() + 5);
            if (p > 0 && p < 65536) cfg.port = uint16_t(p);
        }
    }
    if (!have_id) {
        cfg.first_run = true;
        cfg.agent_id = arc::random_array<16>();
        std::ofstream out(path, std::ios::trunc);
        out << "# Archon agent configuration\n";
        out << "id=" << arc::to_hex(cfg.agent_id) << "\n";
        out << "port=" << cfg.port << "\n";
    }
    return cfg;
}

void save_lockout(int lockouts, int64_t locked_until) {
    const std::wstring path = app_dir() + L"\\agent.cfg";
    std::ifstream in(path);
    std::string line, kept;
    while (std::getline(in, line))
        if (line.rfind("lockouts=", 0) != 0 && line.rfind("locked_until=", 0) != 0) kept += line + "\n";
    in.close();
    std::ofstream out(path, std::ios::trunc);
    out << kept << "lockouts=" << lockouts << "\nlocked_until=" << locked_until << "\n";
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostart_enabled() {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Archon", RRF_RT_REG_SZ, nullptr, buf, &size);
    if (st != ERROR_SUCCESS) return false;
    // Only "ours" if it points to this executable (handles moved installs).
    std::wstring v(buf);
    return v.find(exe_path()) != std::wstring::npos;
}

bool set_autostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS st;
    if (on) {
        std::wstring cmd = L"\"" + exe_path() + L"\" --background";
        st = RegSetValueExW(key, L"Archon", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, L"Archon");
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

void log_line(const std::string& line) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const std::wstring path = app_dir() + L"\\agent.log";
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow > 512 * 1024) {
        const std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::ofstream out(path, std::ios::app);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[32];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond);
    out << ts << line << "\n";
}

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                  &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (sa->sa_family != AF_INET) continue;  // IPv4 is what people type
            char host[64] = {0};
            if (getnameinfo(sa, u->Address.iSockaddrLength, host, sizeof host, nullptr, 0, NI_NUMERICHOST) == 0) {
                std::string h = host;
                if (h.rfind("169.254.", 0) != 0) out.push_back(h);
            }
        }
    }
    return out;
}

std::wstring computer_name() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return L"PC";
    return std::wstring(buf, n);
}

bool icon_to_png(HICON icon, arc::Bytes& png) {
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    BITMAP bm{};
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    const int w = bm.bmWidth;
    const int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    bool ok = false;
    if (w > 0 && h > 0 && w <= 256 && h <= 256) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> px(size_t(w) * size_t(h));
        HDC dc = GetDC(nullptr);
        bool have = ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, UINT(h), px.data(), &bi, DIB_RGB_COLORS);
        bool has_alpha = false;
        for (uint32_t p : px)
            if (p & 0xFF000000) {
                has_alpha = true;
                break;
            }
        if (have && !has_alpha && ii.hbmMask) {
            // Old-style icon: derive alpha from the AND mask.
            std::vector<uint32_t> mask(px.size());
            if (GetDIBits(dc, ii.hbmMask, 0, UINT(h), mask.data(), &bi, DIB_RGB_COLORS))
                for (size_t i = 0; i < px.size(); ++i) px[i] |= (mask[i] & 0xFFFFFF) ? 0 : 0xFF000000;
        }
        ReleaseDC(nullptr, dc);
        if (have) {
            ComPtr<IWICImagingFactory> f;
            ComPtr<IStream> stream;
            ComPtr<IWICBitmapEncoder> enc;
            ComPtr<IWICBitmapFrameEncode> frame;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&f))) &&
                SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
                SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->SetSize(UINT(w), UINT(h)))) {
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                if (SUCCEEDED(frame->WritePixels(UINT(h), UINT(w * 4), UINT(px.size() * 4),
                                                 reinterpret_cast<BYTE*>(px.data()))) &&
                    SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                    HGLOBAL hg = nullptr;
                    if (SUCCEEDED(GetHGlobalFromStream(stream.Get(), &hg))) {
                        STATSTG st{};
                        stream->Stat(&st, STATFLAG_NONAME);
                        const void* p = GlobalLock(hg);
                        if (p) {
                            const uint8_t* b = static_cast<const uint8_t*>(p);
                            png.assign(b, b + st.cbSize.QuadPart);
                            GlobalUnlock(hg);
                            ok = true;
                        }
                    }
                }
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

bool is_portuguese_ui() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE; }

}  // namespace agent
