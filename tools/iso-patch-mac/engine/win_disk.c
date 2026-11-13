/*
 * win_disk.c -- end-to-end UEFI-bootable Windows 11 ARM64 disk builder.
 * Orchestrates the validated engines, sourcing every byte from the provided ISO:
 *   GPT (ESP+MSR+Windows, capture GUIDs) -> NTFS apply(install.wim image 1) ->
 *   FAT32 ESP -> copy boot files out of install.wim -> patched BCD.
 *
 *   win_disk <install.wim> <out.img> [image_idx] [disk_gib]
 */
#include "blockio.h"
#include "gpt.h"
#include "ntfs.h"
#include "fat32.h"
#include "bcd_patch.h"
#include "wim.h"
#include "wimimg.h"
#include "sha1.h"
#include "win_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
static double now_s(void){ struct timeval tv; gettimeofday(&tv,0); return (double)tv.tv_sec + tv.tv_usec/1e6; }

static wim_t *W; static wim_image_t *IMG; static ntfs_writer_t *NW;
static unsigned long g_dirs,g_files,g_empty,g_reparse,g_err,g_links; static unsigned long long g_bytes;
static uint8_t *g_boot_system;
static size_t g_boot_system_len;
static int g_boot_system_written;
static int name_eq(const uint8_t *nm, int nb, const char *a);

/* Apply-progress: bytes durably written so far (worker pool + inline small files),
 * read by a monitor thread vs g_total_bytes (the image's uncompressed total). The
 * apply phase is the bulk of the build; the monitor maps written/total into the
 * 4..80 band so the bar moves continuously instead of jumping 4 -> 82. g_written
 * is updated from the worker threads AND the apply thread, so use __atomic. The
 * monitor is the ONLY caller of progress() during apply (no stdout race). */
static unsigned long long g_written;       /* via __atomic_fetch_add / __atomic_load_n */
static unsigned long long g_total_bytes;
static void (*g_prog)(int,const char*);
static int                g_mon_stop;    /* via __atomic_* (set by main, polled by monitor) */
static int                g_last_pct;

/* WIM hard-link group_id -> NTFS inode ref (the first member written). Open-
 * addressing hash; group_id is never 0 for hard-linked files, so 0 = empty. */
#define HL_BUCKETS (1u<<19)            /* 524288 slots for ~32k groups */
#define HL_MASK (HL_BUCKETS-1)
static struct { uint64_t gid, ref; } *g_hl;
static void hl_init(void){ g_hl = calloc(HL_BUCKETS, sizeof *g_hl); }
static uint64_t hl_get(uint64_t gid){
    uint32_t h=(uint32_t)(gid*0x9E3779B97F4A7C15ull>>45)&HL_MASK;
    while(g_hl[h].gid){ if(g_hl[h].gid==gid) return g_hl[h].ref; h=(h+1)&HL_MASK; }
    return 0;
}
static void hl_put(uint64_t gid, uint64_t ref){
    uint32_t h=(uint32_t)(gid*0x9E3779B97F4A7C15ull>>45)&HL_MASK;
    while(g_hl[h].gid){ if(g_hl[h].gid==gid){ g_hl[h].ref=ref; return; } h=(h+1)&HL_MASK; }
    g_hl[h].gid=gid; g_hl[h].ref=ref;
}

/* ---- parallel file-data extraction pool ----------------------------------
 * The main thread walks the WIM tree and builds ALL NTFS metadata (MFT records,
 * cluster allocations, directory indexes) single-threaded — that part is fast
 * and order-dependent. For each non-trivial file it allocates the file's run via
 * ntfs_add_file_deferred (which writes no data) and hands the worker pool a
 * {resource, target byte offset, size}. Workers decompress the WIM resource
 * (wim_read_resource is reentrant: pread + stack-local LZX state) and pwrite the
 * bytes straight to the disjoint, pre-allocated offset via blockio_pwrite. The
 * decompress+write is the entire cost of the build, so this scales with cores.
 * Cluster allocation stays sequential => the output image is layout-identical to
 * the single-threaded path. */
typedef struct { const wim_resource_t *res; uint64_t off; uint64_t size; } work_item_t;
static struct {
    work_item_t      *q;
    size_t            cap, head, count;   /* ring buffer */
    int               done;               /* producer finished submitting */
    int               nthreads;
    unsigned long     err;                /* worker failures (under mtx) */
    blockio_t        *io;
    pthread_t        *threads;
    pthread_mutex_t   mtx;
    pthread_cond_t    not_empty, not_full;
} P;

static void *worker_main(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&P.mtx);
        while(P.count==0 && !P.done) pthread_cond_wait(&P.not_empty,&P.mtx);
        if(P.count==0 && P.done){ pthread_mutex_unlock(&P.mtx); break; }
        work_item_t it = P.q[P.head];
        P.head = (P.head+1) % P.cap; P.count--;
        pthread_cond_signal(&P.not_full);
        pthread_mutex_unlock(&P.mtx);

        int fail = 0;
        uint8_t *buf = malloc(it.size ? (size_t)it.size : 1);
        if(!buf) fail = 1;
        else {
            if(wim_read_resource(W, it.res, buf) != 0) fail = 1;
            else if(blockio_pwrite(P.io, it.off, buf, (size_t)it.size) != 0) fail = 1;
            else __atomic_fetch_add(&g_written, it.size, __ATOMIC_RELAXED);  /* apply progress */
        }
        free(buf);
        if(fail){ pthread_mutex_lock(&P.mtx); P.err++; pthread_mutex_unlock(&P.mtx); }
    }
    return NULL;
}

static void pool_start(blockio_t *io, int nthreads){
    P.cap = 4096; P.q = malloc(P.cap * sizeof *P.q);
    P.head = P.count = 0; P.done = 0; P.err = 0; P.io = io; P.nthreads = nthreads;
    pthread_mutex_init(&P.mtx, NULL);
    pthread_cond_init(&P.not_empty, NULL);
    pthread_cond_init(&P.not_full, NULL);
    P.threads = malloc((size_t)nthreads * sizeof(pthread_t));
    for(int i=0;i<nthreads;i++) pthread_create(&P.threads[i], NULL, worker_main, NULL);
}

static void pool_submit(const wim_resource_t *res, uint64_t off, uint64_t size){
    pthread_mutex_lock(&P.mtx);
    while(P.count == P.cap) pthread_cond_wait(&P.not_full, &P.mtx);
    size_t tail = (P.head + P.count) % P.cap;
    P.q[tail] = (work_item_t){ res, off, size };
    P.count++;
    pthread_cond_signal(&P.not_empty);
    pthread_mutex_unlock(&P.mtx);
}

