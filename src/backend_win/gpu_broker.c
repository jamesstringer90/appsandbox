#define COBJMACROS
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3d12.h>
#include <dxgi1_4.h>
#pragma warning(pop)
#include <ntsecapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpu_broker.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "d3d12.lib")

struct AsbGpuBroker {
    HANDLE pipe;
    HANDLE process;
    HANDLE job;
};

typedef struct {
    GUID vm;
    LUID adapter;
} GpuBrokerInit;

typedef struct {
    UINT64 handle;
    UINT width;
    UINT height;
    UINT fence;
} GpuBrokerRequest;

typedef struct {
    HRESULT status;
    UINT64 handle;
} GpuBrokerReply;

static wchar_t service_name[96];
static wchar_t pipe_name[128];
static DWORD parent_pid;
static GUID broker_id;
static SERVICE_STATUS_HANDLE service_status;

static void *broker_token_info(HANDLE token, TOKEN_INFORMATION_CLASS kind)
{
    DWORD size = 0;
    GetTokenInformation(token, kind, NULL, 0, &size);
    if (!size) return NULL;
    void *data = calloc(1, size);
    if (data && !GetTokenInformation(token, kind, data, size, &size)) {
        free(data);
        data = NULL;
    }
    return data;
}

static BOOL broker_same_image(HANDLE process)
{
    wchar_t current[32768], other[32768];
    DWORD size = ARRAYSIZE(other);
    return GetModuleFileNameW(NULL, current, ARRAYSIZE(current)) &&
        QueryFullProcessImageNameW(process, 0, other, &size) && _wcsicmp(current, other) == 0;
}

static BOOL broker_same_user(HANDLE process, BOOL require_vm)
{
    HANDLE self = NULL, other = NULL;
    TOKEN_USER *a = NULL, *b = NULL;
    TOKEN_GROUPS *groups = NULL;
    PSID vm = NULL;
    BOOL valid = FALSE, has_vm = !require_vm;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self) ||
        !OpenProcessToken(process, TOKEN_QUERY, &other)) goto done;
    a = broker_token_info(self, TokenUser);
    b = broker_token_info(other, TokenUser);
    if (!a || !b || !EqualSid(a->User.Sid, b->User.Sid)) goto done;
    if (require_vm) {
        groups = broker_token_info(other, TokenGroups);
        if (!groups || !AllocateAndInitializeSid(&nt, 2, SECURITY_VIRTUALSERVER_ID_BASE_RID,
                0, 0, 0, 0, 0, 0, 0, &vm)) goto done;
        for (DWORD i = 0; i < groups->GroupCount; ++i)
            if (EqualSid(vm, groups->Groups[i].Sid) &&
                (groups->Groups[i].Attributes & SE_GROUP_ENABLED)) has_vm = TRUE;
    }
    valid = has_vm;
done:
    if (vm) FreeSid(vm);
    free(groups);
    free(a);
    free(b);
    if (self) CloseHandle(self);
    if (other) CloseHandle(other);
    return valid;
}

