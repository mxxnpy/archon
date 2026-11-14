#include "win_proc.hpp"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <vector>

namespace agent {

namespace {

constexpr wchar_t kRegKey[] = L"Software\\Archon";
constexpr wchar_t kRegValue[] = L"TabsPipe";

std::vector<uint8_t> token_info(HANDLE tok, TOKEN_INFORMATION_CLASS cls) {
    DWORD len = 0;
    GetTokenInformation(tok, cls, nullptr, 0, &len);
    std::vector<uint8_t> buf(len ? len : 1);
    if (!GetTokenInformation(tok, cls, buf.data(), len, &len)) buf.clear();
    return buf;
}

// User SID and integrity RID of a token.
bool token_identity(HANDLE tok, std::vector<uint8_t>& user, DWORD& integrity) {
    user = token_info(tok, TokenUser);
    std::vector<uint8_t> il = token_info(tok, TokenIntegrityLevel);
    if (user.empty() || il.empty()) return false;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(il.data())->Label.Sid;
    integrity = *GetSidSubAuthority(sid, DWORD(*GetSidSubAuthorityCount(sid) - 1));
    return true;
}

PSID token_user_sid(std::vector<uint8_t>& user) { return reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid; }

bool my_identity(std::vector<uint8_t>& user, DWORD& integrity) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    const bool ok = token_identity(tok, user, integrity);
    CloseHandle(tok);
    return ok;
}

}  // namespace

std::wstring user_sid() {
    std::vector<uint8_t> user;
    DWORD il = 0;
    if (!my_identity(user, il)) return {};
    LPWSTR s = nullptr;
    std::wstring out;
    if (ConvertSidToStringSidW(token_user_sid(user), &s)) {
        out = s;
        LocalFree(s);
    }
    return out;
}

std::wstring new_tab_pipe_name() {
    uint8_t rnd[12];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring tail;
    for (uint8_t b : rnd) {
        tail += hex[b >> 4];
        tail += hex[b & 15];
    }
    const std::wstring sid = user_sid();
    if (sid.empty()) return {};
    return L"\\\\.\\pipe\\archon-tabs-" + sid + L"-" + tail;
}

bool publish_tab_pipe_name(const std::wstring& name) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS s = RegSetValueExW(h, kRegValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                                     DWORD((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

void unpublish_tab_pipe_name(const std::wstring& name) {
    if (published_tab_pipe_name() != name) return;  // a newer agent took over: leave it
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return;
    RegDeleteValueW(h, kRegValue);
    RegCloseKey(h);
}

std::wstring published_tab_pipe_name() {
    wchar_t buf[512];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    const std::wstring name = buf;
    // Only our own shape: \\.\pipe\archon-tabs-<this user's SID>-<hex>.
    const std::wstring prefix = L"\\\\.\\pipe\\archon-tabs-" + user_sid() + L"-";
    if (name.size() != prefix.size() + 24 || name.compare(0, prefix.size(), prefix) != 0) return {};
    if (name.find_first_not_of(L"0123456789abcdef", prefix.size()) != std::wstring::npos) return {};
    return name;
}

std::wstring current_user_only_sddl() {
    // Protected DACL: full access for this user only (no inheritance, no Everyone).
    return L"D:P(A;;GA;;;" + user_sid() + L")";
}

std::wstring process_image(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = DWORD(std::size(buf));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

DWORD parent_pid(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) {
            parent = pe.th32ParentProcessID;
            break;
        }
    CloseHandle(snap);
    return parent;
}

bool process_is_peer(DWORD pid) {
    std::vector<uint8_t> me, them;
    DWORD my_il = 0, their_il = 0;
    if (!my_identity(me, my_il)) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    HANDLE tok = nullptr;
    bool ok = OpenProcessToken(h, TOKEN_QUERY, &tok) != FALSE;
    CloseHandle(h);
    if (!ok) return false;
    ok = token_identity(tok, them, their_il);
    CloseHandle(tok);
    return ok && EqualSid(token_user_sid(me), token_user_sid(them)) && their_il >= my_il;
}

bool pipe_owned_by_me(HANDLE pipe) {
    std::vector<uint8_t> me;
    DWORD il = 0;
    if (!my_identity(me, il)) return false;
    // Objects get the creator's default owner: the user, or BUILTIN\Administrators
    // for an elevated administrator. Accept both of this account's owners.
    std::vector<uint8_t> default_owner;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        default_owner = token_info(tok, TokenOwner);
        CloseHandle(tok);
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return false;
    const bool mine = owner && (EqualSid(owner, token_user_sid(me)) ||
                                (!default_owner.empty() &&
                                 EqualSid(owner, reinterpret_cast<TOKEN_OWNER*>(default_owner.data())->Owner)));
    LocalFree(sd);
    return mine;
}

bool same_file(const std::wstring& a, const std::wstring& b) {
    auto id_of = [](const std::wstring& p, FILE_ID_INFO& id) {
        HANDLE f = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandleEx(f, FileIdInfo, &id, sizeof id) != FALSE;
        CloseHandle(f);
        return ok;
    };
    FILE_ID_INFO ia{}, ib{};
    if (a.empty() || b.empty() || !id_of(a, ia) || !id_of(b, ib)) return false;
    return ia.VolumeSerialNumber == ib.VolumeSerialNumber && memcmp(&ia.FileId, &ib.FileId, sizeof ia.FileId) == 0;
}

bool same_path(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring dir_of(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring module_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

}  // namespace agent