/* Signal completion, join workers, return the worker error count, free state. */
static unsigned long pool_finish(void){
    pthread_mutex_lock(&P.mtx); P.done = 1; pthread_cond_broadcast(&P.not_empty); pthread_mutex_unlock(&P.mtx);
    for(int i=0;i<P.nthreads;i++) pthread_join(P.threads[i], NULL);
    unsigned long e = P.err;
    free(P.q); free(P.threads);
    pthread_mutex_destroy(&P.mtx); pthread_cond_destroy(&P.not_empty); pthread_cond_destroy(&P.not_full);
    return e;
}

/* Progress monitor: while apply()+extract run, poll g_written and emit the apply
 * percentage (4..80 band) on each integer-% change. Single emitter -> no race with
 * the main thread, which only emits the 82..100 tail AFTER this thread is joined. */
static void *progress_monitor(void *arg){
    (void)arg;
    while(!__atomic_load_n(&g_mon_stop, __ATOMIC_RELAXED)){
        if(g_total_bytes){
            unsigned long long w = __atomic_load_n(&g_written, __ATOMIC_RELAXED);
            int p = 4 + (int)(76.0 * (double)w / (double)g_total_bytes);
            if(p<4) p=4; if(p>80) p=80;
            if(p!=g_last_pct){ g_last_pct=p; if(g_prog) g_prog(p,"Applying Windows image"); }
        }
        usleep(150000);   /* 150ms cadence */
    }
    return NULL;
}

/* Pre-walk: the EXACT byte total apply() will write, used as the progress denominator.
 * Mirrors apply()'s write decision -- sums each non-reparse, non-empty file's resource
 * orig_size, counting each hard-link group ONCE (subsequent members become NTFS hard
 * links with no data write) but every non-grouped file individually (matching the
 * g_bytes/g_written accounting). Metadata-only: decodes dentries from the in-memory
 * image tree, no decompression, no I/O. This is why we do NOT use the WIM XML
 * <TOTALBYTES> -- that is the logical size counting every hard-link member. */
static uint64_t *g_pw_seen;   /* hard-link-group seen set (HL_BUCKETS slots) */
static int pw_seen_gid(uint64_t gid){
    uint32_t h=(uint32_t)(gid*0x9E3779B97F4A7C15ull>>45)&HL_MASK;
    while(g_pw_seen[h]){ if(g_pw_seen[h]==gid) return 1; h=(h+1)&HL_MASK; }
    g_pw_seen[h]=gid; return 0;
}
static unsigned long long pw_walk(uint64_t off, int depth){
    if(depth>96) return 0;
    unsigned long long sum=0;
    for(;;){
        wim_dentry_t d; int r=wim_dentry_at(IMG,off,&d);
        if(r<=0) break;
        if(d.attributes & WIM_FILE_ATTRIBUTE_DIRECTORY){
            if(!(d.attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT) && d.subdir_offset)
                sum += pw_walk(d.subdir_offset, depth+1);
        } else if(!(d.attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT)){
            int empty=1; for(int i=0;i<20;i++) if(d.hash[i]){ empty=0; break; }
            if(!empty){
                const wim_resource_t *res=wim_lookup_by_hash(W,d.hash);
                if(res){ uint64_t gid=d.hard_link_group_id;
                    if(!gid || !pw_seen_gid(gid)) sum += res->orig_size; }
            }
        }
        off=d.self_offset+d.length;
    }
    return sum;
}

/* ---- WIM tree -> NTFS apply (validated in win2ntfs) ---- */
static int32_t shared_sid;
/* WIM security_id -> interned NTFS security_id. Built once from the WIM's
 * SECURITYDATA table so every file carries its REAL per-file descriptor
 * (indexed by the dentry's security_id). A flat SD breaks lsass/SCM/service
 * ACL checks, so each file must keep its own descriptor. */
static int32_t *g_sidmap=NULL; static uint32_t g_nsd=0;
static int32_t sid_for(const wim_dentry_t *d){
    if (d->security_id >= 0 && (uint32_t)d->security_id < g_nsd && g_sidmap[d->security_id] >= 0)
        return g_sidmap[d->security_id];
    return shared_sid;   /* security_id < 0 (none) or un-interned -> default */
}

/* MFT ref of the top-level \Windows directory, captured during apply() so the
 * post-apply injector can create \Windows\Panther\unattend.xml. */
static uint64_t g_windows_ref=0;

/* Directory path->ref map, populated during apply() and consumed by stage_manifest (both defined
 * below). Forward-declared so apply() can record dirs. */
static void dm_put(const char *path, uint64_t ref);

/* ASCII -> UTF-16LE (names are pure ASCII). Returns code-unit count. */
static int u16(const char*s, uint16_t*o){ int i=0; for(;s[i];i++) o[i]=(uint16_t)(unsigned char)s[i]; return i; }

/* Offline answer file for the generalized install.wim. Dropped at
 * \Windows\Panther\unattend.xml -- the implicit search location windeploy.exe
 * reads on first boot (after registry HKLM\System\Setup\UnattendFile), so the
 * specialize + oobeSystem passes run unattended: a local admin auto-logs on and
 * every OOBE screen (EULA, MSA, privacy, network) is skipped. processorArchitecture
 * MUST be arm64 or the components are ignored by ARM64 Windows. XML attribute
 * values use single quotes so the C literal needs no escaping. */
/* Full hands-free answer file: specialize sets
 * ComputerName + en-US locales; oobeSystem sets en-US locales (so OOBE skips the
 * region/language/keyboard screens), hides every OOBE page, creates local admin
 * user/test123, auto-logs that account on, sets TimeZone, and on first logon runs
 * a read-only chkdsk of the boot volume to C:\chkdsk_boot.txt. */