static HANDLE broker_user_token(HANDLE process)
{
    HANDLE self = NULL, source = NULL, lsa = NULL, authenticated = NULL, restricted = NULL;
    TOKEN_USER *user = NULL, *created = NULL;
    TOKEN_GROUPS *groups = NULL;
    DWORD *session = NULL;
    PSID vm = NULL, interactive = NULL, logon = NULL;
    MSV1_0_S4U_LOGON *auth = NULL;
    void *profile = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    TOKEN_PRIVILEGES privileges = {0};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &self) ||
        !OpenProcessToken(process, TOKEN_QUERY, &source)) goto done;
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(NULL, SE_TCB_NAME, &privileges.Privileges[0].Luid)) goto done;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(self, FALSE, &privileges, 0, NULL, NULL) || GetLastError()) goto done;
    user = broker_token_info(source, TokenUser);
    groups = broker_token_info(source, TokenGroups);
    session = broker_token_info(source, TokenSessionId);
    if (!user || !groups || !session) goto done;
    wchar_t name[256], domain[256];
    DWORD name_size = ARRAYSIZE(name), domain_size = ARRAYSIZE(domain);
    SID_NAME_USE use;
    if (!LookupAccountSidW(NULL, user->User.Sid, name, &name_size, domain, &domain_size, &use) ||
        use != SidTypeUser) goto done;
    for (DWORD i = 0; i < groups->GroupCount; ++i)
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
            logon = groups->Groups[i].Sid;
    if (!logon || !AllocateAndInitializeSid(&nt, 2, SECURITY_VIRTUALSERVER_ID_BASE_RID,
            0, 0, 0, 0, 0, 0, 0, &vm) ||
        !AllocateAndInitializeSid(&nt, 1, SECURITY_INTERACTIVE_RID,
            0, 0, 0, 0, 0, 0, 0, &interactive)) goto done;
    struct { DWORD count; SID_AND_ATTRIBUTES groups[3]; } additional = {0};
    additional.count = 3;
    additional.groups[0].Sid = vm;
    additional.groups[1].Sid = interactive;
    additional.groups[2].Sid = logon;
    for (UINT i = 0; i < 3; ++i)
        additional.groups[i].Attributes = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
    LSA_STRING origin = {12, 13, "AppSandboxGpu"};
    LSA_STRING package = {(USHORT)strlen(MSV1_0_PACKAGE_NAME),
        (USHORT)(strlen(MSV1_0_PACKAGE_NAME) + 1), MSV1_0_PACKAGE_NAME};
    LSA_OPERATIONAL_MODE mode;
    ULONG package_id;
    if (LsaRegisterLogonProcess(&origin, &lsa, &mode) < 0 ||
        LsaLookupAuthenticationPackage(lsa, &package, &package_id) < 0) goto done;
    size_t size = sizeof(*auth) + (wcslen(name) + wcslen(domain) + 2) * sizeof(wchar_t);
    auth = calloc(1, size);
    if (!auth) goto done;
    auth->MessageType = MsV1_0S4ULogon;
    auth->UserPrincipalName.Buffer = (wchar_t *)(auth + 1);
    auth->UserPrincipalName.Length = (USHORT)(wcslen(name) * sizeof(wchar_t));
    auth->UserPrincipalName.MaximumLength = auth->UserPrincipalName.Length + sizeof(wchar_t);
    wcscpy_s(auth->UserPrincipalName.Buffer, wcslen(name) + 1, name);
    auth->DomainName.Buffer = auth->UserPrincipalName.Buffer + wcslen(name) + 1;
    auth->DomainName.Length = (USHORT)(wcslen(domain) * sizeof(wchar_t));
    auth->DomainName.MaximumLength = auth->DomainName.Length + sizeof(wchar_t);
    wcscpy_s(auth->DomainName.Buffer, wcslen(domain) + 1, domain);
    TOKEN_SOURCE token_source = {0};
    memcpy(token_source.SourceName, "ASBGpu", 6);
    if (!AllocateLocallyUniqueId(&token_source.SourceIdentifier)) goto done;
    ULONG profile_size;
    LUID logon_id;
    QUOTA_LIMITS quotas;
    LONG substatus;
    if (LsaLogonUser(lsa, &origin, Batch, package_id, auth, (ULONG)size,
            (TOKEN_GROUPS *)&additional, &token_source, &profile, &profile_size,
            &logon_id, &authenticated, &quotas, &substatus) < 0) goto done;
    created = broker_token_info(authenticated, TokenUser);
    if (!created || !EqualSid(user->User.Sid, created->User.Sid)) goto done;
    if (!CreateRestrictedToken(authenticated, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL,
            0, NULL, &restricted)) goto done;
    if (!SetTokenInformation(restricted, TokenSessionId, session, sizeof(*session))) {
        CloseHandle(restricted);
        restricted = NULL;
    }
done:
    if (profile) LsaFreeReturnBuffer(profile);
    if (lsa) LsaDeregisterLogonProcess(lsa);
    if (authenticated) CloseHandle(authenticated);
    if (source) CloseHandle(source);
    if (self) CloseHandle(self);
    if (vm) FreeSid(vm);
    if (interactive) FreeSid(interactive);
    free(auth);
    free(user);
    free(created);
    free(groups);
    free(session);
    return restricted;
}

