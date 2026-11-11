#include "headless_vm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <spawn.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

typedef struct {
    atomic_int references;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t writer;
    pid_t pid;
    uint64_t id,vm_id;
    int input,output,error,closing,close_input,exited;
    uint32_t exit_code,input_error,head,tail;
    unsigned char pending[65536];
} Ssh;

static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static Ssh *sessions[16];

static void release(Ssh *s)
{
    if (atomic_fetch_sub(&s->references,1)!=1) return;
    if (s->input>=0) close(s->input);
    close(s->output);close(s->error);
    pthread_mutex_destroy(&s->lock);pthread_cond_destroy(&s->changed);free(s);
}

static Ssh *find(uint64_t vm,uint64_t id)
{
    Ssh *found=NULL;pthread_mutex_lock(&registry);
    for (int i=0;i<16;++i) if (sessions[i] && sessions[i]->vm_id==vm && sessions[i]->id==id) {
        found=sessions[i];atomic_fetch_add(&found->references,1);break;
    }
    pthread_mutex_unlock(&registry);return found;
}

static void *writer(void *context)
{
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);pthread_sigmask(SIG_BLOCK,&blocked,NULL);
    Ssh *s=context;
    pthread_mutex_lock(&s->lock);
    while (!s->closing && !s->input_error) {
        while (!s->closing && s->head==s->tail && !s->close_input) pthread_cond_wait(&s->changed,&s->lock);
        if (s->closing) break;
        size_t count=s->tail-s->head;
        if (!count) {
            if (s->input>=0) { close(s->input);s->input=-1; }
            break;
        }
        unsigned char bytes[4096];if (count>sizeof(bytes)) count=sizeof(bytes);
        for (size_t i=0;i<count;++i) bytes[i]=s->pending[(s->head+i)%sizeof(s->pending)];
        int fd=s->input;pthread_mutex_unlock(&s->lock);
        struct pollfd pending={fd,POLLOUT,0};int result=poll(&pending,1,100);
        ssize_t written=result>0?write(fd,bytes,count):-1;int error=errno;
        pthread_mutex_lock(&s->lock);
        if (written>0) s->head+=(uint32_t)written;
        else if (result<0 && error!=EINTR) s->input_error=ASB_WRITE;
        else if (result>0 && error!=EINTR && error!=EAGAIN) s->input_error=ASB_PIPE;
    }
    pthread_mutex_unlock(&s->lock);release(s);return NULL;
}

static int make_pipe(int pair[2])
{
    if (pipe(pair)) return 0;
    fcntl(pair[0],F_SETFD,FD_CLOEXEC);fcntl(pair[1],F_SETFD,FD_CLOEXEC);return 1;
}

uint32_t asb_mac_ssh_open(const AsbMacHeadlessTarget *target,const char *command,uint64_t *id)
{
    if (!target->ssh_port || !target->user[0] || access(target->key,R_OK)) return ASB_NOT_READY;
    int in[2]={-1,-1},out[2]={-1,-1},err[2]={-1,-1};
    if (!make_pipe(in) || !make_pipe(out) || !make_pipe(err)) goto failed;
    char port[16],destination[160],known[2100];
    snprintf(port,sizeof(port),"%u",target->ssh_port);
    snprintf(destination,sizeof(destination),"%s@127.0.0.1",target->user);
    snprintf(known,sizeof(known),"UserKnownHostsFile=%s",target->known_hosts);
    char *args[]={"/usr/bin/ssh","-T","-o","BatchMode=yes","-o","StrictHostKeyChecking=accept-new",
        "-o",known,"-i",(char *)target->key,"-p",port,"--",destination,(char *)(command && *command?command:NULL),NULL};
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,in[0],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,out[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,err[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,in[1]);posix_spawn_file_actions_addclose(&actions,out[0]);posix_spawn_file_actions_addclose(&actions,err[0]);
    posix_spawn_file_actions_addclose(&actions,in[0]);posix_spawn_file_actions_addclose(&actions,out[1]);posix_spawn_file_actions_addclose(&actions,err[1]);
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT|POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes,0);
    pid_t pid;int result=posix_spawn(&pid,args[0],&actions,&attributes,args,environ);
    posix_spawnattr_destroy(&attributes);posix_spawn_file_actions_destroy(&actions);
    if (result) goto failed;
    close(in[0]);close(out[1]);close(err[1]);in[0]=out[1]=err[1]=-1;
    Ssh *s=calloc(1,sizeof(*s));
    if (!s) { kill(-pid,SIGKILL);waitpid(pid,NULL,0);goto failed; }
    atomic_init(&s->references,2);pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->changed,NULL);
    s->pid=pid;s->vm_id=target->id;s->input=in[1];s->output=out[0];s->error=err[0];
    fcntl(s->input,F_SETFL,fcntl(s->input,F_GETFL)|O_NONBLOCK);
    fcntl(s->output,F_SETFL,fcntl(s->output,F_GETFL)|O_NONBLOCK);fcntl(s->error,F_SETFL,fcntl(s->error,F_GETFL)|O_NONBLOCK);
    arc4random_buf(&s->id,sizeof(s->id));if (!s->id) s->id=1;
    pthread_mutex_lock(&registry);int slot=-1;
    for (int i=0;i<16;++i) if (!sessions[i]) { slot=i;break; }
    if (slot<0 || pthread_create(&s->writer,NULL,writer,s)) {
        pthread_mutex_unlock(&registry);kill(-pid,SIGKILL);waitpid(pid,NULL,0);release(s);release(s);return ASB_BUSY;
    }
    sessions[slot]=s;*id=s->id;pthread_mutex_unlock(&registry);return 0;