static const char UNATTEND_XML[] =
"<?xml version='1.0' encoding='utf-8'?>\n"
"<unattend xmlns='urn:schemas-microsoft-com:unattend'>\n"
"  <settings pass='specialize'>\n"
"    <component name='Microsoft-Windows-Shell-Setup' processorArchitecture='arm64' publicKeyToken='31bf3856ad364e35' language='neutral' versionScope='nonSxS' xmlns:wcm='http://schemas.microsoft.com/WMIConfig/2002/State'>\n"
"      <ComputerName>WIN11-VM</ComputerName>\n"
"    </component>\n"
"    <component name='Microsoft-Windows-International-Core' processorArchitecture='arm64' publicKeyToken='31bf3856ad364e35' language='neutral' versionScope='nonSxS' xmlns:wcm='http://schemas.microsoft.com/WMIConfig/2002/State'>\n"
"      <InputLocale>0409:00000409</InputLocale>\n"
"      <SystemLocale>en-US</SystemLocale>\n"
"      <UILanguage>en-US</UILanguage>\n"
"      <UILanguageFallback>en-US</UILanguageFallback>\n"
"      <UserLocale>en-US</UserLocale>\n"
"    </component>\n"
"  </settings>\n"
"  <settings pass='oobeSystem'>\n"
"    <component name='Microsoft-Windows-International-Core' processorArchitecture='arm64' publicKeyToken='31bf3856ad364e35' language='neutral' versionScope='nonSxS' xmlns:wcm='http://schemas.microsoft.com/WMIConfig/2002/State'>\n"
"      <InputLocale>0409:00000409</InputLocale>\n"
"      <SystemLocale>en-US</SystemLocale>\n"
"      <UILanguage>en-US</UILanguage>\n"
"      <UILanguageFallback>en-US</UILanguageFallback>\n"
"      <UserLocale>en-US</UserLocale>\n"
"    </component>\n"
"    <component name='Microsoft-Windows-Shell-Setup' processorArchitecture='arm64' publicKeyToken='31bf3856ad364e35' language='neutral' versionScope='nonSxS' xmlns:wcm='http://schemas.microsoft.com/WMIConfig/2002/State'>\n"
"      <OOBE>\n"
"        <HideEULAPage>true</HideEULAPage>\n"
"        <HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>\n"
"        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
"        <HideLocalAccountScreen>true</HideLocalAccountScreen>\n"
"        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
"        <ProtectYourPC>3</ProtectYourPC>\n"
"        <NetworkLocation>Work</NetworkLocation>\n"
"      </OOBE>\n"
"      <UserAccounts>\n"
"        <LocalAccounts>\n"
"          <LocalAccount wcm:action='add'>\n"
"            <Name>user</Name>\n"
"            <DisplayName>user</DisplayName>\n"
"            <Group>Administrators</Group>\n"
"            <Password><Value>test123</Value><PlainText>true</PlainText></Password>\n"
"          </LocalAccount>\n"
"        </LocalAccounts>\n"
"      </UserAccounts>\n"
"      <AutoLogon>\n"
"        <Username>user</Username>\n"
"        <Password><Value>test123</Value><PlainText>true</PlainText></Password>\n"
"        <Enabled>true</Enabled>\n"
"        <LogonCount>999</LogonCount>\n"
"      </AutoLogon>\n"
"      <TimeZone>Eastern Standard Time</TimeZone>\n"
"      <FirstLogonCommands>\n"
"        <SynchronousCommand wcm:action='add'>\n"
"          <Order>1</Order>\n"
"          <CommandLine>cmd /c &quot;chkdsk C: &gt; C:\\chkdsk_boot.txt 2&gt;&amp;1&quot;</CommandLine>\n"
"          <Description>read-only chkdsk of the boot volume</Description>\n"
"        </SynchronousCommand>\n"
"      </FirstLogonCommands>\n"
"    </component>\n"
"  </settings>\n"
"</unattend>\n";
/* A reparse point's tag-specific buffer is stored in the WIM as the dentry's
 * unnamed "data" resource (keyed by d.hash). Returns malloc'd bytes + *len. */
static uint8_t* read_reparse(const wim_dentry_t *d, uint64_t *len){
    const wim_resource_t *res=wim_lookup_by_hash(W,d->hash);
    if(!res){ *len=0; return NULL; }
    uint8_t *buf=malloc((size_t)res->orig_size?(size_t)res->orig_size:1);
    if(!buf){ *len=0; return NULL; }
    if(wim_read_resource(W,res,buf)!=0){ free(buf); *len=0; return NULL; }
    *len=res->orig_size; return buf;
}