static BOOL broker_transfer(HANDLE pipe, void *data, DWORD size, BOOL write, BOOL overlapped)
{
    DWORD transferred = 0;
    OVERLAPPED operation = {0};
    if (overlapped) {
        operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!operation.hEvent) return FALSE;
    }
    BOOL result = write ? WriteFile(pipe, data, size, &transferred, overlapped ? &operation : NULL)
                        : ReadFile(pipe, data, size, &transferred, overlapped ? &operation : NULL);
    if (!result && overlapped && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(operation.hEvent, 10000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            result = FALSE;
        } else result = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    }
    if (operation.hEvent) CloseHandle(operation.hEvent);
    return result && transferred == size;
}

static int broker_worker(void)
{
    HANDLE pipe = INVALID_HANDLE_VALUE, parent = NULL;
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3D12Device *device = NULL;
    GpuBrokerInit init = {0};
    HRESULT hr = E_FAIL;
    DWORD server = 0;
    if (!WaitNamedPipeW(pipe_name, 10000)) goto done;
    pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe, &server) || server != parent_pid)
        goto done;
    parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid);
    if (!parent || !broker_same_image(parent) || !broker_same_user(parent, FALSE)) goto done;
    if (!broker_transfer(pipe, &init, sizeof(init), FALSE, FALSE)) goto done;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IDXGIFactory4_EnumAdapterByLuid(factory, init.adapter,
        &IID_IDXGIAdapter1, (void **)&adapter);
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
        &IID_ID3D12Device, (void **)&device);
    typedef LONG (WINAPI *RegisterFn)(const GUID *);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    RegisterFn register_vm = gdi ? (RegisterFn)GetProcAddress(gdi, "D3DKMTRegisterVailProcess") : NULL;
    if (SUCCEEDED(hr)) {
        LONG status = register_vm ? register_vm(&init.vm) : (LONG)0xc0000002;
        hr = status < 0 ? HRESULT_FROM_NT(status) : S_OK;
    }
    if (!broker_transfer(pipe, &hr, sizeof(hr), TRUE, FALSE) || FAILED(hr)) goto done;
    for (UINT i = 0; i < 6; ++i) {
        GpuBrokerRequest request;
        GpuBrokerReply reply = {E_INVALIDARG, 0};
        ID3D12Resource *texture = NULL;
        ID3D12Fence *fence = NULL;
        HANDLE shared = NULL;
        if (!broker_transfer(pipe, &request, sizeof(request), FALSE, FALSE)) break;
        if (request.handle && request.fence <= 1) {
            HANDLE original = (HANDLE)(UINT_PTR)request.handle;
            if (request.fence) {
                reply.status = ID3D12Device_OpenSharedHandle(device, original,
                    &IID_ID3D12Fence, (void **)&fence);
            } else {
                reply.status = ID3D12Device_OpenSharedHandle(device, original,
                    &IID_ID3D12Resource, (void **)&texture);
            }
            if (SUCCEEDED(reply.status)) {
                CloseHandle(original);
                if (texture) {
                    D3D12_RESOURCE_DESC desc;
                    D3D12_HEAP_PROPERTIES heap;
                    D3D12_HEAP_FLAGS flags;
                    ID3D12Resource_GetDesc(texture, &desc);
                    reply.status = ID3D12Resource_GetHeapProperties(texture, &heap, &flags);
                    if (SUCCEEDED(reply.status) && (heap.Type != D3D12_HEAP_TYPE_DEFAULT ||
                        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                        !request.width || request.width > 7680 || !request.height || request.height > 4320 ||
                        desc.Width != request.width || desc.Height != request.height ||
                        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.MipLevels != 1 ||
                        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
                        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)))
                        reply.status = E_INVALIDARG;
                }
                if (SUCCEEDED(reply.status)) reply.status = ID3D12Device_CreateSharedHandle(device,
                    texture ? (ID3D12DeviceChild *)texture : (ID3D12DeviceChild *)fence,
                    NULL, GENERIC_ALL, NULL, &shared);
            }
        }
        reply.handle = (UINT64)(UINT_PTR)shared;
        BOOL sent = broker_transfer(pipe, &reply, sizeof(reply), TRUE, FALSE);
        DWORD ack = 0;
        if (sent && shared) sent = broker_transfer(pipe, &ack, sizeof(ack), FALSE, FALSE);
        if (shared) CloseHandle(shared);
        if (texture) ID3D12Resource_Release(texture);
        if (fence) ID3D12Fence_Release(fence);
        if (!sent || FAILED(reply.status)) break;
    }
    DWORD stop;
    broker_transfer(pipe, &stop, sizeof(stop), FALSE, FALSE);