failed:
    for (int i=0;i<2;++i) { if (in[i]>=0) close(in[i]);if (out[i]>=0) close(out[i]);if (err[i]>=0) close(err[i]); }
    return ASB_NOT_READY;
}

static uint32_t read_pipe(int fd,unsigned char **bytes,uint32_t *size)
{
    *bytes=malloc(65536);*size=0;if (!*bytes) return ASB_MEMORY;
    while (*size<65536) {
        ssize_t count=read(fd,*bytes+*size,65536-*size);
        if (count>0) { *size+=(uint32_t)count;continue; }
        if (count<0 && errno==EINTR) continue;
        if (count<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) return ASB_PIPE;
        break;
    }
    return 0;
}

uint32_t asb_mac_ssh_read(uint64_t vm,uint64_t id,unsigned timeout,AsbMacSshOutput *out)
{
    memset(out,0,sizeof(*out));if (timeout>10000) return ASB_INVALID;
    Ssh *s=find(vm,id);if (!s) return ASB_NOT_FOUND;
    struct pollfd pending[2]={{s->output,POLLIN,0},{s->error,POLLIN,0}};
    if (timeout) poll(pending,2,(int)timeout);
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->closing?ASB_NOT_FOUND:read_pipe(s->output,&out->stdout_bytes,&out->stdout_size);
    if (!error) error=read_pipe(s->error,&out->stderr_bytes,&out->stderr_size);
    if (!s->exited) {
        int status=0;pid_t result=waitpid(s->pid,&status,WNOHANG);
        if (result==s->pid) { s->exited=1;s->exit_code=WIFEXITED(status)?(uint32_t)WEXITSTATUS(status):(uint32_t)(128+WTERMSIG(status)); }
    }
    out->running=!s->exited;out->exit_code=s->exited?s->exit_code:259;out->stdin_error=s->input_error;
    pthread_mutex_unlock(&s->lock);release(s);
    if (error) asb_mac_ssh_output_free(out);
    return error;
}

uint32_t asb_mac_ssh_write(uint64_t vm,uint64_t id,const void *data,size_t size,int close_input)
{
    if ((!data && size) || size>65536) return ASB_INVALID;
    Ssh *s=find(vm,id);if (!s) return ASB_NOT_FOUND;
    pthread_mutex_lock(&s->lock);
    uint32_t error=s->closing?ASB_NOT_FOUND:s->close_input || s->input_error?ASB_PIPE:
        size>sizeof(s->pending)-(s->tail-s->head)?ASB_BUSY:0;
    if (!error) {
        for (size_t i=0;i<size;++i) s->pending[(s->tail+i)%sizeof(s->pending)]=((const unsigned char *)data)[i];
        s->tail+=(uint32_t)size;if (close_input) s->close_input=1;pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->lock);release(s);return error;
}

uint32_t asb_mac_ssh_close(uint64_t vm,uint64_t id)
{
    Ssh *s=NULL;pthread_mutex_lock(&registry);
    for (int i=0;i<16;++i) if (sessions[i] && sessions[i]->vm_id==vm && sessions[i]->id==id) { s=sessions[i];sessions[i]=NULL;break; }
    pthread_mutex_unlock(&registry);if (!s) return ASB_NOT_FOUND;
    pthread_mutex_lock(&s->lock);s->closing=1;
    if (!s->exited) { kill(-s->pid,SIGKILL);while (waitpid(s->pid,NULL,0)<0 && errno==EINTR) {} s->exited=1; }
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->lock);
    pthread_join(s->writer,NULL);release(s);return 0;
}

void asb_mac_ssh_output_free(AsbMacSshOutput *out)
{
    free(out->stdout_bytes);free(out->stderr_bytes);memset(out,0,sizeof(*out));
}

void asb_mac_ssh_shutdown(uint64_t vm)
{
    uint64_t ids[16],vms[16];unsigned count=0;
    pthread_mutex_lock(&registry);
    for (int i=0;i<16;++i) if (sessions[i] && (!vm || sessions[i]->vm_id==vm)) { ids[count]=sessions[i]->id;vms[count++]=sessions[i]->vm_id; }
    pthread_mutex_unlock(&registry);
    for (unsigned i=0;i<count;++i) asb_mac_ssh_close(vms[i],ids[i]);
}