static void apply(uint64_t parent_ref, uint64_t children_off, int depth, const char *ppath){
    if (depth>96) return;
    uint64_t off=children_off;
    for(;;){
        wim_dentry_t d; int r=wim_dentry_at(IMG,off,&d);
        if(r==0) break; if(r<0){ g_err++; break; }
        uint16_t nm[256]; int nc=d.file_name_nbytes/2; if(nc>255)nc=255;
        for(int i=0;i<nc;i++) nm[i]=(uint16_t)(d.file_name[2*i]|(d.file_name[2*i+1]<<8));
        /* WIM-supplied DOS 8.3 short name (present only on non-8.3 long names);
         * pass it through so the writer emits the ns=1+ns=2 pair Windows uses. */
        uint16_t sn[16]; int snc=0;
        if(d.short_name && d.short_name_nbytes){ snc=d.short_name_nbytes/2; if(snc>15)snc=15;
            for(int i=0;i<snc;i++) sn[i]=(uint16_t)(d.short_name[2*i]|(d.short_name[2*i+1]<<8)); }
        const uint16_t *snp = snc?sn:NULL;
        int32_t sid = sid_for(&d);   /* REAL per-file security descriptor */
        int is_rep = (d.attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT)!=0;
        if(d.attributes & WIM_FILE_ATTRIBUTE_DIRECTORY){
            /* Keep ALL attribute bits incl. REPARSE: the writer's to_disk_attrs
             * masks to the settable set + honors the REPARSE/dir structural bits.
             * We emit the real $REPARSE_POINT (below) so the record is consistent. */
            uint64_t ref=ntfs_add_dir(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,d.creation_time,d.last_access_time,d.last_write_time);
            if(!ref) g_err++; else { g_dirs++;
                /* Record this dir's guest path -> ref (lowercased ascii, backslash-separated, no
                 * leading slash) so stage_manifest can mkdir -p into any existing OS dir later. */
                char nameA[256]; { int j=0; for(int i=0;i<nc&&j<255;i++){ uint16_t cc=nm[i];
                    if(cc>127) nameA[j++]='?'; else { if(cc>='A'&&cc<='Z')cc+=32; nameA[j++]=(char)cc; } } nameA[j]=0; }
                char childpath[1024];
                if(ppath[0]) snprintf(childpath,sizeof childpath,"%s\\%s",ppath,nameA);
                else snprintf(childpath,sizeof childpath,"%s",nameA);
                dm_put(childpath,ref);
                if(depth==0 && !strcmp(nameA,"windows")) g_windows_ref=ref;
                if(is_rep){
                    g_reparse++;
                    uint64_t rl=0; uint8_t *rb=read_reparse(&d,&rl);
                    if(rb){ ntfs_set_reparse(NW,ref,d.reparse_tag,rb,(uint32_t)rl); free(rb); }
                    else { g_err++; printf("WARN: reparse data missing for a dir reparse point\n"); }
                    /* junction/symlink dirs carry no local children */
                } else if(d.subdir_offset) apply(ref,d.subdir_offset,depth+1,childpath);
            }
        } else if(is_rep){
            uint64_t ref=ntfs_add_file(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,d.creation_time,d.last_access_time,d.last_write_time,NULL,0);
            if(ref){ uint64_t rl=0; uint8_t *rb=read_reparse(&d,&rl);
                if(rb){ ntfs_set_reparse(NW,ref,d.reparse_tag,rb,(uint32_t)rl); free(rb); } else g_err++; }
            g_reparse++;
        } else {
            int empty=1; for(int i=0;i<20;i++) if(d.hash[i]){empty=0;break;}
            if(empty){ ntfs_add_file(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,d.creation_time,d.last_access_time,d.last_write_time,NULL,0); g_empty++; }
            else { const wim_resource_t*res=wim_lookup_by_hash(W,d.hash);
                if(!res){ g_err++; off=d.self_offset+d.length; continue; }
                if(g_boot_system && !strcmp(ppath,"windows\\system32\\config") &&
                   name_eq(d.file_name,d.file_name_nbytes,"SYSTEM")){
                    uint64_t ref=ntfs_add_file(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,
                        d.creation_time,d.last_access_time,d.last_write_time,g_boot_system,g_boot_system_len);
                    if(ref){ g_files++; g_bytes+=g_boot_system_len; g_boot_system_written++;
                        __atomic_fetch_add(&g_written,res->orig_size,__ATOMIC_RELAXED); }
                    else g_err++;
                    off=d.self_offset+d.length; continue;
                }
                /* Hard link: if this file's WIM group already has an inode on the
                 * volume, add another NAME to it (share content) instead of writing
                 * a duplicate copy -- the NTFS hard-link model. Only fall back
                 * to a fresh record (flatten) if that inode's record can't fit the
                 * name (a 40+-link file). */
                uint64_t gid = d.hard_link_group_id;
                if(gid){
                    uint64_t tref = hl_get(gid);
                    if(tref){
                        uint64_t hr=ntfs_add_hardlink(NW,tref,parent_ref,nm,nc,snp,snc,
                            d.attributes,d.creation_time,d.last_access_time,d.last_write_time,res->orig_size);
                        if(hr){ g_links++; off=d.self_offset+d.length; continue; }
                        /* record full -> flatten this link below */
                    }
                }
                /* Fresh copy. Large files (always non-resident): build the record
                 * + run now and hand the decompress+write to the worker pool.
                 * Small files (<=700B, resident-candidate): do inline — cheap,
                 * and resident data must live in the MFT record itself. */
                if(res->orig_size > 700){
                    uint64_t doff=0;
                    uint64_t ref=ntfs_add_file_deferred(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,
                        d.creation_time,d.last_access_time,d.last_write_time,res->orig_size,&doff);
                    if(ref){ pool_submit(res,doff,res->orig_size); g_files++; g_bytes+=res->orig_size;
                             if(gid && !hl_get(gid)) hl_put(gid,ref); }
                    else g_err++;
                } else {
                    uint8_t*buf=malloc((size_t)(res->orig_size?res->orig_size:1));
                    if(wim_read_resource(W,res,buf)!=0){ g_err++; free(buf); off=d.self_offset+d.length; continue; }
                    uint64_t ref=ntfs_add_file(NW,parent_ref,nm,nc,snp,snc,d.attributes,sid,d.creation_time,d.last_access_time,d.last_write_time,buf,res->orig_size);
                    g_bytes+=res->orig_size; __atomic_fetch_add(&g_written,res->orig_size,__ATOMIC_RELAXED); free(buf);
                    if(ref){ g_files++; if(gid && !hl_get(gid)) hl_put(gid,ref); } else g_err++;
                } }
        }
        if((g_files+g_dirs)%20000==0) printf("  ... %lu dirs, %lu files, %.1f GiB\n",g_dirs,g_files,g_bytes/1073741824.0);
        off=d.self_offset+d.length;
    }
}

/* ---- WIM path resolve + read ---- */
static int name_eq(const uint8_t*nm,int nb,const char*a){ int n=nb/2; if((int)strlen(a)!=n)return 0;
    for(int i=0;i<n;i++){ unsigned c=nm[2*i]|(nm[2*i+1]<<8); char x=a[i]; if(c>='a'&&c<='z')c-=32; if(x>='a'&&x<='z')x-=32; if(c!=(unsigned)x)return 0; } return 1; }
static int wim_resolve(const char*path, wim_dentry_t*out){
    wim_dentry_t root; wim_dentry_at(IMG,wim_image_root_offset(IMG),&root);
    uint64_t ch=root.subdir_offset; const char*p=path; wim_dentry_t cur; int have=0;
    while(*p){ while(*p=='\\'||*p=='/')p++; if(!*p)break; const char*c=p; int cl=0; while(*p&&*p!='\\'&&*p!='/'){p++;cl++;}
        char comp[256]; if(cl>255)cl=255; memcpy(comp,c,cl); comp[cl]=0;
        int found=0; uint64_t o=ch;
        for(;;){ wim_dentry_t d; int r=wim_dentry_at(IMG,o,&d); if(r<=0)break;
            if(name_eq(d.file_name,d.file_name_nbytes,comp)){ cur=d; found=1; break; } o=d.self_offset+d.length; }
        if(!found) return 0; have=1; ch=cur.subdir_offset; }
    if(!have)return 0; *out=cur; return 1;
}
static uint8_t* wim_read_path(const char*path, uint64_t*len){
    wim_dentry_t d; if(!wim_resolve(path,&d)) return NULL;
    const wim_resource_t*res=wim_lookup_by_hash(W,d.hash); if(!res) return NULL;
    uint8_t*buf=malloc((size_t)res->orig_size); if(wim_read_resource(W,res,buf)!=0){free(buf);return NULL;}
    *len=res->orig_size; return buf;
}
/* pick the boot locale: prefer en-US, else first \Windows\Boot\EFI\<dir> with bootmgfw.efi.mui */
static int detect_locale(char*out,int cap){
    char probe[128];
    snprintf(probe,sizeof probe,"\\Windows\\Boot\\EFI\\en-US\\bootmgfw.efi.mui");
    wim_dentry_t d; if(wim_resolve(probe,&d)){ snprintf(out,cap,"en-US"); return 1; }
    wim_dentry_t efi; if(!wim_resolve("\\Windows\\Boot\\EFI",&efi)) return 0;
    uint64_t o=efi.subdir_offset;
    for(;;){ wim_dentry_t c; int r=wim_dentry_at(IMG,o,&c); if(r<=0)break;
        if(c.attributes&WIM_FILE_ATTRIBUTE_DIRECTORY){ int nc=c.file_name_nbytes/2; char nm[64]; int j=0;
            for(int i=0;i<nc&&j<63;i++) nm[j++]=(char)(c.file_name[2*i]|(c.file_name[2*i+1]<<8)); nm[j]=0;
            snprintf(probe,sizeof probe,"\\Windows\\Boot\\EFI\\%s\\bootmgfw.efi.mui",nm);
            if(wim_resolve(probe,&d)){ snprintf(out,cap,"%s",nm); return 1; } }
        o=c.self_offset+c.length; }
    return 0;
}