done:
    if (device) ID3D12Device_Release(device);
    if (adapter) IDXGIAdapter1_Release(adapter);
    if (factory) IDXGIFactory4_Release(factory);
    if (parent) CloseHandle(parent);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    return FAILED(hr) ? 1 : 0;
}

static DWORD WINAPI broker_service_control(DWORD control, DWORD event, void *data, void *context)
{
    (void)control; (void)event; (void)data; (void)context;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI broker_service_main(DWORD argc, wchar_t **argv)
{
    (void)argc; (void)argv;
    SERVICE_STATUS state = {0};
    service_status = RegisterServiceCtrlHandlerExW(service_name, broker_service_control, NULL);
    if (!service_status) return;
    state.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    state.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(service_status, &state);
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid);
    HANDLE token = parent && broker_same_image(parent) ? broker_user_token(parent) : NULL;
    if (token && WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) {
        wchar_t path[32768], command[33000], id[40];
        STARTUPINFOW start = {0};
        PROCESS_INFORMATION child = {0};
        start.cb = sizeof(start);
        start.lpDesktop = L"winsta0\\default";
        GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
        StringFromGUID2(&broker_id, id, ARRAYSIZE(id));
        swprintf_s(command, ARRAYSIZE(command), L"\"%s\" --gpu-worker %lu %s", path, parent_pid, id);
        if (CreateProcessAsUserW(token, path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                NULL, NULL, &start, &child)) {
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    }
    if (token) CloseHandle(token);
    if (parent) CloseHandle(parent);
    state.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status, &state);
}

BOOL asb_gpu_broker_dispatch(int *result)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return FALSE;
    BOOL service = argc > 1 && wcscmp(argv[1], L"--gpu-service") == 0;
    BOOL worker = argc > 1 && wcscmp(argv[1], L"--gpu-worker") == 0;
    if (!service && !worker) { LocalFree(argv); return FALSE; }
    *result = 1;
    wchar_t *end = NULL;
    if (argc == 4) {
        parent_pid = wcstoul(argv[2], &end, 10);
        if (parent_pid && end && !*end && SUCCEEDED(CLSIDFromString(argv[3], &broker_id))) {
            wchar_t id[40];
            StringFromGUID2(&broker_id, id, ARRAYSIZE(id));
            swprintf_s(service_name, ARRAYSIZE(service_name), L"AppSandboxGpu-%s", id);
            swprintf_s(pipe_name, ARRAYSIZE(pipe_name), L"\\\\.\\pipe\\AppSandboxGpu-%s", id);
            if (worker) *result = broker_worker();
            else {
                SERVICE_TABLE_ENTRYW entries[] = {{service_name, broker_service_main}, {NULL, NULL}};
                *result = StartServiceCtrlDispatcherW(entries) ? 0 : 1;
            }
        }
    }
    LocalFree(argv);
    return TRUE;
}

void asb_gpu_broker_close(AsbGpuBroker *broker)
{
    if (!broker) return;
    if (broker->pipe != INVALID_HANDLE_VALUE) CloseHandle(broker->pipe);
    if (broker->job) CloseHandle(broker->job);
    if (broker->process) CloseHandle(broker->process);
    free(broker);
}

