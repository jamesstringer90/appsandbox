#include "headless_ssh.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SRWLOCK lock;
    volatile LONG refs;
    UINT64 id;AsbVmId vm_id;
    HANDLE process,stdin_pipe,stdout_pipe,stderr_pipe,writer,stop,ready;
    BYTE pending[65536];
    UINT32 head,tail;
    DWORD error;
    BOOL closing,close_stdin;
} SshSession;

static SRWLOCK registry=SRWLOCK_INIT;
static SshSession *sessions[16];

static void release(SshSession *s)
{
    if (!InterlockedDecrement(&s->refs)) {
        HANDLE handles[]={s->process,s->stdin_pipe,s->stdout_pipe,s->stderr_pipe,s->writer,s->stop,s->ready};
        for (unsigned i=0;i<ARRAYSIZE(handles);++i) if (handles[i]) CloseHandle(handles[i]);
        free(s);
    }
}

static SshSession *find(AsbVmId vm,UINT64 id)
{
    SshSession *result=NULL;
    AcquireSRWLockShared(&registry);
    for (unsigned i=0;i<ARRAYSIZE(sessions);++i)
        if (sessions[i] && sessions[i]->id==id && sessions[i]->vm_id==vm) {
            result=sessions[i];InterlockedIncrement(&result->refs);break;
        }
    ReleaseSRWLockShared(&registry);
    return result;
}

static DWORD WINAPI write_thread(void *context)
{
    SshSession *s=context;
    for (;;) {
        HANDLE wait[]={s->stop,s->ready};
        if (WaitForMultipleObjects(2,wait,FALSE,INFINITE)!=WAIT_OBJECT_0+1) break;
        for (;;) {
            BYTE bytes[4096];DWORD count=0,written=0;
            AcquireSRWLockExclusive(&s->lock);
            if (s->closing) { ReleaseSRWLockExclusive(&s->lock);goto done; }
            count=min(s->tail-s->head,(UINT32)sizeof(bytes));
            for (DWORD i=0;i<count;++i) bytes[i]=s->pending[(s->head+i)%sizeof(s->pending)];
            if (!count) {
                ResetEvent(s->ready);
                if (s->close_stdin && s->stdin_pipe) { CloseHandle(s->stdin_pipe);s->stdin_pipe=NULL; }
            }
            HANDLE pipe=s->stdin_pipe;
            ReleaseSRWLockExclusive(&s->lock);
            if (!count) break;
            BOOL ok=pipe && WriteFile(pipe,bytes,count,&written,NULL);
            DWORD error=ok?0:GetLastError();
            AcquireSRWLockExclusive(&s->lock);
            s->head+=written;
            if (error || !written) s->error=error?error:ERROR_WRITE_FAULT;
            ReleaseSRWLockExclusive(&s->lock);
            if (error || !written) goto done;
        }
    }
done:
    release(s);return 0;
}

static BOOL argument(wchar_t *line,size_t capacity,const wchar_t *value)
{
    size_t n=wcslen(line);
    if (n+4>=capacity) return FALSE;
    if (n) line[n++]=L' ';
    line[n++]=L'"';
    while (*value) {
        size_t slashes=0;
        while (*value==L'\\') { ++slashes;++value; }
        size_t emit=(*value==L'"' || !*value)?slashes*2:slashes;
        if (n+emit+4>=capacity) return FALSE;
        while (emit--) line[n++]=L'\\';
        if (!*value) break;
        if (*value==L'"') line[n++]=L'\\';
        line[n++]=*value++;
    }
    line[n++]=L'"';line[n]=0;return TRUE;
}