/* ---- Generic guest-file staging (from-scratch create, mirrors iso-patch.exe --stage) ---- *
 * After the WIM apply we stage our own files at REAL guest paths via a manifest of
 * "<host-src>\t<guest-dest>" lines (same layout as generate_vhdx_manifest on Windows). The
 * from-scratch NTFS writer has no path lookup, so during apply() we record every directory's
 * guest path -> MFT ref in g_dirmap; stage_manifest then mkdir -p's each dest's parents from that
 * map (creating only the missing components, e.g. \Windows\Setup\Scripts under the existing
 * \Windows\Setup) and writes the file at the leaf. No DISM, no mount, no shortcuts. */
static uint64_t g_ts_c, g_ts_a, g_ts_w;   /* timestamps for staged dirs/files (from the WIM root) */

/* path -> MFT ref map. Keys are lowercased, backslash-separated, NO leading slash ("" = volume
 * root, "windows", "windows\\setup"). Open-addressing hash over strdup'd keys. */
#define DM_BUCKETS (1u<<20)
#define DM_MASK (DM_BUCKETS-1)
static struct { char *path; uint64_t ref; } *g_dirmap;
static void dm_init(void){ g_dirmap = calloc(DM_BUCKETS, sizeof *g_dirmap); }
static uint32_t dm_hash(const char *s){ uint32_t h=2166136261u; for(;*s;s++){ h^=(unsigned char)*s; h*=16777619u; } return h&DM_MASK; }
static void dm_put(const char *path, uint64_t ref){
    if(!g_dirmap) return;
    uint32_t h=dm_hash(path);
    while(g_dirmap[h].path){ if(!strcmp(g_dirmap[h].path,path)){ g_dirmap[h].ref=ref; return; } h=(h+1)&DM_MASK; }
    g_dirmap[h].path=strdup(path); g_dirmap[h].ref=ref;
}
static uint64_t dm_get(const char *path){
    if(!g_dirmap) return 0;
    uint32_t h=dm_hash(path);
    while(g_dirmap[h].path){ if(!strcmp(g_dirmap[h].path,path)) return g_dirmap[h].ref; h=(h+1)&DM_MASK; }
    return 0;
}

static uint8_t *read_host_file(const char *path, uint64_t *len){
    FILE *f=fopen(path,"rb"); if(!f){ *len=0; return NULL; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<0){ fclose(f); *len=0; return NULL; }
    uint8_t *b=malloc((size_t)(n?n:1));
    if(b && n>0 && fread(b,1,(size_t)n,f)!=(size_t)n){ free(b); b=NULL; n=0; }
    fclose(f); *len=(uint64_t)n; return b;
}

/* Security descriptor for the directories we CREATE for staging (\Windows\AppSandbox, \Setup\Scripts,
 * \Panther). Our default `shared_sid` is PROTECTED + non-inheritable, so files created INSIDE these
 * dirs at runtime (e.g. setup.log, which SetupComplete writes as SYSTEM) inherit nothing and fall back
 * to SYSTEM's default DACL = SYSTEM:Full, Administrators:READ-ONLY. That breaks FirstLogon's setup.cmd
 * (append to setup.log -> Access denied -> agent never installs). This SD has INHERITABLE (OICI) ACEs
 * granting SYSTEM + Administrators Full and CREATOR OWNER Full (inherit-only) + Users RX, matching a
 * normal \Windows subdir, so runtime-created children are admin-writable.
 * SDDL: O:BAG:SYD:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU) */
static int32_t g_dir_sid;
static uint32_t asb_build_dir_sd(uint8_t *b){
    static const uint8_t SID_SY[12]={1,1,0,0,0,0,0,5, 18,0,0,0};            /* S-1-5-18  SYSTEM */
    static const uint8_t SID_BA[16]={1,2,0,0,0,0,0,5, 32,0,0,0, 0x20,2,0,0};/* S-1-5-32-544 Admins */
    static const uint8_t SID_CO[12]={1,1,0,0,0,0,0,3, 0,0,0,0};             /* S-1-3-0  CREATOR OWNER */
    static const uint8_t SID_BU[16]={1,2,0,0,0,0,0,5, 32,0,0,0, 0x21,2,0,0};/* S-1-5-32-545 Users */
    uint32_t p=20;                       /* leave room for the 20-byte self-relative header */
    uint32_t dacl_off=p, acl_start=p;
    b[p]=2; b[p+1]=0; p+=8;              /* ACL: rev=2, sbz1=0; size/count/sbz2 filled below */
    uint16_t n=0;
    #define ACE(flags,mask,sid,sl) do{ uint16_t sz=8+(sl); b[p]=0; b[p+1]=(flags); \
        b[p+2]=sz&0xff;b[p+3]=sz>>8; uint32_t m=(mask); b[p+4]=m;b[p+5]=m>>8;b[p+6]=m>>16;b[p+7]=m>>24; \
        memcpy(b+p+8,(sid),(sl)); p+=sz; n++; }while(0)
    ACE(0x03,0x001F01FF,SID_SY,12);      /* OICI    SYSTEM Full */
    ACE(0x03,0x001F01FF,SID_BA,16);      /* OICI    Administrators Full */
    ACE(0x0B,0x001F01FF,SID_CO,12);      /* OICIIO  CREATOR OWNER Full (inherit-only) */
    ACE(0x03,0x001200A9,SID_BU,16);      /* OICI    Users Read+Execute */
    #undef ACE
    uint16_t acl_sz=(uint16_t)(p-acl_start);
    b[acl_start+2]=acl_sz&0xff; b[acl_start+3]=acl_sz>>8; b[acl_start+4]=n&0xff; b[acl_start+5]=n>>8;
    b[acl_start+6]=0; b[acl_start+7]=0;
    uint32_t owner_off=p; memcpy(b+p,SID_BA,16); p+=16;
    uint32_t group_off=p; memcpy(b+p,SID_SY,12); p+=12;
    b[0]=1; b[1]=0; b[2]=0x04; b[3]=0x80;          /* rev=1, control=SE_SELF_RELATIVE|SE_DACL_PRESENT */
    #define PUT32(o,v) do{ b[o]=(v);b[o+1]=(v)>>8;b[o+2]=(v)>>16;b[o+3]=(v)>>24; }while(0)
    PUT32(4,owner_off); PUT32(8,group_off); PUT32(12,0); PUT32(16,dacl_off);
    #undef PUT32
    return p;
}