AsbGpuBroker *asb_gpu_broker_open(const GUID *vm, LUID adapter)
{
    AsbGpuBroker *broker = calloc(1, sizeof(*broker));
    if (!broker) return NULL;
    broker->pipe = INVALID_HANDLE_VALUE;
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    wchar_t *sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SC_HANDLE manager = NULL, service = NULL;
    OVERLAPPED connection = {0};
    BOOL pending = FALSE, success = FALSE;
    GUID id;
    wchar_t id_text[40], name[96], pipe[128], path[32768], command[33000], acl[256];
    if (FAILED(CoCreateGuid(&id)) || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto done;
    user = broker_token_info(token, TokenUser);
    if (!user || !ConvertSidToStringSidW(user->User.Sid, &sid)) goto done;
    swprintf_s(acl, ARRAYSIZE(acl), L"D:P(A;;GA;;;SY)(A;;GA;;;%s)", sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl, SDDL_REVISION_1, &descriptor, NULL)) goto done;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    StringFromGUID2(&id, id_text, ARRAYSIZE(id_text));
    swprintf_s(name, ARRAYSIZE(name), L"AppSandboxGpu-%s", id_text);
    swprintf_s(pipe, ARRAYSIZE(pipe), L"\\\\.\\pipe\\AppSandboxGpu-%s", id_text);
    broker->pipe = CreateNamedPipeW(pipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &attributes);
    if (broker->pipe == INVALID_HANDLE_VALUE) goto done;
    connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connection.hEvent) goto done;
    if (!ConnectNamedPipe(broker->pipe, &connection)) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) pending = TRUE;
        else if (error != ERROR_PIPE_CONNECTED) goto done;
    }
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!manager || !GetModuleFileNameW(NULL, path, ARRAYSIZE(path))) goto done;
    swprintf_s(command, ARRAYSIZE(command), L"\"%s\" --gpu-service %lu %s",
        path, GetCurrentProcessId(), id_text);
    service = CreateServiceW(manager, name, L"AppSandbox GPU display", SERVICE_START | DELETE,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
        command, NULL, NULL, NULL, NULL, NULL);
    if (!service) goto done;
    BOOL started = StartServiceW(service, 0, NULL);
    DeleteService(service);
    if (!started) goto done;
    if (pending) {
        DWORD transferred;
        if (WaitForSingleObject(connection.hEvent, 10000) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(broker->pipe, &connection, &transferred, FALSE)) goto done;
        pending = FALSE;
    }
    DWORD pid;
    if (!GetNamedPipeClientProcessId(broker->pipe, &pid)) goto done;
    broker->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE |
        PROCESS_SET_QUOTA | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!broker->process || !broker_same_image(broker->process) || !broker_same_user(broker->process, TRUE)) goto done;
    broker->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!broker->job || !SetInformationJobObject(broker->job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits)) || !AssignProcessToJobObject(broker->job, broker->process)) goto done;
    GpuBrokerInit init = {*vm, adapter};
    HRESULT status;
    if (!broker_transfer(broker->pipe, &init, sizeof(init), TRUE, TRUE) ||
        !broker_transfer(broker->pipe, &status, sizeof(status), FALSE, TRUE) || FAILED(status)) goto done;
    success = TRUE;
done:
    if (pending) {
        DWORD transferred;
        CancelIoEx(broker->pipe, &connection);
        GetOverlappedResult(broker->pipe, &connection, &transferred, TRUE);
    }
    if (connection.hEvent) CloseHandle(connection.hEvent);
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    if (descriptor) LocalFree(descriptor);
    if (sid) LocalFree(sid);
    free(user);
    if (token) CloseHandle(token);
    if (!success) { asb_gpu_broker_close(broker); return NULL; }
    return broker;
}

HRESULT asb_gpu_broker_import(AsbGpuBroker *broker, UINT64 handle, BOOL fence,
                             UINT width, UINT height, HANDLE *imported)
{
    *imported = NULL;
    if (!broker || !handle) return E_INVALIDARG;
    GpuBrokerRequest request = {handle, width, height, fence != FALSE};
    GpuBrokerReply reply;
    if (!broker_transfer(broker->pipe, &request, sizeof(request), TRUE, TRUE) ||
        !broker_transfer(broker->pipe, &reply, sizeof(reply), FALSE, TRUE)) return E_FAIL;
    if (FAILED(reply.status)) return reply.status;
    HRESULT status = DuplicateHandle(broker->process, (HANDLE)(UINT_PTR)reply.handle,
        GetCurrentProcess(), imported, 0, FALSE, DUPLICATE_SAME_ACCESS)
        ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    DWORD ack = 1;
    if (!broker_transfer(broker->pipe, &ack, sizeof(ack), TRUE, TRUE)) {
        if (*imported) CloseHandle(*imported);
        *imported = NULL;
        return E_FAIL;
    }
    return status;
}