ASB_API DWORD asb_ssh_open(const AsbHeadlessTarget *target,const wchar_t *command,UINT64 *id)
{
    if (!target->ssh_port || !target->user[0]) return ERROR_NOT_READY;
    wchar_t executable[MAX_PATH],base[MAX_PATH],key[MAX_PATH],known[MAX_PATH+80],port[16],destination[160];
    UINT length=GetSystemDirectoryW(executable,ARRAYSIZE(executable));
    if (!length || length>=ARRAYSIZE(executable)) return ERROR_PATH_NOT_FOUND;
    if (wcscat_s(executable,ARRAYSIZE(executable),L"\\OpenSSH\\ssh.exe")) return ERROR_INVALID_PARAMETER;
    if (!GetEnvironmentVariableW(L"ProgramData",base,ARRAYSIZE(base))) wcscpy_s(base,ARRAYSIZE(base),L"C:\\ProgramData");
    swprintf_s(key,ARRAYSIZE(key),L"%s\\AppSandbox\\ssh\\id_appsandbox",base);
    swprintf_s(known,ARRAYSIZE(known),L"UserKnownHostsFile=%s\\AppSandbox\\ssh\\known_hosts",base);
    swprintf_s(port,ARRAYSIZE(port),L"%lu",target->ssh_port);
    swprintf_s(destination,ARRAYSIZE(destination),L"%s@127.0.0.1",target->user);
    if (GetFileAttributesW(key)==INVALID_FILE_ATTRIBUTES) return ERROR_NOT_READY;
    wchar_t *line=calloc(32768,sizeof(wchar_t));
    if (!line) return ERROR_NOT_ENOUGH_MEMORY;
    const wchar_t *args[]={executable,L"-T",L"-o",L"BatchMode=yes",L"-o",L"StrictHostKeyChecking=accept-new",
        L"-o",known,L"-i",key,L"-p",port,L"--",destination};
    BOOL valid=TRUE;
    for (unsigned i=0;i<ARRAYSIZE(args);++i) if (!argument(line,32768,args[i])) valid=FALSE;
    if (command && command[0] && !argument(line,32768,command)) valid=FALSE;
    if (!valid) { free(line);return ERROR_INVALID_PARAMETER; }
    SshSession *s=calloc(1,sizeof(*s));
    if (!s) { free(line);return ERROR_NOT_ENOUGH_MEMORY; }
    InitializeSRWLock(&s->lock);s->refs=1;s->vm_id=target->id;
    s->id=asb_vm_id_new();
    HANDLE child_in=NULL,child_out=NULL,child_err=NULL;
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    DWORD error=0;
    STARTUPINFOEXW startup={0};PROCESS_INFORMATION process={0};
    startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    SIZE_T attribute_size=0;BOOL attributes_ready=FALSE;
    if (!s->id || !CreatePipe(&child_in,&s->stdin_pipe,&security,65536) ||
        !CreatePipe(&s->stdout_pipe,&child_out,&security,65536) ||
        !CreatePipe(&s->stderr_pipe,&child_err,&security,65536)) { error=GetLastError();if (!error) error=ERROR_GEN_FAILURE;goto done; }
    SetHandleInformation(s->stdin_pipe,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(s->stdout_pipe,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(s->stderr_pipe,HANDLE_FLAG_INHERIT,0);
    InitializeProcThreadAttributeList(NULL,1,0,&attribute_size);
    startup.lpAttributeList=malloc(attribute_size);
    if (!startup.lpAttributeList) { error=ERROR_NOT_ENOUGH_MEMORY;goto done; }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size)) { error=GetLastError();goto done; }
    attributes_ready=TRUE;
    HANDLE inherited[]={child_in,child_out,child_err};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL)) { error=GetLastError();goto done; }
    startup.StartupInfo.hStdInput=child_in;startup.StartupInfo.hStdOutput=child_out;startup.StartupInfo.hStdError=child_err;
    if (!CreateProcessW(executable,line,NULL,NULL,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&process)) { error=GetLastError();goto done; }
    s->process=process.hProcess;CloseHandle(process.hThread);
    s->stop=CreateEventW(NULL,TRUE,FALSE,NULL);s->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!s->stop || !s->ready) { error=GetLastError();goto done; }
    InterlockedIncrement(&s->refs);
    s->writer=CreateThread(NULL,0,write_thread,s,CREATE_SUSPENDED,NULL);
    if (!s->writer) { InterlockedDecrement(&s->refs);error=GetLastError();goto done; }
    AcquireSRWLockExclusive(&registry);
    int slot=-1;
    for (unsigned i=0;i<ARRAYSIZE(sessions);++i) if (!sessions[i]) { slot=(int)i;break; }
    if (slot>=0) sessions[slot]=s;
    ReleaseSRWLockExclusive(&registry);
    if (slot<0) { error=ERROR_BUSY;SetEvent(s->stop); }
    ResumeThread(s->writer);
    if (!error) *id=s->id;
done:
    if (attributes_ready) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);free(line);
    if (child_in) CloseHandle(child_in);if (child_out) CloseHandle(child_out);if (child_err) CloseHandle(child_err);
    if (error) {
        if (s->process) TerminateProcess(s->process,1);
        if (s->writer) WaitForSingleObject(s->writer,10000);
        release(s);
    }
    return error;
}