/* Ensure all components of a normalized dir path exist; return the leaf dir ref (0 on failure).
 * `norm` is lowercased, backslash-separated, no leading slash. Creates only missing components. */
static uint64_t mkdir_p(const char *norm){
    if(!norm[0]) return dm_get("");           /* volume root */
    uint64_t exist = dm_get(norm);
    if(exist) return exist;
    char acc[1024]; acc[0]=0;
    uint64_t parent = dm_get("");
    const char *p=norm;
    while(*p){
        char comp[256]; int cl=0;
        while(*p && *p!='\\' && cl<255) comp[cl++]=*p++;
        comp[cl]=0;
        while(*p=='\\') p++;
        if(!cl) continue;
        /* cumulative path */
        if(acc[0]) { size_t n=strlen(acc); snprintf(acc+n,sizeof acc-n,"\\%s",comp); }
        else snprintf(acc,sizeof acc,"%s",comp);
        uint64_t r = dm_get(acc);
        if(!r){
            uint16_t n16[256]; int nl=u16(comp,n16); if(nl>255)nl=255;
            r = ntfs_add_dir(NW,parent,n16,nl,NULL,0,0x10,g_dir_sid,g_ts_c,g_ts_a,g_ts_w);
            if(!r) return 0;
            dm_put(acc,r);
        }
        parent=r;
    }
    return parent;
}

/* Split "<src>\t<dest>" lines; for each, mkdir -p the dest's parent and write the file at the leaf.
 * dest uses Windows '\' separators relative to the volume root. Returns staged count; *errs bumped. */
static unsigned long stage_manifest(const char *manifest_path, unsigned long *errs){
    FILE *mf=fopen(manifest_path,"r"); if(!mf){ printf("stage: cannot open manifest %s\n",manifest_path); (*errs)++; return 0; }
    char line[4096]; unsigned long staged=0;
    while(fgets(line,sizeof line,mf)){
        char *nl=strpbrk(line,"\r\n"); if(nl)*nl=0;
        if(!line[0]||line[0]=='#') continue;
        char *tab=strchr(line,'\t'); if(!tab){ continue; }
        *tab=0; const char *src=line; const char *dest=tab+1;
        if(!*src||!*dest) continue;
        /* normalize dest, split into parent + leaf */
        char nd[1024]; { const char *q=dest; while(*q=='\\'||*q=='/')q++; size_t o=0;
            for(; *q && o+1<sizeof nd; q++){ char c=*q; if(c=='/')c='\\'; if(c>='A'&&c<='Z')c+=32; nd[o++]=c; } nd[o]=0; }
        char *ls=strrchr(nd,'\\');
        char parent[1024]; char leafl[256];
        if(ls){ size_t pl=(size_t)(ls-nd); if(pl>=sizeof parent)pl=sizeof parent-1; memcpy(parent,nd,pl); parent[pl]=0; snprintf(leafl,sizeof leafl,"%s",ls+1); }
        else { parent[0]=0; snprintf(leafl,sizeof leafl,"%s",nd); }
        /* leaf name with ORIGINAL case from dest (preserve case for the on-disk name) */
        const char *od=dest; while(*od=='\\'||*od=='/')od++;
        const char *ols=strrchr(od,'\\'); const char *ols2=strrchr(od,'/');
        if(ols2 && (!ols||ols2>ols)) ols=ols2;
        const char *leaf = ols?ols+1:od;
        uint64_t pref = mkdir_p(parent);
        if(!pref){ printf("stage: mkdir_p failed for %s\n",parent); (*errs)++; continue; }
        uint64_t len=0; uint8_t *data=read_host_file(src,&len);
        /* read_host_file returns NULL on any real failure (missing/unreadable/short read/OOM);
         * a genuinely empty 0-byte file returns non-NULL with len==0. So NULL => error out,
         * rather than silently staging a missing source as an empty file. */
        if(!data){ printf("stage: read failed %s\n",src); (*errs)++; continue; }
        char lo[256]; snprintf(lo,sizeof lo,"%s",leaf);   /* bound the leaf so u16's UTF-16 expansion can't overflow n16[256] */
        uint16_t n16[256]; int nlc=u16(lo,n16); if(nlc>255)nlc=255;
        uint64_t fr=ntfs_add_file(NW,pref,n16,nlc,NULL,0,0x20,shared_sid,g_ts_c,g_ts_a,g_ts_w,(const char*)data,len);
        free(data);
        if(fr){ staged++; printf("  staged %s (%llu bytes)\n",dest,(unsigned long long)len); }
        else { printf("stage: add_file failed %s\n",dest); (*errs)++; }
    }
    fclose(mf);
    return staged;
}