ASB_API DWORD asb_ssh_write(AsbVmId vm,UINT64 id,const BYTE *bytes,DWORD count,BOOL close_stdin)
{
    SshSession *s=find(vm,id);
    if (!s) return ERROR_NOT_FOUND;
    AcquireSRWLockExclusive(&s->lock);
    DWORD error=s->closing?ERROR_NOT_FOUND:s->close_stdin || !s->stdin_pipe?ERROR_BROKEN_PIPE:s->error;
    if (!error && count>sizeof(s->pending)-(s->tail-s->head)) error=ERROR_BUSY;
    if (!error) {
        for (DWORD i=0;i<count;++i) s->pending[s->tail++%sizeof(s->pending)]=bytes[i];
        if (close_stdin) s->close_stdin=TRUE;
        SetEvent(s->ready);
    }
    ReleaseSRWLockExclusive(&s->lock);release(s);return error;
}

static DWORD read_pipe(HANDLE pipe,BYTE **bytes,DWORD *size)
{
    DWORD available=0;
    if (!PeekNamedPipe(pipe,NULL,0,NULL,&available,NULL)) return GetLastError()==ERROR_BROKEN_PIPE?0:GetLastError();
    available=min(available,65536u);
    if (!available) return 0;
    *bytes=malloc(available);
    if (!*bytes) return ERROR_NOT_ENOUGH_MEMORY;
    return ReadFile(pipe,*bytes,available,size,NULL)?0:GetLastError();
}

ASB_API DWORD asb_ssh_read(AsbVmId vm,UINT64 id,DWORD timeout,AsbSshOutput *out)
{
    memset(out,0,sizeof(*out));
    if (timeout>10000) return ERROR_INVALID_PARAMETER;
    SshSession *s=find(vm,id);
    if (!s) return ERROR_NOT_FOUND;
    ULONGLONG deadline=GetTickCount64()+timeout;
    DWORD error=0;
    do {
        AcquireSRWLockExclusive(&s->lock);
        if (s->closing) error=ERROR_NOT_FOUND;
        if (!error) error=read_pipe(s->stdout_pipe,&out->stdout_bytes,&out->stdout_size);
        if (!error) error=read_pipe(s->stderr_pipe,&out->stderr_bytes,&out->stderr_size);
        DWORD code=STILL_ACTIVE;
        if (!error && !GetExitCodeProcess(s->process,&code)) error=GetLastError();
        out->running=code==STILL_ACTIVE;out->exit_code=code;out->stdin_error=s->error;
        ReleaseSRWLockExclusive(&s->lock);
        if (error || !out->running || out->stdout_size || out->stderr_size || GetTickCount64()>=deadline) break;
        Sleep(20);
    } while (TRUE);
    release(s);
    if (error) asb_ssh_output_free(out);
    return error;
}

ASB_API void asb_ssh_output_free(AsbSshOutput *out)
{
    free(out->stdout_bytes);free(out->stderr_bytes);memset(out,0,sizeof(*out));
}

ASB_API DWORD asb_ssh_close(AsbVmId vm,UINT64 id)
{
    SshSession *s=NULL;
    AcquireSRWLockExclusive(&registry);
    for (unsigned i=0;i<ARRAYSIZE(sessions);++i)
        if (sessions[i] && sessions[i]->vm_id==vm && sessions[i]->id==id) { s=sessions[i];sessions[i]=NULL;break; }
    ReleaseSRWLockExclusive(&registry);
    if (!s) return ERROR_NOT_FOUND;
    AcquireSRWLockExclusive(&s->lock);s->closing=TRUE;ReleaseSRWLockExclusive(&s->lock);
    SetEvent(s->stop);CancelSynchronousIo(s->writer);
    if (WaitForSingleObject(s->process,0)==WAIT_TIMEOUT) TerminateProcess(s->process,1);
    DWORD result=WaitForSingleObject(s->writer,10000)==WAIT_OBJECT_0?0:ERROR_TIMEOUT;
    release(s);return result;
}

ASB_API void asb_ssh_shutdown(AsbVmId vm)
{
    for (;;) {
        UINT64 id=0;AsbVmId target=0;
        AcquireSRWLockShared(&registry);
        for (unsigned i=0;i<ARRAYSIZE(sessions);++i) if (sessions[i] && (!vm || sessions[i]->vm_id==vm)) { id=sessions[i]->id;target=sessions[i]->vm_id;break; }
        ReleaseSRWLockShared(&registry);
        if (!id) break;
        asb_ssh_close(target,id);
    }
}