int win_disk_build(const char *wimp, const char *imgp, uint32_t idx, uint64_t gib,
                   const char *manifest_path,
                   const char *vioscsi_dir,
                   void (*progress)(int,const char*)){
    setvbuf(stdout,NULL,_IONBF,0);
    if(!wimp || !imgp || (vioscsi_dir && !manifest_path)) return 2;
    if(idx==0) idx=1;
    if(gib==0) gib=34;
    #define PROG(p,m) do{ if(progress) progress((p),(m)); }while(0)

    W=wim_open(wimp);
    if(!W){ printf("wim open fail\n"); return 2; }
    IMG=wim_image_open(W,idx);
    if(!IMG){ printf("wim image open fail\n"); return 2; }
    g_boot_system=NULL; g_boot_system_len=0; g_boot_system_written=0;
    if(vioscsi_dir){
        uint32_t driver_index=0;
        char published_inf[32], path[64];
        for(;;){
            snprintf(published_inf,sizeof published_inf,"oem%u.inf",driver_index);
            snprintf(path,sizeof path,"\\Windows\\INF\\%s",published_inf);
            wim_dentry_t entry;
            if(!wim_resolve(path,&entry)) break;
            if(driver_index==UINT32_MAX) return 2;
            driver_index++;
        }
        uint64_t len=0;
        uint8_t *system=wim_read_path("\\Windows\\System32\\config\\SYSTEM",&len);
        int result=system ? windows_boot_vioscsi(system,(size_t)len,published_inf,
                                               &g_boot_system,&g_boot_system_len) : -1;
        free(system);
        if(result){ printf("Cannot prepare the Windows SYSTEM hive for VirtIO SCSI boot\n"); return 2; }
        FILE *mf=fopen(manifest_path,"a");
        if(!mf){ free(g_boot_system); g_boot_system=NULL; return 2; }
        int written=fprintf(mf,"%s/vioscsi.inf\t\\Windows\\INF\\%s\n"
            "%s/vioscsi.cat\t\\Windows\\System32\\CatRoot\\{F750E6C3-38EE-11D1-85E5-00C04FC295EE}\\oem%u.cat\n",
            vioscsi_dir,published_inf,vioscsi_dir,driver_index);
        int closed=fclose(mf);
        if(written<0 || closed){ free(g_boot_system); g_boot_system=NULL; return 2; }
    }

    uint64_t bytes=gib<<30;
    blockio_t*io=blockio_create(imgp,bytes); if(!io){printf("image create fail\n");return 2;}

    /* 1) GPT */
    gpt_layout_t L;
    if(gpt_write_windows(io,512,260,16,&L)!=0){ printf("gpt fail\n"); return 1; }
    printf("GPT: ESP @%llu (%llu MiB), MSR @%llu, Windows @%llu (%.1f GiB)\n",
        (unsigned long long)L.esp_start_lba,(unsigned long long)L.esp_count_lba/2048,
        (unsigned long long)L.msr_start_lba,(unsigned long long)L.win_start_lba,L.win_count_lba/2097152.0);

    /* 2) NTFS apply */
    NW=ntfs_writer_open(io,L.win_start_lba,L.win_count_lba,"Windows");
    shared_sid=ntfs_secure_intern(NW,NULL,0);
    { uint8_t dirsd[256]; uint32_t dl=asb_build_dir_sd(dirsd);
      g_dir_sid=ntfs_secure_intern(NW,dirsd,dl);
      if(g_dir_sid<0) g_dir_sid=shared_sid;   /* fall back if intern failed */
      printf("staged-dir SD interned (inheritable Admins:F): sid=%d\n", g_dir_sid); }
    /* Pre-intern the WIM's REAL per-file security descriptors (each inode keeps
     * its own descriptor, indexed by security_id). ntfs_secure_intern dedups
     * identical descriptors, so the volume's $Secure ends up with the WIM's
     * ~hundreds of distinct ACLs, each file referencing the correct one —
     * instead of one flat over-permissive SD. */
    g_nsd = wim_image_sd_count(IMG);
    g_sidmap = malloc(g_nsd ? g_nsd*sizeof(int32_t) : 1);
    uint32_t sd_ok=0;
    for(uint32_t i=0;i<g_nsd;i++){
        uint64_t sdl=0; const uint8_t *sdb=wim_image_sd(IMG,(int32_t)i,&sdl);
        g_sidmap[i] = (sdb && sdl) ? ntfs_secure_intern(NW,sdb,(uint32_t)sdl) : -1;
        if(g_sidmap[i]>=0) sd_ok++;
    }
    printf("interned %u/%u WIM per-file security descriptors\n", sd_ok, g_nsd);
    uint64_t root=ntfs_root_ref(NW);
    wim_dentry_t r; wim_dentry_at(IMG,wim_image_root_offset(IMG),&r);
    ntfs_set_root_security(NW, sid_for(&r));   /* root dir gets the WIM root's real SD */
    hl_init();                                 /* hard-link group -> inode map */
    dm_init();                                 /* dir path -> ref map (for stage_manifest mkdir -p) */
    dm_put("", root);                          /* volume root */
    g_ts_c=r.creation_time; g_ts_a=r.last_access_time; g_ts_w=r.last_write_time;  /* staged file times */
    int nthreads=0; { const char*e=getenv("NTFS_THREADS"); if(e) nthreads=atoi(e);
        if(nthreads<=0){ long n=sysconf(_SC_NPROCESSORS_ONLN); nthreads=(n>0)?(int)n:4; }
        if(nthreads>64) nthreads=64; }
    pool_start(io, nthreads);
    printf("applying image %u to NTFS ... (root sec_id from WIM, %d extract threads)\n",idx,nthreads);
    /* Apply-progress denominator: the EXACT bytes apply() will write, from a metadata-only
     * pre-walk (pw_walk) that mirrors apply's hard-link dedup -- so the 4..80 bar reaches 80
     * exactly when the last byte lands. (The WIM XML <TOTALBYTES> is the logical size counting
     * every hard-link member, ~2.5x too big on WinSxS, which stalls the bar near 40%.) The
     * monitor runs across apply()+extract; it is joined BEFORE the 82..100 tail. */
    g_prog = progress; g_last_pct = -1; g_mon_stop = 0;
    __atomic_store_n(&g_written, 0ULL, __ATOMIC_RELAXED);
    g_pw_seen = calloc(HL_BUCKETS, sizeof *g_pw_seen);
    g_total_bytes = g_pw_seen ? pw_walk(r.subdir_offset, 0) : 0;   /* 0 => monitor off (no regression) */
    free(g_pw_seen); g_pw_seen = NULL;
    pthread_t mon = 0; int mon_ok = (g_total_bytes && pthread_create(&mon,NULL,progress_monitor,NULL)==0);
    double T_apply0 = now_s();
    apply(root,r.subdir_offset,0,"");
    unsigned long perr = pool_finish();        /* drain + join workers before any close */
    free(g_boot_system); g_boot_system=NULL;
    if(vioscsi_dir && g_boot_system_written!=1) g_err++;
    if(mon_ok){ __atomic_store_n(&g_mon_stop, 1, __ATOMIC_RELAXED); pthread_join(mon,NULL); }
    double T_apply1 = now_s();
    printf("PHASE apply+extract (parallel) = %.1fs\n", T_apply1 - T_apply0);
    g_err += perr;
    if(perr) printf("WARNING: %lu file(s) failed to extract in worker threads\n",perr);
    printf("applied: %lu dirs, %lu files, %lu hardlinks, %lu reparse, %lu err, %.2f GiB\n",g_dirs,g_files,g_links,g_reparse,g_err,g_bytes/1073741824.0);

    /* Stage our own files (answer file, agent + channel EXEs, test-signed drivers, setup scripts)
     * at their real guest paths via the manifest -- mirrors iso-patch.exe --stage on Windows, but
     * writing straight into our from-scratch NTFS (no DISM/mount). mkdir_p uses the dir map built
     * during apply, so e.g. \Windows\Setup\Scripts is created under the existing \Windows\Setup.
     * No manifest (standalone smoke test) -> inject only the built-in answer file. */
    PROG(82,"Staging guest files");
    if(manifest_path){
        unsigned long se=0; unsigned long ns=stage_manifest(manifest_path,&se);
        printf("staged %lu file(s) from manifest, %lu error(s)\n",ns,se);
        g_err += se;
    } else if(g_windows_ref){
        uint16_t pn[16]; int pl=u16("Panther",pn);
        uint64_t panther=ntfs_add_dir(NW,g_windows_ref,pn,pl,NULL,0,0x10,shared_sid,
                                      r.creation_time,r.last_access_time,r.last_write_time);
        if(panther){
            uint16_t un[16]; int ul=u16("unattend.xml",un);
            uint64_t uref=ntfs_add_file(NW,panther,un,ul,NULL,0,0x20,shared_sid,
                                        r.creation_time,r.last_access_time,r.last_write_time,
                                        UNATTEND_XML,sizeof(UNATTEND_XML)-1);
            printf("injected built-in \\Windows\\Panther\\unattend.xml (%zu bytes)%s\n",
                   sizeof(UNATTEND_XML)-1, uref?"":" [FILE ADD FAILED]");
        } else printf("WARN: could not create \\Windows\\Panther; unattend NOT injected\n");
    } else printf("WARN: \\Windows dir not found during apply; unattend NOT injected\n");

    PROG(90,"Finalizing filesystem");
    double T_close0 = now_s();
    if(ntfs_writer_close(NW)!=0){ printf("ntfs close FAIL\n"); return 1; }
    printf("PHASE ntfs_writer_close (serial) = %.1fs\n", now_s() - T_close0);
    printf("NTFS done.\n");
    PROG(95,"Writing boot files");

    /* 3) FAT32 ESP */
    fat32_writer_t*F=fat32_open(io,L.esp_start_lba,L.esp_count_lba,"ESP");
    if(!F){ printf("fat32 open fail\n"); return 1; }
    fat32_mkdir(F,"/EFI/Boot");
    fat32_mkdir(F,"/EFI/Microsoft/Boot");

    /* 4) boot files from install.wim (per-ISO) */
    uint64_t bm_len,mui_len,tpl_len;
    uint8_t*bootmgfw=wim_read_path("\\Windows\\Boot\\EFI\\bootmgfw.efi",&bm_len);
    if(!bootmgfw){ printf("bootmgfw.efi not found in image\n"); return 1; }
    char locale[32]="en-US"; detect_locale(locale,sizeof locale);
    char muipath[160]; snprintf(muipath,sizeof muipath,"\\Windows\\Boot\\EFI\\%s\\bootmgfw.efi.mui",locale);
    uint8_t*mui=wim_read_path(muipath,&mui_len);
    uint8_t*tpl=wim_read_path("\\Windows\\System32\\config\\BCD-Template",&tpl_len);
    if(!tpl){ printf("BCD-Template not found\n"); return 1; }
    printf("boot files: bootmgfw.efi=%llu  mui(%s)=%llu  BCD-Template=%llu\n",
        (unsigned long long)bm_len,locale,(unsigned long long)(mui?mui_len:0),(unsigned long long)tpl_len);

    /* 5) write ESP files (a failed boot-file write makes the disk unbootable) */
    int eerr=0;
    eerr |= fat32_addfile(F,"/EFI/Boot/bootaa64.efi",bootmgfw,bm_len);
    eerr |= fat32_addfile(F,"/EFI/Microsoft/Boot/bootmgfw.efi",bootmgfw,bm_len);
    if(mui){ char dst[160]; snprintf(dst,sizeof dst,"/EFI/Microsoft/Boot/%s/bootmgfw.efi.mui",locale);
        char dir[160]; snprintf(dir,sizeof dir,"/EFI/Microsoft/Boot/%s",locale); fat32_mkdir(F,dir);
        eerr |= fat32_addfile(F,dst,mui,mui_len); }

    /* 6) patched BCD */
    bcd_guids_t g; memcpy(g.disk_guid,L.disk_guid,16); memcpy(g.esp_part_guid,L.esp_part_guid,16); memcpy(g.win_part_guid,L.win_part_guid,16);
    uint8_t*bcd; size_t bcd_len;
    if(bcd_build(tpl,(size_t)tpl_len,&g,&bcd,&bcd_len)!=0){ printf("bcd_build fail\n"); return 1; }
    eerr |= fat32_addfile(F,"/EFI/Microsoft/Boot/BCD",bcd,bcd_len);
    printf("BCD built (%zu bytes) + ESP populated.\n",bcd_len);
    if(eerr){ printf("ESP boot-file write FAILED\n"); return 1; }

    if(fat32_close(F)!=0){ printf("fat32 close fail\n"); return 1; }
    PROG(98,"Configuring boot");
    blockio_close(io);
    /* Any apply/extract/stage error (g_err includes worker perr + staging se) means a file is
     * missing or corrupt on the volume -> fail rather than report a bootable disk. */
    if(g_err){ printf("\nDISK INCOMPLETE: %lu file error(s) during build: %s\n",g_err,imgp); return 1; }
    PROG(100,"Disk complete");
    printf("\nDISK COMPLETE: %s (%llu GiB image)\n",imgp,(unsigned long long)gib);
    return 0;
    #undef PROG
}

#ifdef WIN_DISK_STANDALONE
int main(int argc,char**argv){
    if(argc<3){ printf("usage: win_disk <install.wim> <out.img> [image_idx] [disk_gib] [manifest]\n"); return 2; }
    uint32_t idx = argc>3?(uint32_t)atoi(argv[3]):1;
    uint64_t gib = argc>4?(uint64_t)atoll(argv[4]):34;
    const char *manifest = argc>5?argv[5]:NULL;
    return win_disk_build(argv[1],argv[2],idx,gib,manifest,NULL,NULL);
}
#endif
